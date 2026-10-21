/* Probe for the built-in gateway (runtime/gateway.c), without the game: starts the server on a free loopback port,
   publishes fake position JSON and checks the HTTP/SSE contract and the discovery file.
   make -C games/totk probe-gateway */
#include "gateway.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

static long long now_ms(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts) == 0 ? (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 : 0;
}

/* Sends one GET and reads the whole response (the server closes non-SSE connections). */
static int http_get(int port, const char *path, char *out, size_t capacity) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) { close(fd); return -1; }
    char request[256];
    int asked = snprintf(request, sizeof request, "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    if (asked <= 0 || send(fd, request, (size_t)asked, 0) != asked) { close(fd); return -1; }
    size_t used = 0;
    for (;;) {
        ssize_t n = recv(fd, out + used, capacity - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= capacity - 1) break;
    }
    out[used] = 0;
    close(fd);
    return (int)used;
}

static void publish(const char *json) { runtime_gateway_publish(json, strlen(json)); }

int main(void) {
    char directory[] = "/tmp/gateway-probe-XXXXXX";
    if (!mkdtemp(directory)) { perror("mkdtemp"); return 2; }
    char port_start[16];
    snprintf(port_start, sizeof port_start, "%d", 19000 + (int)(getpid() % 500));
    setenv("TOTK_ROMFS_PROOF_DIR", directory, 1);
    setenv("TOTK_GATEWAY_PORT", port_start, 1);

    runtime_gateway_start();
    int port = runtime_gateway_port();
    check(port > 0, "gateway starts on a loopback port");

    char body[8192];
    publish("{\"schema\":\"macnativegames.maplink/1\",\"ok\":false,\"inWorld\":false,\"game\":\"totk\","
            "\"version\":\"1.4.3\",\"frame\":1,\"t\":0,\"inWorld\":false}");
    char json[256];
    snprintf(json, sizeof json, "{\"schema\":\"macnativegames.maplink/1\",\"ok\":true,\"inWorld\":true,"
                                "\"game\":\"totk\",\"version\":\"1.4.3\",\"frame\":10,\"t\":%lld,"
                                "\"x\":1.5,\"y\":2.5,\"z\":3.5,\"yaw\":90.0,\"headingSource\":\"actor\",\"layer\":null}",
             now_ms());
    publish(json);

    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"live\"") != NULL,
          "/v1/state reports live");
    check(strstr(body, "\"x\":1.5") != NULL, "/v1/state carries the published position");
    check(http_get(port, "/v1/health", body, sizeof body) > 0 && strstr(body, "200 OK") != NULL, "/v1/health is 200");
    check(http_get(port, "/v1/meta", body, sizeof body) > 0 && strstr(body, "gameapi/1") != NULL, "/v1/meta reports the api");
    check(http_get(port, "/nope", body, sizeof body) > 0 && strstr(body, "404") != NULL, "unknown path is 404");

    char discovery[512];
    snprintf(discovery, sizeof discovery, "%s/gateway.json", directory);
    FILE *f = fopen(discovery, "r");
    check(f != NULL, "discovery file exists");
    if (f) {
        char text[512] = {0};
        if (fread(text, 1, sizeof text - 1, f) == 0) text[0] = 0;
        fclose(f);
        check(strstr(text, "\"url\":\"http://127.0.0.1:") != NULL, "discovery file has the url");
    }

    snprintf(json, sizeof json, "{\"schema\":\"macnativegames.maplink/1\",\"ok\":true,\"inWorld\":true,"
                                "\"version\":\"1.4.3\",\"frame\":11,\"t\":%lld,\"x\":1.0,\"y\":2.0,\"z\":3.0}",
             now_ms() - 5000);
    publish(json);
    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"stale\"") != NULL,
          "/v1/state reports stale after 5 s");

    snprintf(json, sizeof json, "{\"schema\":\"macnativegames.maplink/1\",\"ok\":false,\"inWorld\":false,"
                                "\"version\":\"1.4.3\",\"frame\":12,\"t\":%lld}", now_ms());
    publish(json);
    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"not-in-world\"") != NULL,
          "/v1/state reports not-in-world");

    /* SSE: the first two events arrive immediately. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    int sse_ok = 0;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) {
        const char *request = "GET /v1/stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
        if (send(fd, request, strlen(request), 0) > 0) {
            char buffer[8192];
            size_t used = 0;
            buffer[0] = 0;
            while (used < sizeof buffer - 1) {
                ssize_t n = recv(fd, buffer + used, sizeof buffer - 1 - used, 0);
                if (n <= 0) break;
                used += (size_t)n;
                buffer[used] = 0;
                if (strstr(buffer, "event: hello") && strstr(buffer, "event: state")) { sse_ok = 1; break; }
            }
        }
    }
    close(fd);
    check(sse_ok, "SSE sends hello and state");

    runtime_gateway_stop();
    check(access(discovery, F_OK) != 0, "stop removes the discovery file");

    printf(failures ? "FAILED %d check(s)\n" : "PASS gateway contract\n", failures);
    return failures ? 1 : 0;
}
