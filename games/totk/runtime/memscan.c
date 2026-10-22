/* Diagnostic memory scanner (env TOTK_MEMSCAN=1, off by default) for the live collection events.

   Goal: prove that a collection in game can be observed in memory before the next save. Two anchors are used,
   both taken from the newest progress.sav:
     - flag pairs:  known (hash, value) entries (the in-game GameData flag table),
     - GUID pairs:  two consecutive 64-bit GUIDs from the save's GUID array (the collected-object list).
   Scanning is restricted to the guest memory windows (alias space at 0x7000000000 and the low 6 GiB) so it stays
   fast; everything is read-only (mach_vm_read_overwrite). A trigger file <data>/memscan.trigger forces a rescan.
   Log: <data>/memscan.log (profile parent). */
#include "memscan.h"
#include "game_version.h"
#include "world_survey.h"
#include <dirent.h>
#include <limits.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef GAME_BOTW
void runtime_memscan_start(void) { }
#else
#include "mach_vm_compat.h"

#define MS_FLAG_ANCHORS 12
#define MS_CHUNK (4u << 20)
#define MS_MAX_BYTES ((uint64_t)10 << 30)
#define MS_MAX_HITS 256
#define MS_ALIAS_BASE 0x7000000000ULL
#define MS_ALIAS_LIMIT (MS_ALIAS_BASE + (64ULL << 30))
#define MS_LOW_BASE 0x300000000ULL
#define MS_WATCH_BYTES 65536
#define MS_PROBE_MAX 8
#define MS_PROBE_HITS 24
#define MS_TARGET_MAX 32
#define MS_TARGET_WATCH_BYTES 4096
#define MS_WATCH_MS 100

static FILE *ms_file;
static uint64_t ms_flags[MS_FLAG_ANCHORS];
static int ms_flag_count;
static unsigned long long ms_guids[4];
static unsigned ms_probe_hashes[MS_PROBE_MAX];
static int ms_probe_count;
typedef struct { uintptr_t address; } MsProbeHit;
static MsProbeHit ms_probe_hits[MS_PROBE_MAX][MS_PROBE_HITS];
static int ms_probe_hit_count[MS_PROBE_MAX];
static unsigned char *ms_hash_prev;
static uintptr_t ms_hash_base;
static size_t ms_hash_size;
static unsigned char *ms_unstable_pages;
static unsigned long long ms_target_id;
static int ms_have_target;
static double ms_target_xyz[3];
static int ms_have_target_xyz;
static unsigned long long ms_target_hits[MS_TARGET_MAX];
static int ms_target_hit_count;
static unsigned char *ms_snap_data;
static uintptr_t ms_snap_base;
static int ms_snap_valid;
static unsigned char *ms_snap2_data;
static uintptr_t ms_snap2_base;
static int ms_snap2_valid;
static unsigned ms_target_hash;
static int ms_have_target_hash;
static unsigned long long ms_target_hash_hits[MS_TARGET_MAX];
static int ms_target_hash_hit_count;
#define MS_SNAP_BYTES 4096
static int ms_guid_count;
static double ms_last_xyz[3];
static int ms_have_xyz;

static void ms_log(const char *format, ...) {
    if (!ms_file) {
        const char *profile = getenv("TOTK_PROFILE_DIR");
        if (profile && *profile) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/../memscan.log", profile);
            ms_file = fopen(path, "a");
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&ts.tv_sec));
    float xyz[3];
    int have = runtime_survey_player(xyz, NULL);
    char where[64] = "";
    if (have) snprintf(where, sizeof where, " pos=%.1f,%.1f,%.1f", xyz[0], xyz[1], xyz[2]);
    va_list arguments;
    va_start(arguments, format);
    char line[2048];
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (ms_file) { fprintf(ms_file, "[%s.%03ld]%s %s\n", stamp, ts.tv_nsec / 1000000, where, line); fflush(ms_file); }
    fprintf(stderr, "[memscan][%s]%s %s\n", stamp, where, line);
}

static unsigned read_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int find_newest_save(char *out, size_t capacity) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (!profile || !*profile) return 0;
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/save", profile);
    DIR *hashes = opendir(root);
    if (!hashes) return 0;
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
            if (stamp > best) { best = stamp; snprintf(out, capacity, "%s", path); }
        }
        closedir(slots);
    }
    closedir(hashes);
    return best != 0;
}

/* Dynamic keys that change without a collection; not used as table anchors. */
static int is_dynamic(unsigned hash) {
    switch (hash) {
        case 0x449B3BA6u: case 0x2106FA19u: case 0x1EA93BD8u: case 0x46920F1Bu: case 0x878AEE40u:
        case 0x3F8D9D86u: case 0xE573F564u: case 0xC3C3C9CEu: case 0x31AB5580u:
            return 1;
        default: return 0;
    }
}

static int load_save_anchors(void) {
    char path[PATH_MAX];
    if (!find_newest_save(path, sizeof path)) { ms_log("no save found"); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0x10000 || size > (16 << 20)) { fclose(f); return 0; }
    unsigned char *data = malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(data); return 0; }

    ms_flag_count = 0;
    unsigned guids_offset = 0;
    unsigned POS = 0xC884818Du, WPOS = 0x8773F02Du, END = 0xA3DB7114u;
    for (size_t i = 0x28; i + 8 <= got; i += 8) {
        unsigned hash = read_u32(data + i), value = read_u32(data + i + 4);
        if (hash == END) { guids_offset = value; break; }
        if ((hash == POS || hash == WPOS) && value >= 0x10000 && (value & 3u) == 0 && (size_t)value + 12 <= got) {
            float xyz[3];
            memcpy(xyz, data + value, sizeof xyz);
            if (xyz[0] == xyz[0] && xyz[1] == xyz[1] && xyz[2] == xyz[2]) {
                ms_last_xyz[0] = xyz[0]; ms_last_xyz[1] = xyz[1]; ms_last_xyz[2] = xyz[2];
                ms_have_xyz = 1;
            }
        }
        if (ms_flag_count < MS_FLAG_ANCHORS && value == 1 && !is_dynamic(hash))
            ms_flags[ms_flag_count++] = ((uint64_t)value << 32) | hash;
    }
    ms_guid_count = 0;
    if (guids_offset >= 0x10000 && guids_offset + 16 <= got) {
        for (size_t i = guids_offset; i + 16 <= got && ms_guid_count < 4; i += 8) {
            unsigned lo = read_u32(data + i), up = read_u32(data + i + 4);
            if (lo == 0 && up == 0) break;
            ms_guids[ms_guid_count++] = ((unsigned long long)up << 32) | lo;
        }
    }
    free(data);
    ms_probe_count = 0;
    const char *probe = getenv("TOTK_MEMSCAN_PROBE");
    if (probe && *probe) {
        char copy[128];
        snprintf(copy, sizeof copy, "%s", probe);
        char *saveptr = NULL;
        for (char *token = strtok_r(copy, ", ", &saveptr); token && ms_probe_count < MS_PROBE_MAX; token = strtok_r(NULL, ", ", &saveptr))
            ms_probe_hashes[ms_probe_count++] = (unsigned)strtoul(token, NULL, 16);
    }
    ms_have_target = 0;
    const char *target = getenv("TOTK_MEMSCAN_TARGET");
    if (target && *target) {
        ms_target_id = strtoull(target, NULL, 0);
        ms_have_target = 1;
    }
    ms_have_target_xyz = 0;
    const char *target_xyz = getenv("TOTK_MEMSCAN_TARGET_XYZ");
    if (target_xyz && *target_xyz) {
        char copy[128];
        snprintf(copy, sizeof copy, "%s", target_xyz);
        char *saveptr = NULL;
        int n = 0;
        for (char *token = strtok_r(copy, ",", &saveptr); token && n < 3; token = strtok_r(NULL, ",", &saveptr))
            ms_target_xyz[n++] = atof(token);
        ms_have_target_xyz = n == 3;
    }
    ms_have_target_hash = 0;
    const char *target_hash = getenv("TOTK_MEMSCAN_TARGET_HASH");
    if (target_hash && *target_hash) { ms_target_hash = (unsigned)strtoul(target_hash, NULL, 16); ms_have_target_hash = 1; }
    ms_log("save %s: flag anchors %d, guid anchors %d, flag probes %d, target %s%016llX%s", path, ms_flag_count,
           ms_guid_count, ms_probe_count, ms_have_target ? "0x" : "(none) ",
           ms_have_target ? ms_target_id : 0ULL, ms_have_xyz ? "" : " (no position)");
    return ms_flag_count >= 2 && ms_guid_count >= 2;
}

typedef struct { uintptr_t address; } MsHit;
static MsHit ms_flag_hits[MS_FLAG_ANCHORS][MS_MAX_HITS];
static int ms_flag_hit_count[MS_FLAG_ANCHORS];
static MsHit ms_guid_hits[MS_MAX_HITS];
static int ms_guid_hit_count;

static void scan_buffer(const unsigned char *buffer, size_t length, uintptr_t base) {
    for (int a = 0; a < ms_flag_count && ms_flag_hit_count[a] < MS_MAX_HITS; a++) {
        const unsigned char *needle = (const unsigned char *)&ms_flags[a];
        const unsigned char *p = buffer;
        while (p + 8 <= buffer + length && ms_flag_hit_count[a] < MS_MAX_HITS) {
            const unsigned char *at = memmem(p, (size_t)(buffer + length - p), needle, 8);
            if (!at) break;
            ms_flag_hits[a][ms_flag_hit_count[a]++].address = base + (uintptr_t)(at - buffer);
            p = at + 1;
        }
    }
    if (ms_have_target && ms_target_hit_count < MS_TARGET_MAX) {
        unsigned char needle[8];
        memcpy(needle, &ms_target_id, 8);
        const unsigned char *p = buffer;
        while (p + 8 <= buffer + length && ms_target_hit_count < MS_TARGET_MAX) {
            const unsigned char *at = memmem(p, (size_t)(buffer + length - p), needle, 8);
            if (!at) break;
            ms_target_hits[ms_target_hit_count++] = (unsigned long long)(base + (uintptr_t)(at - buffer));
            p = at + 1;
        }
    }
    if (ms_have_target_hash && ms_target_hash_hit_count < MS_TARGET_MAX) {
        const unsigned char *p = buffer;
        while (p + 4 <= buffer + length && ms_target_hash_hit_count < MS_TARGET_MAX) {
            const unsigned char *at = memmem(p, (size_t)(buffer + length - p), &ms_target_hash, 4);
            if (!at) break;
            ms_target_hash_hits[ms_target_hash_hit_count++] = (unsigned long long)(base + (uintptr_t)(at - buffer));
            p = at + 1;
        }
    }
    for (int a = 0; a < ms_probe_count && ms_probe_hit_count[a] < MS_PROBE_HITS; a++) {
        unsigned needle = ms_probe_hashes[a];
        const unsigned char *p = buffer;
        while (p + 4 <= buffer + length && ms_probe_hit_count[a] < MS_PROBE_HITS) {
            const unsigned char *at = memmem(p, (size_t)(buffer + length - p), &needle, 4);
            if (!at) break;
            ms_probe_hits[a][ms_probe_hit_count[a]++].address = base + (uintptr_t)(at - buffer);
            p = at + 1;
        }
    }
    if (ms_guid_count >= 2 && ms_guid_hit_count < MS_MAX_HITS) {
        unsigned char needle[16];
        memcpy(needle, &ms_guids[0], 8);
        memcpy(needle + 8, &ms_guids[1], 8);
        const unsigned char *p = buffer;
        while (p + 16 <= buffer + length && ms_guid_hit_count < MS_MAX_HITS) {
            const unsigned char *at = memmem(p, (size_t)(buffer + length - p), needle, 16);
            if (!at) break;
            ms_guid_hits[ms_guid_hit_count++].address = base + (uintptr_t)(at - buffer);
            p = at + 1;
        }
    }
}

extern void runtime_guest_regions(uintptr_t *heap_base, uint64_t *heap_bytes, uintptr_t *alias_base, uint64_t *alias_size_out);

static uint64_t ms_scan_range(const char *label, uintptr_t base, uint64_t size) {
    if (!base || !size) return 0;
    unsigned char *buffer = malloc(MS_CHUNK);
    if (!buffer) return 0;
    uint64_t scanned = 0;
    int chunks = 0, failed = 0;
    for (uint64_t offset = 0; offset < size; offset += MS_CHUNK) {
        size_t want = (size_t)((size - offset) < MS_CHUNK ? (size - offset) : MS_CHUNK);
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(base + offset), (mach_vm_size_t)want,
                                   (mach_vm_address_t)(uintptr_t)buffer, &got) != KERN_SUCCESS || got == 0) {
            failed++;
            continue;
        }
        scan_buffer(buffer, (size_t)got, base + offset);
        scanned += got;
        chunks++;
    }
    free(buffer);
    ms_log("range %s %#llx+%#llu: %d chunks readable (%llu MiB), %d pages unmapped", label,
           (unsigned long long)base, (unsigned long long)size, chunks, (unsigned long long)(scanned >> 20), failed);
    return scanned;
}

static void scan_memory(void) {
    memset(ms_flag_hit_count, 0, sizeof ms_flag_hit_count);
    memset(ms_probe_hit_count, 0, sizeof ms_probe_hit_count);
    ms_guid_hit_count = 0;
    ms_target_hit_count = 0;
    ms_target_hash_hit_count = 0;
    uintptr_t heap = 0, alias = 0;
    uint64_t heap_size = 0, alias_size = 0;
    runtime_guest_regions(&heap, &heap_size, &alias, &alias_size);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t scanned = 0;
    scanned += ms_scan_range("heap", heap, heap_size);
    if (scanned < MS_MAX_BYTES) scanned += ms_scan_range("alias", alias, alias_size < (64ULL << 30) ? alias_size : (64ULL << 30));
    clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    int flag_total = 0;
    for (int i = 0; i < ms_flag_count; i++) flag_total += ms_flag_hit_count[i];
    ms_log("scan %.1f s, %llu MiB readable: flag hits %d, guid-pair hits %d", seconds,
           (unsigned long long)(scanned >> 20), flag_total, ms_guid_hit_count);
    for (int i = 0; i < ms_flag_count; i++)
        if (ms_flag_hit_count[i]) ms_log("  flag anchor 0x%08X hit %d, first %#llx", (unsigned)(ms_flags[i] & 0xFFFFFFFFu), ms_flag_hit_count[i], (unsigned long long)ms_flag_hits[i][0].address);
    for (int i = 0; i < ms_guid_hit_count && i < 8; i++)
        ms_log("  guid pair hit %#llx", (unsigned long long)ms_guid_hits[i].address);
    if (ms_have_target) ms_log("target 0x%016llX hits %d", ms_target_id, ms_target_hit_count);
    if (ms_have_target_hash) ms_log("target hash 0x%08X hits %d", ms_target_hash, ms_target_hash_hit_count);
    for (int a = 0; a < ms_probe_count; a++) {
        ms_log("  flag probe 0x%08X hits %d", ms_probe_hashes[a], ms_probe_hit_count[a]);
        for (int k = 0; k < ms_probe_hit_count[a] && k < 6; k++) {
            uintptr_t address = ms_probe_hits[a][k].address & ~(uintptr_t)3;
            unsigned char context[64];
            mach_vm_size_t got2 = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(address - 16), (mach_vm_size_t)sizeof context,
                                       (mach_vm_address_t)(uintptr_t)context, &got2) != KERN_SUCCESS || got2 != sizeof context) {
                ms_log("    0x%llx (context unreadable)", (unsigned long long)address);
                continue;
            }
            char text[512];
            size_t used = 0;
            for (int w = 0; w < 16 && used + 12 < sizeof text; w++)
                used += (size_t)snprintf(text + used, sizeof text - used, "%08X ", read_u32(context + w * 4));
            ms_log("    0x%llx: %s", (unsigned long long)address, text);
        }
    }
}

static uintptr_t choose_flag_table(void) {
    uintptr_t best = 0;
    int best_score = 0;
    for (int i = 0; i < ms_flag_hit_count[0]; i++) {
        uintptr_t base = ms_flag_hits[0][i].address;
        int score = 1;
        for (int a = 1; a < ms_flag_count; a++) {
            for (int k = 0; k < ms_flag_hit_count[a]; k++) {
                uintptr_t other = ms_flag_hits[a][k].address;
                uintptr_t delta = other > base ? other - base : base - other;
                if (delta < 0x200000) { score++; break; }
            }
        }
        if (score > best_score) { best_score = score; best = base; }
    }
    if (best && best_score >= 2) {
        ms_log("flag table candidate %#llx (score %d/%d)", (unsigned long long)best, best_score, ms_flag_count);
        return best;
    }
    ms_log("no flag table candidate (best %d)", best_score);
    return 0;
}

static void watch(uintptr_t address, const char *label) {
    uintptr_t base = address & ~(uintptr_t)0xFFF;
    unsigned char previous[MS_WATCH_BYTES], current[MS_WATCH_BYTES];
    if (!base) return;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)base, MS_WATCH_BYTES,
                               (mach_vm_address_t)(uintptr_t)previous, &got) != KERN_SUCCESS || got != MS_WATCH_BYTES) {
        ms_log("watch %s at %#llx unreadable", label, (unsigned long long)base);
        return;
    }
    ms_log("watching %s at %#llx", label, (unsigned long long)base);
    for (;;) {
        usleep(MS_WATCH_MS * 1000);
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)base, MS_WATCH_BYTES,
                                   (mach_vm_address_t)(uintptr_t)current, &got) != KERN_SUCCESS || got != MS_WATCH_BYTES)
            continue;
        for (size_t i = 0; i + 4 <= MS_WATCH_BYTES; i += 4) {
            unsigned before = read_u32(previous + i), after = read_u32(current + i);
            if (before == after) continue;
            ms_log("%s change at %#llx: 0x%08X -> 0x%08X (u32 pairs: %08X/%08X)", label,
                   (unsigned long long)(base + i), before, after,
                   read_u32(current + (i & ~3u)), read_u32(current + (i & ~3u) + 4));
        }
        memcpy(previous, current, MS_WATCH_BYTES);
    }
}

static void trigger_path(char *out, size_t capacity) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (profile && *profile) snprintf(out, capacity, "%s/../memscan.trigger", profile);
    else snprintf(out, capacity, "memscan.trigger");
}

static void *hash_watch_thread(void *unused) {
    (void)unused;
    unsigned char *current = malloc(ms_hash_size);
    unsigned char *probe = malloc(ms_hash_size);
    if (!current || !probe || !ms_hash_prev) return NULL;
    size_t page_count = ms_hash_size / 4096;
    ms_unstable_pages = calloc(page_count, 1);
    if (!ms_unstable_pages) return NULL;
    /* Calibrate: pages that change within 300 ms are lists/pointers, not the flag table. */
    usleep(300000);
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)ms_hash_base, (mach_vm_size_t)ms_hash_size,
                               (mach_vm_address_t)(uintptr_t)probe, &got) == KERN_SUCCESS && got == ms_hash_size) {
        for (size_t page = 0; page < page_count; page++) {
            const unsigned char *a = ms_hash_prev + page * 4096, *b = probe + page * 4096;
            if (memcmp(a, b, 4096) != 0) ms_unstable_pages[page] = 1;
        }
        memcpy(ms_hash_prev, probe, ms_hash_size);
    }
    size_t unstable = 0;
    for (size_t page = 0; page < page_count; page++) unstable += ms_unstable_pages[page];
    ms_log("watching live hash array %#llx+%#llu (stable pages %zu/%zu)", (unsigned long long)ms_hash_base,
           (unsigned long long)ms_hash_size, page_count - unstable, page_count);
    for (;;) {
        usleep(MS_WATCH_MS * 1000);
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)ms_hash_base, (mach_vm_size_t)ms_hash_size,
                                   (mach_vm_address_t)(uintptr_t)current, &got) != KERN_SUCCESS || got != ms_hash_size)
            continue;
        for (size_t page = 0; page < page_count; page++) {
            if (ms_unstable_pages[page]) continue;
            for (size_t j = 0; j < 4096; j += 4) {
                size_t i = page * 4096 + j;
                unsigned before = read_u32(ms_hash_prev + i), after = read_u32(current + i);
                if (before == after) continue;
                if ((before == 0) != (after == 0))
                    ms_log("HASH-ARRAY %#llx: 0x%08X -> 0x%08X", (unsigned long long)(ms_hash_base + i), before, after);
            }
        }
        memcpy(ms_hash_prev, current, ms_hash_size);
    }
    return NULL;
}

static void choose_hash_window(void) {
    if (ms_probe_count <= 0) return;
    uintptr_t low = 0, high = 0;
    int hits = 0, array_hits = 0;
    for (int a = 0; a < ms_probe_count; a++) {
        /* A flag hash is stored twice in the world: a stable node and an entry in the live hash array.
           Only the array entry has a hit count >= 2 for the same probe, and its neighbourhood is dense
           with hash-like (large, random) values instead of pointer/small-integer pairs. */
        if (ms_probe_hit_count[a] < 2) continue;
        for (int k = 0; k < ms_probe_hit_count[a]; k++) {
            uintptr_t at = ms_probe_hits[a][k].address;
            unsigned char page[4096];
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(at & ~(uintptr_t)0xFFF),
                                       (mach_vm_size_t)sizeof page, (mach_vm_address_t)(uintptr_t)page, &got) != KERN_SUCCESS || got != sizeof page)
                continue;
            int total = (int)(sizeof page / 4), nonzero = 0, small = 0;
            for (int w = 0; w < total; w++) {
                unsigned v = read_u32(page + w * 4);
                if (v) nonzero++;
                if (v && v < 0x10000) small++;
            }
            /* Hash-table pages are dense with random 32-bit hashes; pointer/list pages have many small
               high halves (0x00000070 ...) and node pages have many zeros and small indices. */
            if (nonzero < total / 2 || small > total / 20) continue;
            if (!hits || at < low) low = at;
            if (!hits || at > high) high = at;
            hits++;
            array_hits++;
        }
    }
    if (hits < 3) { ms_log("hash array: not enough array-like probe hits (%d, probes with 2 hits: %d)", hits, array_hits); return; }
    /* Window: cover the hits with a 2 MiB margin and align to 2 MiB. */
    uintptr_t base = (low & ~(uintptr_t)0x1FFFFF) - 0x200000;
    uintptr_t end = ((high + 0x1FFFFF) & ~(uintptr_t)0x1FFFFF) + 0x200000;
    if (base < 0x300000000ULL) base = 0x300000000ULL;
    size_t size = (size_t)(end - base);
    if (size > (16u << 20)) size = 16u << 20;
    ms_hash_prev = malloc(size);
    if (!ms_hash_prev) { ms_log("hash array: allocation failed"); return; }
    ms_hash_base = base;
    ms_hash_size = size;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)base, (mach_vm_size_t)size,
                               (mach_vm_address_t)(uintptr_t)ms_hash_prev, &got) != KERN_SUCCESS || got != size) {
        ms_log("hash array window unreadable at %#llx", (unsigned long long)base);
        free(ms_hash_prev); ms_hash_prev = NULL;
        return;
    }
    ms_log("hash array window %#llx+%#llu from %d array-like hits", (unsigned long long)base, (unsigned long long)size, hits);
    pthread_t thread;
    if (pthread_create(&thread, NULL, hash_watch_thread, NULL) == 0) pthread_detach(thread);
}

static uintptr_t locate_target(void) {
    uintptr_t best = 0;
    for (int i = 0; i < ms_target_hit_count && !best; i++) {
        uintptr_t at = (uintptr_t)ms_target_hits[i];
        unsigned char page[4096];
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(at & ~(uintptr_t)0xFFF),
                                   (mach_vm_size_t)sizeof page, (mach_vm_address_t)(uintptr_t)page, &got) != KERN_SUCCESS || got != sizeof page)
            continue;
        if (!ms_have_target_xyz) { best = at; break; }
        for (int f = 0; f + 12 <= (int)sizeof page; f += 4) {
            float fx, fy, fz;
            memcpy(&fx, page + f, 4); memcpy(&fy, page + f + 4, 4); memcpy(&fz, page + f + 8, 4);
            double dx = fx - ms_target_xyz[0], dy = fy - ms_target_xyz[1], dz = fz - ms_target_xyz[2];
            if (dx * dx + dy * dy + dz * dz < 25.0) { best = at; break; }
        }
    }
    return best;
}

static void take_snapshot(uintptr_t base) {
    if (!ms_snap_data) ms_snap_data = malloc(MS_SNAP_BYTES);
    if (!ms_snap_data) return;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)base, MS_SNAP_BYTES,
                               (mach_vm_address_t)(uintptr_t)ms_snap_data, &got) != KERN_SUCCESS || got != MS_SNAP_BYTES) {
        ms_log("snapshot read failed at %#llx", (unsigned long long)base);
        return;
    }
    ms_snap_base = base;
    ms_snap_valid = 1;
    ms_log("snapshot taken at %#llx (%d bytes)", (unsigned long long)base, MS_SNAP_BYTES);
}

static void diff_snapshot(void) {
    if (!ms_snap_valid || !ms_snap_data) { ms_log("diff requested but no snapshot"); return; }
    unsigned char current[MS_SNAP_BYTES];
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)ms_snap_base, MS_SNAP_BYTES,
                               (mach_vm_address_t)(uintptr_t)current, &got) != KERN_SUCCESS || got != MS_SNAP_BYTES) {
        ms_log("diff read failed at %#llx", (unsigned long long)ms_snap_base);
        return;
    }
    int changes = 0;
    for (int i = 0; i + 4 <= MS_SNAP_BYTES; i += 4) {
        unsigned before = read_u32(ms_snap_data + i), after = read_u32(current + i);
        if (before == after) continue;
        changes++;
        ms_log("SNAPSHOT DIFF %#llx: 0x%08X -> 0x%08X", (unsigned long long)(ms_snap_base + i), before, after);
    }
    ms_log("snapshot diff done: %d changed dwords (base %#llx)", changes, (unsigned long long)ms_snap_base);
    if (ms_snap2_valid && ms_snap2_data) {
        unsigned char current2[MS_SNAP_BYTES];
        mach_vm_size_t got2 = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)ms_snap2_base, MS_SNAP_BYTES,
                                   (mach_vm_address_t)(uintptr_t)current2, &got2) == KERN_SUCCESS && got2 == MS_SNAP_BYTES) {
            int changes2 = 0;
            for (int i = 0; i + 4 <= MS_SNAP_BYTES; i += 4) {
                unsigned before = read_u32(ms_snap2_data + i), after = read_u32(current2 + i);
                if (before == after) continue;
                changes2++;
                ms_log("HASH SNAPSHOT DIFF %#llx: 0x%08X -> 0x%08X", (unsigned long long)(ms_snap2_base + i), before, after);
            }
            ms_log("hash snapshot diff done: %d changed dwords", changes2);
            memcpy(ms_snap2_data, current2, MS_SNAP_BYTES);
        }
    }
    memcpy(ms_snap_data, current, MS_SNAP_BYTES);
}

static void snapshot_thread_paths(char *before, size_t bcap, char *after, size_t acap) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (profile && *profile) {
        snprintf(before, bcap, "%s/../memscan.before", profile);
        snprintf(after, acap, "%s/../memscan.after", profile);
    } else {
        snprintf(before, bcap, "memscan.before");
        snprintf(after, acap, "memscan.after");
    }
}

/* Optional <data>/memscan.target file lets the orchestrator retarget without restarting the game:
   lines: id=<decimal instance id>, hash=<hex flag hash>, xyz=<x,y,z> */
static void read_target_file(void) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (!profile || !*profile) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/../memscan.target", profile);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *value = eq + 1;
        while (*value == ' ') value++;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!strcmp(line, "id")) { ms_target_id = strtoull(value, NULL, 0); ms_have_target = 1; }
        else if (!strcmp(line, "hash")) { ms_target_hash = (unsigned)strtoul(value, NULL, 16); ms_have_target_hash = 1; }
        else if (!strcmp(line, "xyz")) {
            char copy[128];
            snprintf(copy, sizeof copy, "%s", value);
            char *saveptr = NULL;
            int n = 0;
            for (char *token = strtok_r(copy, ",", &saveptr); token && n < 3; token = strtok_r(NULL, ",", &saveptr))
                ms_target_xyz[n++] = atof(token);
            ms_have_target_xyz = n == 3;
        }
    }
    fclose(f);
    ms_log("target file: id %s%016llX hash 0x%08X xyz %s", ms_have_target ? "0x" : "(none)",
           ms_have_target ? ms_target_id : 0ULL, ms_target_hash, ms_have_target_xyz ? "set" : "none");
}

/* Full guest-memory page diff: dump every readable page before the action, then after the action find pages
   whose content changed but stayed stable for >1.2 s (animation pages keep changing and are excluded).
   For each persistent page the changed dwords are logged, which reveals where a flag value lives. */
#define PD_PAGE 4096
#define PD_CHUNK (4u << 20)
static uintptr_t pd_regions[2];
static uint64_t pd_sizes[2];
static int pd_region_count;
static uint64_t *pd_before;
static uint64_t *pd_after;
static uint32_t pd_pages;
static FILE *pd_file;
static uint64_t pd_data_start[2];

static uint64_t pd_hash(const unsigned char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static uint64_t pd_page_count(void) {
    uint64_t n = 0;
    for (int r = 0; r < pd_region_count; r++) n += (pd_sizes[r] + PD_PAGE - 1) / PD_PAGE;
    return n;
}

static int pd_dump_before(void) {
    runtime_guest_regions(&pd_regions[0], &pd_sizes[0], &pd_regions[1], &pd_sizes[1]);
    pd_region_count = 2;
    pd_pages = (uint32_t)pd_page_count();
    pd_before = calloc(pd_pages, 8);
    if (!pd_before) return 0;
    const char *profile = getenv("TOTK_PROFILE_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/../memscan.pagedump.bin", profile ? profile : ".");
    pd_file = fopen(path, "wb");
    if (!pd_file) { ms_log("pagediff: cannot open dump"); return 0; }
    uint64_t header = 0x5041474544494646ULL; /* "PAGEDIFF" */
    fwrite(&header, 8, 1, pd_file);
    fwrite(&pd_region_count, 4, 1, pd_file);
    uint64_t offset = 8 + 4 + 8 * 2 * 2;
    for (int r = 0; r < pd_region_count; r++) {
        fwrite(&pd_regions[r], 8, 1, pd_file); fwrite(&pd_sizes[r], 8, 1, pd_file);
        pd_data_start[r] = offset; offset += pd_sizes[r];
    }
    unsigned char *buffer = malloc(PD_CHUNK);
    uint32_t page = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < pd_region_count && buffer; r++) {
        uint64_t limit = pd_sizes[r];
        for (uint64_t off = 0; off < limit; off += PD_CHUNK) {
            size_t want = (size_t)((limit - off) < PD_CHUNK ? (limit - off) : PD_CHUNK);
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(pd_regions[r] + off), (mach_vm_size_t)want,
                                       (mach_vm_address_t)(uintptr_t)buffer, &got) != KERN_SUCCESS || got == 0) {
                memset(buffer, 0, want);
                got = want;
                for (size_t pageoff = 0; pageoff < want; pageoff += PD_PAGE) pd_before[page++] = 0;
            } else {
                for (size_t pageoff = 0; pageoff < (size_t)got; pageoff += PD_PAGE)
                    pd_before[page++] = got - pageoff >= PD_PAGE ? pd_hash(buffer + pageoff, PD_PAGE) : 0;
            }
            fwrite(buffer, 1, (size_t)got, pd_file);
        }
    }
    free(buffer);
    fclose(pd_file); pd_file = NULL;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms_log("pagediff before: %u pages, dump %.1f s", pd_pages, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return 1;
}

static int pd_hash_live(uint64_t *out) {
    unsigned char *buffer = malloc(PD_CHUNK);
    if (!buffer) return 0;
    uint32_t page = 0;
    for (int r = 0; r < pd_region_count; r++) {
        for (uint64_t off = 0; off < pd_sizes[r]; off += PD_CHUNK) {
            size_t want = (size_t)((pd_sizes[r] - off) < PD_CHUNK ? (pd_sizes[r] - off) : PD_CHUNK);
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(pd_regions[r] + off), (mach_vm_size_t)want,
                                       (mach_vm_address_t)(uintptr_t)buffer, &got) != KERN_SUCCESS || got == 0) {
                got = want; memset(buffer, 0, want);
                for (size_t pageoff = 0; pageoff < want; pageoff += PD_PAGE) out[page++] = 0;
            } else {
                for (size_t pageoff = 0; pageoff < (size_t)got; pageoff += PD_PAGE)
                    out[page++] = got - pageoff >= PD_PAGE ? pd_hash(buffer + pageoff, PD_PAGE) : 0;
            }
        }
    }
    free(buffer);
    return 1;
}

static void pd_diff_after(void) {
    if (!pd_before) { ms_log("pagediff after: no before snapshot"); return; }
    pd_after = calloc(pd_pages, 8);
    if (!pd_after) return;
    pd_hash_live(pd_after);
    usleep(1200000);
    uint64_t *stable = calloc(pd_pages, 8);
    if (!stable) { free(pd_after); return; }
    pd_hash_live(stable);
    /* reopen dump for before-page bytes */
    const char *profile = getenv("TOTK_PROFILE_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/../memscan.pagedump.bin", profile ? profile : ".");
    FILE *dump = fopen(path, "rb");
    int persistent = 0;
    uint32_t page = 0;
    for (int r = 0; r < pd_region_count; r++) {
        uint64_t limit = pd_sizes[r];
        for (uint64_t off = 0; off < limit; off += PD_PAGE, page++) {
            if (!pd_before[page] || !pd_after[page] || !stable[page]) continue;
            if (pd_before[page] == pd_after[page]) continue;
            if (pd_after[page] != stable[page]) continue; /* still changing: animation */
            persistent++;
            if (persistent > 40) continue;
            unsigned char before_page[PD_PAGE], after_page[PD_PAGE];
            int have_before = 0;
            if (dump && fseek(dump, (long)(pd_data_start[r] + off), SEEK_SET) == 0 &&
                fread(before_page, 1, PD_PAGE, dump) == PD_PAGE) have_before = 1;
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(pd_regions[r] + off), PD_PAGE,
                                       (mach_vm_address_t)(uintptr_t)after_page, &got) != KERN_SUCCESS || got != PD_PAGE) continue;
            ms_log("pagediff persistent page %#llx (region %d offset %#llx%s)", (unsigned long long)(pd_regions[r] + off), r,
                   (unsigned long long)off, have_before ? "" : ", no before data");
            int shown = 0;
            for (int i = 0; have_before && i + 4 <= PD_PAGE && shown < 10; i += 4) {
                unsigned b = read_u32(before_page + i), a = read_u32(after_page + i);
                if (b == a) continue;
                ms_log("  page %#llx +%#x: 0x%08X -> 0x%08X", (unsigned long long)(pd_regions[r] + off), i, b, a);
                shown++;
            }
        }
    }
    if (dump) fclose(dump);
    ms_log("pagediff after: %d persistent changed pages", persistent);
    free(stable); free(pd_after); pd_after = NULL;
}

/* Index/value array search: the GameData node holds  hash -> small index.  If the game keeps an array of
   values indexed by that id, then for known flags (id, expected value) we can find its base by searching for
   the two known 1-valued indices 8 bytes apart (15621 / 15623) and verifying the other pairs. */
static unsigned vs_ids[16];
static unsigned vs_expect[16];
static int vs_count;

static void vs_parse(void) {
    vs_count = 0;
    const char *spec = getenv("TOTK_MEMSCAN_VALUES");
    if (!spec || !*spec) return;
    char copy[512];
    snprintf(copy, sizeof copy, "%s", spec);
    char *saveptr = NULL;
    for (char *token = strtok_r(copy, ",", &saveptr); token && vs_count < 16; token = strtok_r(NULL, ",", &saveptr)) {
        char *colon = strchr(token, ':');
        if (!colon) continue;
        *colon = 0;
        vs_ids[vs_count] = (unsigned)strtoul(token, NULL, 10);
        vs_expect[vs_count] = (unsigned)strtoul(colon + 1, NULL, 16);
        vs_count++;
    }
}

static int vs_read_u32(uintptr_t address, unsigned *out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address, 4, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == 4;
}

static void vs_search(void) {
    vs_parse();
    if (vs_count < 3) { ms_log("valuesearch: need >=3 id:hex pairs"); return; }
    uintptr_t heap = 0, alias = 0;
    uint64_t heap_size = 0, alias_size = 0;
    runtime_guest_regions(&heap, &heap_size, &alias, &alias_size);
    uintptr_t regions[2] = {heap, alias};
    uint64_t sizes[2] = {heap_size, alias_size};
    unsigned anchor_id = vs_ids[0], anchor_expect = vs_expect[0];
    unsigned second_id = vs_ids[1], second_expect = vs_expect[1];
    long long second_delta = (long long)second_id * 4 - (long long)anchor_id * 4;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned char *buffer = malloc(PD_CHUNK);
    int candidates = 0;
    uint64_t scanned = 0;
    for (int r = 0; r < 2 && buffer; r++) {
        for (uint64_t off = 0; off < sizes[r]; off += PD_CHUNK) {
            size_t want = (size_t)((sizes[r] - off) < PD_CHUNK ? (sizes[r] - off) : PD_CHUNK);
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(regions[r] + off), (mach_vm_size_t)want,
                                       (mach_vm_address_t)(uintptr_t)buffer, &got) != KERN_SUCCESS || got == 0)
                continue;
            scanned += got;
            for (size_t i = 0; i + 4 <= (size_t)got; i += 4) {
                unsigned v = read_u32(buffer + i);
                if (v != anchor_expect) continue;
                uintptr_t anchor = regions[r] + off + i;
                unsigned second = 0;
                if (!vs_read_u32((uintptr_t)((long long)anchor + second_delta), &second) || second != second_expect) continue;
                uintptr_t base = anchor - (uintptr_t)anchor_id * 4;
                int matches = 0, checked = 0;
                for (int k = 0; k < vs_count; k++) {
                    unsigned value = 0;
                    if (!vs_read_u32(base + (uintptr_t)vs_ids[k] * 4, &value)) continue;
                    checked++;
                    if (vs_ids[k] == 5670u) { if (value < 24) matches++; continue; }
                    if (vs_ids[k] == 5671u) { if (value < 60) matches++; continue; }
                    if (value == vs_expect[k]) matches++;
                }
                if (checked >= 3 && matches >= vs_count - 1) {
                    candidates++;
                    if (candidates <= 20) {
                        char values[256];
                        size_t used = 0;
                        for (int k = 0; k < vs_count && used + 16 < sizeof values; k++) {
                            unsigned value = 0;
                            vs_read_u32(base + (uintptr_t)vs_ids[k] * 4, &value);
                            used += (size_t)snprintf(values + used, sizeof values - used, "%u:%u ", vs_ids[k], value);
                        }
                        ms_log("valuesearch candidate base %#llx (scanned %llu MiB): %s", (unsigned long long)base,
                               (unsigned long long)(scanned >> 20), values);
                    }
                }
            }
        }
    }
    free(buffer);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms_log("valuesearch done: %d candidates in %.1f s", candidates,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
}

static void *snapshot_thread(void *unused) {
    (void)unused;
    char before[PATH_MAX], after[PATH_MAX];
    snapshot_thread_paths(before, sizeof before, after, sizeof after);
    sleep(8);
    for (;;) {
        struct stat st;
        if (stat(after, &st) == 0) {
            remove(after);
            if (getenv("TOTK_MEMSCAN_PAGEDIFF") && *getenv("TOTK_MEMSCAN_PAGEDIFF")) pd_diff_after(); else diff_snapshot();
            continue;
        }
        if (stat(before, &st) == 0) {
            remove(before);
            if (getenv("TOTK_MEMSCAN_VALUESEARCH") && *getenv("TOTK_MEMSCAN_VALUESEARCH")) {
                float vxyz[3];
                int inw = runtime_survey_player(vxyz, NULL) && (vxyz[0] * vxyz[0] + vxyz[2] * vxyz[2] > 4.0f);
                if (!inw) { ms_log("valuesearch before while at menu; ignored"); continue; }
                vs_search();
                continue;
            }
            if (getenv("TOTK_MEMSCAN_PAGEDIFF") && *getenv("TOTK_MEMSCAN_PAGEDIFF")) {
                float pxyz[3];
                int inw = runtime_survey_player(pxyz, NULL) && (pxyz[0] * pxyz[0] + pxyz[2] * pxyz[2] > 4.0f);
                if (!inw) { ms_log("pagediff before while at menu; ignored"); continue; }
                pd_dump_before();
                continue;
            }
            float xyz[3];
            int in_world = runtime_survey_player(xyz, NULL) && (xyz[0] * xyz[0] + xyz[2] * xyz[2] > 4.0f);
            if (!in_world) { ms_log("before trigger while at menu; ignored"); continue; }
            read_target_file();
            if (!load_save_anchors()) { ms_log("before trigger: no anchors"); continue; }
            scan_memory();
            uintptr_t base = locate_target();
            if (base) take_snapshot(base & ~(uintptr_t)0xFF); else ms_log("before trigger: target not located");
            if (ms_have_target_hash && ms_target_hash_hit_count > 0) {
                if (!ms_snap2_data) ms_snap2_data = malloc(MS_SNAP_BYTES);
                uintptr_t hb = (uintptr_t)ms_target_hash_hits[0] & ~(uintptr_t)0xFFF;
                mach_vm_size_t got2 = 0;
                if (ms_snap2_data && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)hb, MS_SNAP_BYTES,
                        (mach_vm_address_t)(uintptr_t)ms_snap2_data, &got2) == KERN_SUCCESS && got2 == MS_SNAP_BYTES) {
                    ms_snap2_base = hb;
                    ms_snap2_valid = 1;
                    ms_log("hash snapshot taken at %#llx", (unsigned long long)hb);
                }
            }
            continue;
        }
        usleep(500000);
    }
    return NULL;
}

static void *scan_thread(void *unused) {
    (void)unused;
    sleep(12); /* let the world load */
    char trigger[PATH_MAX];
    trigger_path(trigger, sizeof trigger);
    for (;;) {
        struct stat st;
        int requested = stat(trigger, &st) == 0;
        if (requested) remove(trigger);
        if (!load_save_anchors()) { sleep(5); continue; }
        scan_memory();
        float xyz[3];
        int in_world = runtime_survey_player(xyz, NULL) && (xyz[0] * xyz[0] + xyz[2] * xyz[2] > 4.0f);
        if (!in_world) {
            ms_log("menu placeholder position; watching skipped, waiting for trigger/next scan");
        } else if (getenv("TOTK_MEMSCAN_NOWATCH") && *getenv("TOTK_MEMSCAN_NOWATCH")) {
            ms_log("probe-only scan done; waiting for next trigger");
        } else {
            if (ms_have_target && ms_target_hit_count > 0) {
                /* Pick the hit whose neighbourhood holds the expected world position (the actor struct). */
                uintptr_t best = 0;
                for (int i = 0; i < ms_target_hit_count; i++) {
                    uintptr_t at = (uintptr_t)ms_target_hits[i];
                    unsigned char page[4096];
                    mach_vm_size_t got2 = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(at & ~(uintptr_t)0xFFF),
                                               (mach_vm_size_t)sizeof page, (mach_vm_address_t)(uintptr_t)page, &got2) != KERN_SUCCESS || got2 != sizeof page)
                        continue;
                    for (int f = 0; f + 12 <= (int)sizeof page; f += 4) {
                        float fx, fy, fz;
                        memcpy(&fx, page + f, 4); memcpy(&fy, page + f + 4, 4); memcpy(&fz, page + f + 8, 4);
                        if (!ms_have_target_xyz) { best = at; break; }
                        double dx = fx - ms_target_xyz[0], dy = fy - ms_target_xyz[1], dz = fz - ms_target_xyz[2];
                        if (dx * dx + dy * dy + dz * dz < 25.0) {
                            best = at; break;
                        }
                    }
                    if (best) break;
                }
                if (best) {
                    ms_log("target actor candidate %#llx", (unsigned long long)best);
                    watch(best & ~(uintptr_t)0xFFF, "target");
                } else {
                    ms_log("target id found but no position match");
                    watch((uintptr_t)ms_target_hits[0] & ~(uintptr_t)0xFFF, "target");
                }
            } else {
                choose_hash_window();
                uintptr_t table = choose_flag_table();
                if (table) watch(table, "flagtable");
                if (ms_guid_hit_count > 0) watch(ms_guid_hits[0].address, "guidlist");
            }
        }
        /* No watch started (nothing found): retry after a while or when triggered. */
        for (int i = 0; i < 30; i++) {
            sleep(2);
            if (stat(trigger, &st) == 0) { remove(trigger); break; }
        }
    }
    return NULL;
}

void runtime_memscan_start(void) {
    const char *on = getenv("TOTK_MEMSCAN");
    if (!on || !*on || !strcmp(on, "0")) return;
    const char *snapshot = getenv("TOTK_MEMSCAN_SNAPSHOT");
    pthread_t thread;
    if (snapshot && *snapshot && strcmp(snapshot, "0") != 0) {
        if (pthread_create(&thread, NULL, snapshot_thread, NULL) == 0) pthread_detach(thread);
        ms_log("starting target snapshot mode (read-only)");
    } else {
        if (pthread_create(&thread, NULL, scan_thread, NULL) == 0) pthread_detach(thread);
        ms_log("starting diagnostic scan (read-only)");
    }
}
#endif
