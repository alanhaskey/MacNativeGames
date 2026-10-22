/* Instant GUID collection events (production path). A background thread locates the in-memory sorted GUID
   array (the same instance ids the save stores), watches it, and publishes inserted GUIDs for the gateway's
   SSE endpoint (/v1/progress/events). Read-only; opt out with TOTK_EVENTS=0; TOTK_EVENTS_LOG=1 writes a log. */
#include "progress_events.h"
#include "game_version.h"
#include "world_survey.h"
#include <dirent.h>
#include <limits.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(GAME_BOTW)
void runtime_progress_events_start(void) { }
int progress_events_next(long long *cursor, char *json, size_t capacity) {
    (void)cursor; (void)json; (void)capacity; return 0;
}
#else
#include "mach_vm_compat.h"

#define PE_MAX_GUIDS 4096
#define PE_RING 256
#define PE_CHUNK (4u << 20)
#define PE_MAX_SCAN ((uint64_t)12 << 30)

extern void runtime_guest_regions(uintptr_t *heap_base, uint64_t *heap_bytes, uintptr_t *alias_base, uint64_t *alias_size_out);

typedef struct { long long seq, t; char json[256]; } PeEvent;
static PeEvent pe_ring[PE_RING];
static int pe_first, pe_count;
static long long pe_seq;
static pthread_mutex_t pe_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int pe_running;
static FILE *pe_log_file;

static void pe_log(const char *fmt, ...) {
    const char *on = getenv("TOTK_EVENTS_LOG");
    if (!on || !*on || !strcmp(on, "0")) return;
    if (!pe_log_file) {
        const char *profile = getenv("TOTK_PROFILE_DIR");
        if (!profile || !*profile) return;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/../events.log", profile);
        pe_log_file = fopen(path, "a");
        if (!pe_log_file) return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&ts.tv_sec));
    va_list ap;
    va_start(ap, fmt);
    fprintf(pe_log_file, "[%s.%03ld] ", stamp, ts.tv_nsec / 1000000);
    vfprintf(pe_log_file, fmt, ap);
    va_end(ap);
    fputc('\n', pe_log_file);
    fflush(pe_log_file);
}

static unsigned read_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int read_mem(uintptr_t address, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address, (mach_vm_size_t)size,
                                  (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}

static int load_save_guids(unsigned long long *out, int maximum) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (!profile || !*profile) return 0;
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/save", profile);
    DIR *hashes = opendir(root);
    if (!hashes) return 0;
    char newest[PATH_MAX] = "";
    long long best = 0;
    struct dirent *hash;
    while ((hash = readdir(hashes))) {
        if (hash->d_name[0] == '.') continue;
        char directory[PATH_MAX];
        snprintf(directory, sizeof directory, "%s/%s", root, hash->d_name);
        DIR *slots = opendir(directory);
        if (!slots) continue;
        struct dirent *slot;
        while ((slot = readdir(slots))) {
            if (strncmp(slot->d_name, "slot_", 5) != 0) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s/progress.sav", directory, slot->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            long long stamp = (long long)st.st_mtime * 1000000000LL + st.st_mtimespec.tv_nsec;
            if (stamp > best) { best = stamp; snprintf(newest, sizeof newest, "%s", path); }
        }
        closedir(slots);
    }
    closedir(hashes);
    if (!best) return 0;
    FILE *f = fopen(newest, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x10000 || size > (16 << 20)) { fclose(f); return 0; }
    unsigned char *data = malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(data); return 0; }
    unsigned guids_offset = 0;
    for (size_t i = 0x28; i + 8 <= got; i += 8) {
        if (read_u32(data + i) == 0xA3DB7114u) { guids_offset = read_u32(data + i + 4); break; }
    }
    int count = 0;
    if (guids_offset >= 0x10000 && guids_offset < got) {
        for (size_t i = guids_offset; i + 8 <= got && count < maximum; i += 8) {
            unsigned lo = read_u32(data + i), up = read_u32(data + i + 4);
            if (lo == 0 && up == 0) break;
            out[count++] = ((unsigned long long)up << 32) | lo;
        }
    }
    free(data);
    return count;
}

typedef struct { uintptr_t start; uint64_t length; } PeArray;

static int pe_contains(const unsigned long long *values, uint64_t count, unsigned long long needle) {
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (values[mid] == needle) return 1;
        if (values[mid] < needle) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static PeArray locate_guid_array(const unsigned long long *guids, int count) {
    PeArray none = {0, 0};
    if (count < 8) return none;
    unsigned char needle[16];
    int middle = count / 2;
    memcpy(needle, &guids[middle], 8);
    memcpy(needle + 8, &guids[middle + 1], 8);
    uintptr_t heap = 0, alias = 0;
    uint64_t heap_size = 0, alias_size = 0;
    runtime_guest_regions(&heap, &heap_size, &alias, &alias_size);
    uintptr_t regions[2] = {heap, alias};
    uint64_t sizes[2] = {heap_size, alias_size};
    unsigned char *buffer = malloc(PE_CHUNK);
    if (!buffer) return none;
    PeArray found = {0, 0};
    uint64_t scanned = 0;
    for (int r = 0; r < 2 && !found.start; r++) {
        for (uint64_t off = 0; off < sizes[r] && !found.start && scanned < PE_MAX_SCAN; off += PE_CHUNK) {
            size_t want = (size_t)((sizes[r] - off) < PE_CHUNK ? (sizes[r] - off) : PE_CHUNK);
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(regions[r] + off), (mach_vm_size_t)want,
                                       (mach_vm_address_t)(uintptr_t)buffer, &got) != KERN_SUCCESS || got == 0)
                continue;
            scanned += got;
            for (size_t i = 0; i + 16 <= (size_t)got; i += 8) {
                if (memcmp(buffer + i, needle, 16) != 0) continue;
                uintptr_t hit = regions[r] + off + i;
                uintptr_t start = hit;
                unsigned long long current = 0, previous = 0;
                if (!read_mem(start, &current, 8)) continue;
                for (int k = 0; k < 4096; k++) {
                    if (!read_mem(start - 8, &previous, 8)) break;
                    if (previous >= current) break;
                    start -= 8;
                    current = previous;
                }
                uint64_t length = 1;
                previous = current;
                for (int k = 0; k < 8192; k++) {
                    unsigned long long next = 0;
                    if (!read_mem(start + (uintptr_t)length * 8, &next, 8)) break;
                    if (next <= previous) break;
                    previous = next;
                    length++;
                }
                if (length < (uint64_t)count / 2) continue;
                int samples = 0, present = 0;
                for (int k = 0; k < 5; k++) {
                    int index = (int)((uint64_t)(count - 1) * (uint64_t)k / 4);
                    if (index >= count) index = count - 1;
                    unsigned long long value = 0;
                    if (!read_mem(start + (uintptr_t)index * 8, &value, 8)) continue;
                    samples++;
                    if (pe_contains(guids, (uint64_t)count, value)) present++;
                }
                if (samples >= 3 && present >= 3) { found.start = start; found.length = length; break; }
            }
        }
    }
    free(buffer);
    return found;
}

static void publish_guid(unsigned long long guid) {
    float xyz[3] = {0, 0, 0};
    int have = runtime_survey_player(xyz, NULL);
    PeEvent event;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    event.t = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    snprintf(event.json, sizeof event.json,
             "{\"schema\":\"macnativegames.progress.events/1\",\"t\":%lld,\"kind\":\"guid\","
             "\"guid\":\"0x%016llX\",\"hasPosition\":%s,\"position\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"layer\":%d}",
             event.t, guid, have ? "true" : "false",
             have ? xyz[0] : 0.0, have ? xyz[1] : 0.0, have ? xyz[2] : 0.0,
             have ? (xyz[1] < -500 ? -1 : (xyz[1] > 1300 ? 1 : 0)) : 0);
    pthread_mutex_lock(&pe_lock);
    event.seq = ++pe_seq;
    if (pe_count == PE_RING) { pe_first = (pe_first + 1) % PE_RING; pe_count--; }
    pe_ring[(pe_first + pe_count) % PE_RING] = event;
    pe_count++;
    pthread_mutex_unlock(&pe_lock);
    pe_log("event seq %lld guid 0x%016llX", event.seq, guid);
}

static int in_world(void) {
    float xyz[3];
    return runtime_survey_player(xyz, NULL) && (xyz[0] * xyz[0] + xyz[2] * xyz[2] > 4.0f);
}

static void *pe_thread(void *unused) {
    (void)unused;
    unsigned long long guids[PE_MAX_GUIDS];
    unsigned long long previous[PE_MAX_GUIDS];
    PeArray array = {0, 0};
    int previous_count = 0;
    pe_log("thread started");
    for (;;) {
        usleep(250000);
        if (!in_world()) { array.start = 0; continue; }
        if (!array.start) {
            int fresh = load_save_guids(guids, PE_MAX_GUIDS);
            if (fresh < 8) { usleep(2000000); continue; }
            struct timespec ts0, ts1;
            clock_gettime(CLOCK_MONOTONIC, &ts0);
            array = locate_guid_array(guids, fresh);
            clock_gettime(CLOCK_MONOTONIC, &ts1);
            pe_log("locate save=%d found=%s start=%#llx length=%llu (%.1f s)", fresh,
                   array.start ? "yes" : "no", (unsigned long long)array.start,
                   (unsigned long long)array.length,
                   (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9);
            if (!array.start) { sleep(5); continue; }
            uint64_t take = array.length < PE_MAX_GUIDS ? array.length : PE_MAX_GUIDS;
            if (!read_mem(array.start, previous, (size_t)take * 8)) { array.start = 0; continue; }
            previous_count = (int)take;
        }
        uint64_t take = array.length < PE_MAX_GUIDS ? array.length : PE_MAX_GUIDS;
        unsigned long long current[PE_MAX_GUIDS];
        if (!read_mem(array.start, current, (size_t)take * 8)) { array.start = 0; continue; }
        int ascending = 1;
        for (uint64_t i = 1; i < take; i++) if (!(current[i - 1] < current[i])) { ascending = 0; break; }
        if (!ascending) { array.start = 0; usleep(500000); continue; }
        /* the array may have grown since the locate: walk forward while still ascending */
        while (take < PE_MAX_GUIDS) {
            unsigned long long next = 0;
            if (!read_mem(array.start + (uintptr_t)take * 8, &next, 8)) break;
            if (next <= current[take - 1]) break;
            current[take++] = next;
        }
        array.length = take;
        for (uint64_t i = 0; i < take; i++) {
            if (current[i] == 0) continue;
            if (!pe_contains(previous, (uint64_t)previous_count, current[i])) publish_guid(current[i]);
        }
        previous_count = (int)take;
        memcpy(previous, current, (size_t)take * 8);
    }
    return NULL;
}

void runtime_progress_events_start(void) {
    const char *off = getenv("TOTK_EVENTS");
    if (off && *off && !strcmp(off, "0")) return;
    if (atomic_exchange(&pe_running, 1)) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, pe_thread, NULL) == 0) pthread_detach(thread);
}

int progress_events_next(long long *cursor, char *json, size_t capacity) {
    int result = 0;
    pthread_mutex_lock(&pe_lock);
    for (int i = 0; i < pe_count; i++) {
        PeEvent *event = &pe_ring[(pe_first + i) % PE_RING];
        if (event->seq <= *cursor) continue;
        snprintf(json, capacity, "%s", event->json);
        *cursor = event->seq;
        result = 1;
        break;
    }
    pthread_mutex_unlock(&pe_lock);
    return result;
}
#endif
