/* Probe for the built-in gateway (runtime/gateway.c), without the game: starts the server on a free loopback port,
   publishes fake position JSON and checks the position, trail and save domains and the discovery file.
   make -C games/totk probe-gateway */
#include "gateway.h"
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", what); failures++; }
    else printf("ok   %s\n", what);
}

static long long now_ms(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts) == 0 ? (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 : 0;
}

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
    char request[512];
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

static int count_of(const char *text, const char *needle) {
    int count = 0;
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) { count++; p++; }
    return count;
}

static void publish(const char *json) { runtime_gateway_publish(json, strlen(json)); }

static void position_json(char *out, size_t capacity, int in_world, long long t, double x, double z, double yaw) {
    snprintf(out, capacity,
             "{\"schema\":\"macnativegames.maplink/1\",\"ok\":%s,\"inWorld\":%s,\"game\":\"totk\","
             "\"version\":\"1.4.3\",\"frame\":1,\"t\":%lld,\"x\":%.3f,\"y\":10.0,\"z\":%.3f,"
             "\"yaw\":%.1f,\"headingSource\":\"actor\",\"layer\":null}",
             in_world ? "true" : "false", in_world ? "true" : "false", t, x, z, yaw);
}

static void put_u32(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value & 0xff);
    p[1] = (unsigned char)((value >> 8) & 0xff);
    p[2] = (unsigned char)((value >> 16) & 0xff);
    p[3] = (unsigned char)((value >> 24) & 0xff);
}

static void put_f32(unsigned char *p, float value) { memcpy(p, &value, sizeof value); }

/* A minimal progress.sav: magic, the two position tags and an end tag, with plausible float vectors. */
static void write_fake_save(const char *path) {
    static unsigned char data[0x12000];
    memset(data, 0, sizeof data);
    data[0] = 4; data[1] = 3; data[2] = 2; data[3] = 1;
    put_u32(data + 0x28, 0x8773F02Du); put_u32(data + 0x2c, 0x10000u);
    put_u32(data + 0x30, 0xC884818Du); put_u32(data + 0x34, 0x11000u);
    put_u32(data + 0x38, 0xA3DB7114u); put_u32(data + 0x3c, 0u);
    put_f32(data + 0x10000, 10.0f); put_f32(data + 0x10004, 20.0f); put_f32(data + 0x10008, 30.0f);
    put_f32(data + 0x11000, 1.5f); put_f32(data + 0x11004, 2.5f); put_f32(data + 0x11008, 3.5f);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, sizeof data, f);
    fclose(f);
}

int main(void) {
    char directory[] = "/tmp/gateway-probe-XXXXXX";
    if (!mkdtemp(directory)) { perror("mkdtemp"); return 2; }
    char profile[PATH_MAX], saves[PATH_MAX], hash[PATH_MAX], slot[PATH_MAX], save_path[PATH_MAX];
    snprintf(profile, sizeof profile, "%s/profile", directory);
    snprintf(saves, sizeof saves, "%s/save", profile);
    snprintf(hash, sizeof hash, "%s/hash", saves);
    snprintf(slot, sizeof slot, "%s/slot_00", hash);
    mkdir(profile, 0755); mkdir(saves, 0755); mkdir(hash, 0755); mkdir(slot, 0755);
    snprintf(save_path, sizeof save_path, "%s/progress.sav", slot);
    write_fake_save(save_path);

    char port_start[16];
    snprintf(port_start, sizeof port_start, "%d", 19000 + (int)(getpid() % 500));
    setenv("TOTK_ROMFS_PROOF_DIR", directory, 1);
    setenv("TOTK_PROFILE_DIR", profile, 1);
    setenv("TOTK_GATEWAY_PORT", port_start, 1);

    runtime_gateway_start();
    int port = runtime_gateway_port();
    check(port > 0, "gateway starts on a loopback port");

    char body[65536], json[512];
    long long t = now_ms();
    position_json(json, sizeof json, 1, t, 1.5, 3.5, 90.0);
    publish(json);
    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"live\"") != NULL,
          "/v1/state reports live");
    check(strstr(body, "\"x\":1.5") != NULL, "/v1/state carries the published position");
    check(http_get(port, "/v1/health", body, sizeof body) > 0 && strstr(body, "200 OK") != NULL, "/v1/health is 200");
    check(http_get(port, "/v1/meta", body, sizeof body) > 0 && strstr(body, "gameapi/1") != NULL, "/v1/meta reports the api");
    check(strstr(body, "\"trail\":{\"schema\":\"macnativegames.trail/1\",\"available\":true") != NULL, "meta advertises trail");
    check(strstr(body, "\"save\":{\"schema\":\"macnativegames.save/1\",\"summary\":true") != NULL, "meta advertises save");
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

    /* town: 5 s old is stale, not-in-world is reported */
    position_json(json, sizeof json, 1, now_ms() - 5000, 1.0, 3.0, 0.0);
    publish(json);
    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"stale\"") != NULL,
          "/v1/state reports stale after 5 s");
    position_json(json, sizeof json, 0, now_ms(), 0.0, 0.0, 0.0);
    publish(json);
    check(http_get(port, "/v1/state", body, sizeof body) > 0 && strstr(body, "\"status\":\"not-in-world\"") != NULL,
          "/v1/state reports not-in-world");

    /* trail: two samples 0.6 s apart, then a 2.1 s pause (gap), all with real increasing timestamps */
    usleep(600000);
    position_json(json, sizeof json, 1, now_ms(), 10.0, 20.0, 0.0); publish(json);
    usleep(600000);
    position_json(json, sizeof json, 1, now_ms(), 11.0, 21.0, 10.0); publish(json);
    usleep(2100000);
    position_json(json, sizeof json, 1, now_ms(), 12.0, 22.0, 20.0); publish(json);
    check(http_get(port, "/v1/trail?seconds=600&max=600", body, sizeof body) > 0 &&
          count_of(body, "\"t\":") >= 3, "/v1/trail returns the recorded points");
    check(strstr(body, "\"gap\":true") != NULL, "/v1/trail marks a gap after a pause");
    check(http_get(port, "/v1/trail?seconds=1", body, sizeof body) > 0 &&
          count_of(body, "\"t\":") == 1, "/v1/trail honours the time window");
    check(http_get(port, "/v1/trail?seconds=600&max=2", body, sizeof body) > 0 &&
          count_of(body, "\"t\":") == 2 && strstr(body, "\"truncated\":true") != NULL,
          "/v1/trail honours max and flags truncation");
    int before_menu = http_get(port, "/v1/trail?seconds=600&max=600", body, sizeof body) > 0 ? count_of(body, "\"t\":") : -1;
    position_json(json, sizeof json, 0, now_ms(), 0.0, 0.0, 0.0); publish(json);
    check(http_get(port, "/v1/trail?seconds=600&max=600", body, sizeof body) > 0 &&
          count_of(body, "\"t\":") == before_menu, "/v1/trail does not record menu frames");

    /* save: synthetic progress.sav with the position tags */
    check(http_get(port, "/v1/save/slots", body, sizeof body) > 0 && strstr(body, "\"slot\":\"00\"") != NULL,
          "/v1/save/slots lists slot 00");
    check(http_get(port, "/v1/save/summary?slot=latest", body, sizeof body) > 0 &&
          strstr(body, "\"x\":1.500") != NULL && strstr(body, "\"y\":2.500") != NULL,
          "/v1/save/summary parses the player position");
    check(http_get(port, "/v1/save/summary?slot=99", body, sizeof body) > 0 && strstr(body, "no-save") != NULL,
          "/v1/save/summary reports an unknown slot");
    check(http_get(port, "/v1/save/summary?slot=latest", body, sizeof body) > 0 && strstr(body, "\"savedAt\":\"20") != NULL,
          "/v1/save/summary reports savedAt");

    /* SSE: hello and state arrive immediately */
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
