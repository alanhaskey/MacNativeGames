/* Built-in local gateway: the game itself serves the read-only contract on 127.0.0.1, so the player never starts a
   command and no Python is needed at runtime. It starts with the game (after the game patches are installed), serves:

       GET /v1/meta      version, rate, capabilities
       GET /v1/state     the latest published snapshot + status/ageMs
       GET /v1/stream    Server-Sent Events: hello, state, status, heartbeat
       GET /v1/health    200 while live, otherwise 503

   and stops when the process exits. Loopback only; TOTK_GATEWAY=0 disables it; TOTK_GATEWAY_PORT sets the first port
   tried (17862..17871, the first free one wins). The actual URL is published in <data>/gateway.json so external
   programs never guess. See docs/game-interface.md. */
#include "gateway.h"
#include "game_version.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GW_SCHEMA "macnativegames.maplink/1"
#define GW_DEFAULT_PORT 17862
#define GW_PORT_TRIES 10
#define GW_MAX_SNAPSHOT 4096
#define GW_MAX_CLIENTS 32

#ifdef GAME_BOTW
#define GW_GAME "botw"
#else
#define GW_GAME "totk"
#endif

static pthread_mutex_t gw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gw_cond = PTHREAD_COND_INITIALIZER;
static char gw_snapshot[GW_MAX_SNAPSHOT];
static size_t gw_snapshot_len;
static uint64_t gw_seq;
static atomic_int gw_running;
static int gw_listen_fd = -1;
static int gw_port;
static atomic_int gw_sse_clients;
static char gw_discovery[PATH_MAX];

static long long now_ms(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts) == 0 ? (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 : 0;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int find_ll(const char *json, const char *key, long long *out) {
    const char *p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    char *end = NULL;
    long long value = strtoll(p, &end, 10);
    if (end == p) return 0;
    *out = value;
    return 1;
}

/* The published snapshot is the maplink JSON line. status/ageMs are computed here, from t and ok, so a stopped game
   becomes stale/disconnected instead of a frozen "live". */
static void build_state(char *out, size_t capacity) {
    pthread_mutex_lock(&gw_lock);
    char snap[GW_MAX_SNAPSHOT];
    size_t length = gw_snapshot_len;
    if (length >= sizeof snap) length = sizeof snap - 1;
    memcpy(snap, gw_snapshot, length);
    snap[length] = 0;
    pthread_mutex_unlock(&gw_lock);

    while (length && (snap[length - 1] == '\n' || snap[length - 1] == '\r' || snap[length - 1] == ' ')) snap[--length] = 0;
    if (!length) {
        snprintf(out, capacity, "{\"schema\":\"" GW_SCHEMA "\",\"ok\":false,\"inWorld\":false,"
                                "\"status\":\"disconnected\",\"ageMs\":null,\"reason\":\"starting\"}");
        return;
    }
    long long t = 0;
    int has_t = find_ll(snap, "\"t\":", &t);
    int ok = strstr(snap, "\"ok\":true") != NULL;
    long long age = has_t ? now_ms() - t : -1;
    if (age < 0) age = 0;
    const char *status;
    const char *reason = NULL;
    if (!has_t) {
        status = "disconnected";
        reason = "producer-invalid";
    } else if (age > 10000) {
        status = "disconnected";
        reason = "producer-silent";
    } else if (age > 1000) {
        status = "stale";
    } else {
        status = ok ? "live" : "not-in-world";
    }
    const char *close = strrchr(snap, '}');
    size_t prefix = close ? (size_t)(close - snap) : length;
    if (has_t) {
        snprintf(out, capacity, "%.*s,\"status\":\"%s\",\"ageMs\":%lld%s%s}", (int)prefix, snap, status, age,
                 reason ? ",\"reason\":\"" : "", reason ? reason : "");
    } else {
        snprintf(out, capacity, "%.*s,\"status\":\"%s\",\"ageMs\":null%s%s}", (int)prefix, snap, status,
                 reason ? ",\"reason\":\"" : "", reason ? reason : "");
    }
}

static void respond(int fd, int code, const char *ctype, const char *body) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : (code == 503 ? "Service Unavailable" : "Not Found"), ctype,
                     strlen(body));
    if (n > 0 && send_all(fd, head, (size_t)n)) send_all(fd, body, strlen(body));
}

static int event_send(int fd, const char *name, const char *data) {
    char head[64];
    int n = snprintf(head, sizeof head, "event: %s\ndata: ", name);
    if (n <= 0 || !send_all(fd, head, (size_t)n)) return 0;
    if (!send_all(fd, data, strlen(data))) return 0;
    return send_all(fd, "\n\n", 2);
}

static void stream_client(int fd) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                     "Cache-Control: no-cache\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");
    if (n <= 0 || !send_all(fd, head, (size_t)n)) return;
    if (!event_send(fd, "hello", "{\"schema\":\"" GW_SCHEMA "\",\"bridge\":\"" TOTK_GAME_VERSION "\",\"rateHz\":10}")) return;
    char state[GW_MAX_SNAPSHOT + 256];
    build_state(state, sizeof state);
    if (!event_send(fd, "state", state)) return;
    char last_status[32];
    snprintf(last_status, sizeof last_status, "%s", strstr(state, "\"status\":\"") ? strstr(state, "\"status\":\"") + 10 : "unknown");
    char *quote = strchr(last_status, '"');
    if (quote) *quote = 0;
    uint64_t seen = 0;
    pthread_mutex_lock(&gw_lock);
    seen = gw_seq;
    pthread_mutex_unlock(&gw_lock);
    time_t last_ping = time(NULL);
    while (atomic_load(&gw_running)) {
        uint64_t seq;
        pthread_mutex_lock(&gw_lock);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        pthread_cond_timedwait(&gw_cond, &gw_lock, &deadline);
        seq = gw_seq;
        pthread_mutex_unlock(&gw_lock);
        if (seq != seen) {
            seen = seq;
            build_state(state, sizeof state);
            char status[32] = "unknown";
            const char *found = strstr(state, "\"status\":\"");
            if (found) {
                snprintf(status, sizeof status, "%s", found + 10);
                char *end = strchr(status, '"');
                if (end) *end = 0;
            }
            if (strcmp(status, last_status) != 0) {
                snprintf(last_status, sizeof last_status, "%s", status);
                char payload[128];
                snprintf(payload, sizeof payload, "{\"status\":\"%s\"}", status);
                if (!event_send(fd, "status", payload)) break;
            }
            if (!event_send(fd, "state", state)) break;
        }
        if (time(NULL) - last_ping >= 15) {
            last_ping = time(NULL);
            if (!send_all(fd, ": ping\n\n", 8)) break;
        }
    }
}

static void *client_thread(void *argument) {
    int fd = (int)(intptr_t)argument;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    atomic_fetch_add(&gw_sse_clients, 1);
    char request[8192];
    size_t used = 0;
    int ready = 0;
    while (used < sizeof request - 1) {
        ssize_t n = recv(fd, request + used, sizeof request - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n")) { ready = 1; break; }
    }
    if (ready) {
        char method[8] = {0}, target[256] = {0};
        if (sscanf(request, "%7s %255s", method, target) == 2 && strcmp(method, "GET") == 0) {
            char *query = strchr(target, '?');
            if (query) *query = 0;
            if (!strcmp(target, "/v1/stream")) {
                stream_client(fd);
                close(fd);
                return NULL;
            }
            char body[GW_MAX_SNAPSHOT + 256];
            if (!strcmp(target, "/v1/state")) {
                build_state(body, sizeof body);
                respond(fd, 200, "application/json; charset=utf-8", body);
            } else if (!strcmp(target, "/v1/health")) {
                build_state(body, sizeof body);
                int live = strstr(body, "\"status\":\"live\"") != NULL;
                respond(fd, live ? 200 : 503, "application/json; charset=utf-8",
                        live ? "{\"ok\":true,\"status\":\"live\"}" : "{\"ok\":false,\"status\":\"down\"}");
            } else if (!strcmp(target, "/v1/meta")) {
                snprintf(body, sizeof body,
                         "{\"api\":\"gameapi/1\",\"bridge\":\"%s\",\"builtin\":true,\"producer\":\"maplink\","
                         "\"rateHz\":10,\"game\":\"%s\",\"version\":\"%s\",\"port\":%d,"
                         "\"capabilities\":{\"position\":{\"schema\":\"" GW_SCHEMA "\",\"available\":true,\"rateHz\":10},"
                         "\"save\":{\"summary\":false},\"trail\":{\"available\":false},\"control\":{\"available\":false}}}",
                         TOTK_GAME_VERSION, GW_GAME, TOTK_GAME_VERSION, gw_port);
                respond(fd, 200, "application/json; charset=utf-8", body);
            } else {
                respond(fd, 404, "application/json; charset=utf-8",
                        "{\"error\":{\"code\":\"not-found\"},\"endpoints\":[\"/v1/meta\",\"/v1/state\",\"/v1/stream\",\"/v1/health\"]}");
            }
        } else {
            respond(fd, 404, "application/json; charset=utf-8", "{\"error\":{\"code\":\"not-found\"}}");
        }
    }
    close(fd);
    atomic_fetch_sub(&gw_sse_clients, 1);
    return NULL;
}

static void *accept_thread(void *unused) {
    (void)unused;
    while (atomic_load(&gw_running)) {
        int fd = accept(gw_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (atomic_load(&gw_sse_clients) >= GW_MAX_CLIENTS) { close(fd); continue; }
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, (void *)(intptr_t)fd) != 0) { close(fd); continue; }
        pthread_detach(thread);
    }
    return NULL;
}

static void write_discovery(const char *directory, int port) {
    if (!directory || !*directory) return;
    snprintf(gw_discovery, sizeof gw_discovery, "%s/gateway.json", directory);
    char temporary[PATH_MAX + 8];
    snprintf(temporary, sizeof temporary, "%s.tmp", gw_discovery);
    FILE *f = fopen(temporary, "w");
    if (!f) return;
    fprintf(f, "{\"schema\":\"macnativegames.gateway/1\",\"url\":\"http://127.0.0.1:%d\",\"port\":%d,"
               "\"pid\":%d,\"game\":\"%s\",\"version\":\"%s\",\"startedAt\":%lld}\n",
            port, port, (int)getpid(), GW_GAME, TOTK_GAME_VERSION, now_ms());
    if (fclose(f) == 0) rename(temporary, gw_discovery);
    else remove(temporary);
}

void runtime_gateway_start(void) {
    const char *off = getenv("TOTK_GATEWAY");
    if (off && (!strcmp(off, "0") || !strcmp(off, "off"))) {
        fprintf(stderr, "[gateway] disabled by TOTK_GATEWAY\n");
        return;
    }
    const char *port_env = getenv("TOTK_GATEWAY_PORT");
    int first = port_env ? atoi(port_env) : GW_DEFAULT_PORT;
    if (first < 1024 || first > 65000) first = GW_DEFAULT_PORT;
    signal(SIGPIPE, SIG_IGN);

    int fd = -1, port = 0;
    for (int i = 0; i < GW_PORT_TRIES; i++) {
        port = first + i;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        struct sockaddr_in address;
        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&address, sizeof address) == 0 && listen(fd, 16) == 0) break;
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        fprintf(stderr, "[gateway] no free loopback port in %d..%d; interface unavailable\n", first, first + GW_PORT_TRIES - 1);
        return;
    }
    gw_listen_fd = fd;
    gw_port = port;
    atomic_store(&gw_running, 1);
    const char *directory = getenv("TOTK_ROMFS_PROOF_DIR");
    if (!directory || !*directory) directory = getenv("TOTK_PROFILE_DIR");
    write_discovery(directory, port);
    pthread_t thread;
    if (pthread_create(&thread, NULL, accept_thread, NULL) != 0) {
        fprintf(stderr, "[gateway] cannot start the listener thread\n");
        close(fd);
        gw_listen_fd = -1;
        atomic_store(&gw_running, 0);
        return;
    }
    pthread_detach(thread);
    atexit(runtime_gateway_stop);
    if (gw_discovery[0]) fprintf(stderr, "[gateway] listening on http://127.0.0.1:%d (discovery: %s)\n", port, gw_discovery);
    else fprintf(stderr, "[gateway] listening on http://127.0.0.1:%d\n", port);
}

void runtime_gateway_stop(void) {
    if (!atomic_exchange(&gw_running, 0)) return;
    if (gw_listen_fd >= 0) {
        shutdown(gw_listen_fd, SHUT_RDWR);
        close(gw_listen_fd);
        gw_listen_fd = -1;
    }
    pthread_mutex_lock(&gw_lock);
    pthread_cond_broadcast(&gw_cond);
    pthread_mutex_unlock(&gw_lock);
    if (gw_discovery[0]) remove(gw_discovery);
    gw_discovery[0] = 0;
}

int runtime_gateway_publish(const char *json, size_t length) {
    if (!atomic_load(&gw_running) || !json || !length) return 0;
    if (length >= GW_MAX_SNAPSHOT) length = GW_MAX_SNAPSHOT - 1;
    pthread_mutex_lock(&gw_lock);
    memcpy(gw_snapshot, json, length);
    gw_snapshot[length] = 0;
    gw_snapshot_len = length;
    gw_seq++;
    pthread_cond_broadcast(&gw_cond);
    pthread_mutex_unlock(&gw_lock);
    return 1;
}

int runtime_gateway_port(void) { return gw_port; }
