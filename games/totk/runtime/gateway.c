/* Built-in local gateway: the game itself serves the read-only contract on 127.0.0.1, so the player never starts a
   command and no Python is needed at runtime. It starts with the game (after the game patches are installed) and stops
   when the process exits. Endpoints:

       GET /v1/meta                       version, rate, capabilities
       GET /v1/state                      latest published snapshot + status/ageMs
       GET /v1/stream                     SSE: hello, state, status, heartbeat
       GET /v1/health                     200 while live, otherwise 503
       GET /v1/trail?seconds=&max=        recent path (in-memory ring buffer, 2 Hz, 10 minutes)
       GET /v1/save/slots                 save slots (slot, savedAt, size)
       GET /v1/save/summary?slot=latest   last save position/time (read-only parse of progress.sav)

   Loopback only; TOTK_GATEWAY=0 disables it; TOTK_GATEWAY_PORT sets the first port tried (17862..17871, first free
   wins). The actual URL is published in <data>/gateway.json. See docs/game-interface.md and docs/next-features-plan.md. */
#include "gateway.h"
#include "game_version.h"
#include <arpa/inet.h>
#include <dirent.h>
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
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GW_SCHEMA "macnativegames.maplink/1"
#define GW_DEFAULT_PORT 17862
#define GW_PORT_TRIES 10
#define GW_MAX_SNAPSHOT 4096
#define GW_MAX_CLIENTS 32
#define GW_TRAIL_CAPACITY 2400
#define GW_TRAIL_INTERVAL_MS 500
#define GW_TRAIL_GAP_MS 2000
#define GW_TRAIL_MAX_ITEM 176
#define GW_SAVE_MAX_SLOTS 16
#define GW_SAVE_READ_LIMIT (8 * 1024 * 1024)

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
static atomic_int gw_clients;
static char gw_discovery[PATH_MAX];

typedef struct {
    long long t;
    float x, z, yaw;
    unsigned char gap, has_yaw;
} TrailPoint;
static TrailPoint gw_trail[GW_TRAIL_CAPACITY];
static size_t gw_trail_head, gw_trail_count;
static long long gw_trail_last_t;

static char gw_save_cache_path[PATH_MAX];
static long long gw_save_cache_mtime, gw_save_cache_size;
static double gw_save_cache_xyz[3];

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

static int find_double(const char *json, const char *key, double *out) {
    const char *p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    if (strncmp(p, "null", 4) == 0) return 0;
    char *end = NULL;
    double value = strtod(p, &end);
    if (end == p) return 0;
    *out = value;
    return 1;
}

static void iso_time(long long epoch_ms, char *out, size_t capacity) {
    time_t seconds = (time_t)(epoch_ms / 1000);
    struct tm tm;
    if (gmtime_r(&seconds, &tm)) strftime(out, capacity, "%Y-%m-%dT%H:%M:%SZ", &tm);
    else snprintf(out, capacity, "1970-01-01T00:00:00Z");
}

static int query_int(const char *query, const char *key, int fallback, int minimum, int maximum) {
    if (!query) return fallback;
    char pattern[32];
    snprintf(pattern, sizeof pattern, "%s=", key);
    const char *p = strstr(query, pattern);
    if (!p) return fallback;
    p += strlen(pattern);
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) return fallback;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return (int)value;
}

static void trail_record(const char *json) {
    long long t;
    double x, z, yaw = 0;
    if (!find_ll(json, "\"t\":", &t) || !find_double(json, "\"x\":", &x) || !find_double(json, "\"z\":", &z)) return;
    if (!strstr(json, "\"ok\":true") || !strstr(json, "\"inWorld\":true")) return;
    int has_yaw = find_double(json, "\"yaw\":", &yaw);
    pthread_mutex_lock(&gw_lock);
    if (gw_trail_last_t && t <= gw_trail_last_t) { pthread_mutex_unlock(&gw_lock); return; }
    if (gw_trail_last_t && t - gw_trail_last_t < GW_TRAIL_INTERVAL_MS) { pthread_mutex_unlock(&gw_lock); return; }
    if (gw_trail_count == GW_TRAIL_CAPACITY) {
        gw_trail_head = (gw_trail_head + 1) % GW_TRAIL_CAPACITY;
        gw_trail_count--;
    }
    size_t index = (gw_trail_head + gw_trail_count) % GW_TRAIL_CAPACITY;
    TrailPoint *point = &gw_trail[index];
    point->t = t;
    point->x = (float)x;
    point->z = (float)z;
    point->yaw = (float)yaw;
    point->has_yaw = (unsigned char)has_yaw;
    point->gap = (unsigned char)(gw_trail_last_t && t - gw_trail_last_t > GW_TRAIL_GAP_MS);
    gw_trail_count++;
    gw_trail_last_t = t;
    pthread_mutex_unlock(&gw_lock);
}

/* Caller frees; NULL on allocation failure. */
static char *trail_json(const char *query) {
    int seconds = query_int(query, "seconds", 120, 1, 600);
    int maximum = query_int(query, "max", 600, 1, 6000);
    long long since = now_ms() - (long long)seconds * 1000;
    size_t capacity = (size_t)GW_TRAIL_CAPACITY * GW_TRAIL_MAX_ITEM + 512;
    char *out = malloc(capacity);
    if (!out) return NULL;
    size_t used = (size_t)snprintf(out, capacity, "{\"schema\":\"macnativegames.trail/1\",\"source\":\"gateway\","
                                                  "\"seconds\":%d,\"points\":[", seconds);
    int emitted = 0, truncated = 0;
    pthread_mutex_lock(&gw_lock);
    for (size_t i = 0; i < gw_trail_count; i++) {
        const TrailPoint *point = &gw_trail[(gw_trail_head + i) % GW_TRAIL_CAPACITY];
        if (point->t < since) continue;
        if (emitted >= maximum) { truncated = 1; continue; }
        char item[GW_TRAIL_MAX_ITEM];
        int n = point->has_yaw
            ? snprintf(item, sizeof item, "{\"t\":%lld,\"x\":%.3f,\"z\":%.3f,\"yaw\":%.1f%s}",
                       point->t, (double)point->x, (double)point->z, (double)point->yaw, point->gap ? ",\"gap\":true" : "")
            : snprintf(item, sizeof item, "{\"t\":%lld,\"x\":%.3f,\"z\":%.3f%s}",
                       point->t, (double)point->x, (double)point->z, point->gap ? ",\"gap\":true" : "");
        if (n <= 0 || used + (size_t)n + 64 >= capacity) { truncated = 1; continue; }
        if (emitted) out[used++] = ',';
        memcpy(out + used, item, (size_t)n);
        used += (size_t)n;
        emitted++;
    }
    pthread_mutex_unlock(&gw_lock);
    used += (size_t)snprintf(out + used, capacity - used, "],\"truncated\":%s}", truncated ? "true" : "false");
    return out;
}

typedef struct {
    char slot[16];
    char path[PATH_MAX];
    long long mtime;
    long long size;
} SaveEntry;

static const char *profile_dir(void) {
    const char *p = getenv("TOTK_PROFILE_DIR");
    return (p && *p) ? p : NULL;
}

static int collect_saves(SaveEntry *out, int maximum) {
    const char *profile = profile_dir();
    if (!profile) return 0;
    char saves[PATH_MAX];
    snprintf(saves, sizeof saves, "%s/save", profile);
    DIR *hashes = opendir(saves);
    if (!hashes) return 0;
    int count = 0;
    struct dirent *hash;
    while ((hash = readdir(hashes)) && count < maximum) {
        if (hash->d_name[0] == '.') continue;
        char directory[PATH_MAX];
        snprintf(directory, sizeof directory, "%s/%s", saves, hash->d_name);
        DIR *slots = opendir(directory);
        if (!slots) continue;
        struct dirent *slot;
        while ((slot = readdir(slots)) && count < maximum) {
            if (strncmp(slot->d_name, "slot_", 5) != 0) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s/progress.sav", directory, slot->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(out[count].slot, sizeof out[count].slot, "%s", slot->d_name + 5);
            snprintf(out[count].path, sizeof out[count].path, "%s", path);
            out[count].mtime = (long long)st.st_mtime * 1000 + (long long)(st.st_mtimespec.tv_nsec / 1000000);
            out[count].size = (long long)st.st_size;
            count++;
        }
        closedir(slots);
    }
    closedir(hashes);
    for (int i = 1; i < count; i++) {
        SaveEntry key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].mtime < key.mtime) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return count;
}

/* 0: parsed, -1: not a save, -2: file changed while reading. */
static int parse_save_position(const char *path, double xyz[3]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat before, after;
    if (fstat(fileno(f), &before) != 0 || (long long)before.st_size > GW_SAVE_READ_LIMIT) { fclose(f); return -1; }
    unsigned char *data = malloc((size_t)before.st_size);
    if (!data) { fclose(f); return -1; }
    size_t got = fread(data, 1, (size_t)before.st_size, f);
    fclose(f);
    if (got != (size_t)before.st_size) { free(data); return -1; }
    if (stat(path, &after) != 0 || after.st_size != before.st_size || after.st_mtime != before.st_mtime) {
        free(data);
        return -2;
    }
    if (got < 0x30 || data[0] != 0x04 || data[1] != 0x03 || data[2] != 0x02 || data[3] != 0x01) { free(data); return -1; }
    const unsigned POSITION_HASH = 0xC884818Du, WORLD_POSITION_HASH = 0x8773F02Du, END_HASH = 0xA3DB7114u;
    int found = 0, have_world = 0, have_player = 0;
    double world[3] = {0, 0, 0}, player[3] = {0, 0, 0};
    for (size_t i = 0x28; i + 8 <= got; i += 8) {
        unsigned tag = (unsigned)data[i] | ((unsigned)data[i + 1] << 8) | ((unsigned)data[i + 2] << 16) | ((unsigned)data[i + 3] << 24);
        unsigned value = (unsigned)data[i + 4] | ((unsigned)data[i + 5] << 8) | ((unsigned)data[i + 6] << 16) | ((unsigned)data[i + 7] << 24);
        if (tag == END_HASH) break;
        if ((tag != POSITION_HASH && tag != WORLD_POSITION_HASH) || value < 0x10000 || (value & 3u) || (size_t)value + 12 > got) continue;
        float xyz3[3];
        memcpy(xyz3, data + value, sizeof xyz3);
        if (!(xyz3[0] == xyz3[0] && xyz3[1] == xyz3[1] && xyz3[2] == xyz3[2])) continue;
        if (tag == WORLD_POSITION_HASH) { world[0] = xyz3[0]; world[1] = xyz3[1]; world[2] = xyz3[2]; have_world = 1; }
        else { player[0] = xyz3[0]; player[1] = xyz3[1]; player[2] = xyz3[2]; have_player = 1; }
        found = 1;
    }
    free(data);
    if (!found) return -1;
    if (have_player) { xyz[0] = player[0]; xyz[1] = player[1]; xyz[2] = player[2]; }
    else { xyz[0] = world[0]; xyz[1] = world[1]; xyz[2] = world[2]; }
    (void)have_world;
    return 0;
}

static char *save_slots_json(void) {
    SaveEntry entries[GW_SAVE_MAX_SLOTS];
    int count = collect_saves(entries, GW_SAVE_MAX_SLOTS);
    size_t capacity = 512 + (size_t)GW_SAVE_MAX_SLOTS * 128;
    char *out = malloc(capacity);
    if (!out) return NULL;
    size_t used = (size_t)snprintf(out, capacity, "{\"schema\":\"macnativegames.save/1\",\"slots\":[");
    for (int i = 0; i < count; i++) {
        char stamp[32];
        iso_time(entries[i].mtime, stamp, sizeof stamp);
        if (i) out[used++] = ',';
        used += (size_t)snprintf(out + used, capacity - used,
                                 "{\"slot\":\"%s\",\"savedAt\":\"%s\",\"size\":%lld}", entries[i].slot, stamp, entries[i].size);
    }
    snprintf(out + used, capacity - used, "]}");
    return out;
}

static char *save_summary_json(const char *query) {
    SaveEntry entries[GW_SAVE_MAX_SLOTS];
    int count = collect_saves(entries, GW_SAVE_MAX_SLOTS);
    char wanted[16] = "latest";
    if (query) {
        const char *p = strstr(query, "slot=");
        if (p) {
            p += 5;
            size_t n = strcspn(p, "&");
            if (n > 0 && n < sizeof wanted) { memcpy(wanted, p, n); wanted[n] = 0; }
        }
    }
    SaveEntry *chosen = NULL;
    for (int i = 0; i < count; i++) {
        if (!strcmp(wanted, "latest") || !strcmp(wanted, entries[i].slot)) { chosen = &entries[i]; break; }
    }
    char *out = malloc(1024);
    if (!out) return NULL;
    if (!chosen) {
        snprintf(out, 1024, "{\"schema\":\"macnativegames.save/1\",\"ok\":false,\"reason\":\"no-save\"}");
        return out;
    }
    double xyz[3];
    int result;
    if (strcmp(chosen->path, gw_save_cache_path) == 0 && chosen->mtime == gw_save_cache_mtime && chosen->size == gw_save_cache_size) {
        xyz[0] = gw_save_cache_xyz[0]; xyz[1] = gw_save_cache_xyz[1]; xyz[2] = gw_save_cache_xyz[2];
        result = 0;
    } else {
        result = parse_save_position(chosen->path, xyz);
        if (result == 0) {
            snprintf(gw_save_cache_path, sizeof gw_save_cache_path, "%s", chosen->path);
            gw_save_cache_mtime = chosen->mtime;
            gw_save_cache_size = chosen->size;
            gw_save_cache_xyz[0] = xyz[0]; gw_save_cache_xyz[1] = xyz[1]; gw_save_cache_xyz[2] = xyz[2];
        }
    }
    if (result == -2) {
        snprintf(out, 1024, "{\"schema\":\"macnativegames.save/1\",\"ok\":false,\"reason\":\"save-busy\"}");
        return out;
    }
    if (result != 0) {
        snprintf(out, 1024, "{\"schema\":\"macnativegames.save/1\",\"ok\":false,\"reason\":\"unreadable\"}");
        return out;
    }
    char stamp[32];
    iso_time(chosen->mtime, stamp, sizeof stamp);
    snprintf(out, 1024,
             "{\"schema\":\"macnativegames.save/1\",\"ok\":true,\"game\":\"%s\",\"version\":\"%s\","
             "\"slot\":\"%s\",\"savedAt\":\"%s\","
             "\"player\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
             "\"optional\":{\"playtimeSeconds\":null,\"hearts\":null,\"stamina\":null,\"rupees\":null}}",
             GW_GAME, TOTK_GAME_VERSION, chosen->slot, stamp, xyz[0], xyz[1], xyz[2]);
    return out;
}

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
    if (!has_t) { status = "disconnected"; reason = "producer-invalid"; }
    else if (age > 10000) { status = "disconnected"; reason = "producer-silent"; }
    else if (age > 1000) status = "stale";
    else status = ok ? "live" : "not-in-world";
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
                     code, code == 200 ? "OK" : (code == 409 ? "Conflict" : (code == 503 ? "Service Unavailable" : "Not Found")),
                     ctype, strlen(body));
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
    char last_status[32] = "unknown";
    const char *found = strstr(state, "\"status\":\"");
    if (found) {
        snprintf(last_status, sizeof last_status, "%s", found + 10);
        char *quote = strchr(last_status, '"');
        if (quote) *quote = 0;
    }
    uint64_t seen;
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
            const char *where = strstr(state, "\"status\":\"");
            if (where) {
                snprintf(status, sizeof status, "%s", where + 10);
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

static void meta_json(char *body, size_t capacity) {
    snprintf(body, capacity,
             "{\"api\":\"gameapi/1\",\"bridge\":\"%s\",\"builtin\":true,\"producer\":\"maplink\","
             "\"rateHz\":10,\"game\":\"%s\",\"version\":\"%s\",\"port\":%d,"
             "\"capabilities\":{"
             "\"position\":{\"schema\":\"" GW_SCHEMA "\",\"available\":true,\"rateHz\":10},"
             "\"trail\":{\"schema\":\"macnativegames.trail/1\",\"available\":true,\"maxSeconds\":600,\"rateHz\":2},"
             "\"save\":{\"schema\":\"macnativegames.save/1\",\"summary\":true,\"slots\":true,"
             "\"fields\":[\"position\",\"savedAt\",\"slot\"]},"
             "\"control\":{\"available\":false}}}",
             TOTK_GAME_VERSION, GW_GAME, TOTK_GAME_VERSION, gw_port);
}

static void *client_thread(void *argument) {
    int fd = (int)(intptr_t)argument;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    atomic_fetch_add(&gw_clients, 1);
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
        char method[8] = {0}, target[512] = {0};
        if (sscanf(request, "%7s %511s", method, target) == 2 && strcmp(method, "GET") == 0) {
            char *query = strchr(target, '?');
            if (query) { *query = 0; query++; }
            if (!strcmp(target, "/v1/stream")) {
                stream_client(fd);
                close(fd);
                atomic_fetch_sub(&gw_clients, 1);
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
                meta_json(body, sizeof body);
                respond(fd, 200, "application/json; charset=utf-8", body);
            } else if (!strcmp(target, "/v1/trail")) {
                char *json = trail_json(query);
                if (json) { respond(fd, 200, "application/json; charset=utf-8", json); free(json); }
                else respond(fd, 503, "application/json; charset=utf-8", "{\"error\":{\"code\":\"busy\"}}");
            } else if (!strcmp(target, "/v1/save/slots")) {
                char *json = save_slots_json();
                if (json) { respond(fd, 200, "application/json; charset=utf-8", json); free(json); }
                else respond(fd, 503, "application/json; charset=utf-8", "{\"error\":{\"code\":\"busy\"}}");
            } else if (!strcmp(target, "/v1/save/summary")) {
                char *json = save_summary_json(query);
                if (!json) respond(fd, 503, "application/json; charset=utf-8", "{\"error\":{\"code\":\"busy\"}}");
                else {
                    int conflict = strstr(json, "\"save-busy\"") != NULL;
                    respond(fd, conflict ? 409 : 200, "application/json; charset=utf-8", json);
                    free(json);
                }
            } else {
                respond(fd, 404, "application/json; charset=utf-8",
                        "{\"error\":{\"code\":\"not-found\"},\"endpoints\":[\"/v1/meta\",\"/v1/state\",\"/v1/stream\","
                        "\"/v1/health\",\"/v1/trail\",\"/v1/save/slots\",\"/v1/save/summary\"]}");
            }
        } else {
            respond(fd, 404, "application/json; charset=utf-8", "{\"error\":{\"code\":\"not-found\"}}");
        }
    }
    close(fd);
    atomic_fetch_sub(&gw_clients, 1);
    return NULL;
}

static void *accept_thread(void *unused) {
    (void)unused;
    while (atomic_load(&gw_running)) {
        int fd = accept(gw_listen_fd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        if (atomic_load(&gw_clients) >= GW_MAX_CLIENTS) { close(fd); continue; }
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
    pthread_mutex_unlock(&gw_lock);
    trail_record(json);
    return 1;
}

int runtime_gateway_port(void) { return gw_port; }
