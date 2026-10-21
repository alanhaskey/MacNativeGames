/* Diagnostic memory scanner (env TOTK_MEMSCAN=1, off by default).

   Finds the in-memory "processed object" list by looking for a few stable 8-byte IDs that the save blob
   (progress.sav, 0x0D9510) carries, then watches the best candidate region and logs every change together with
   the player position. Read-only: only mach_vm_read_overwrite against our own task. Used to design the live
   object-completion event interface (docs/collection-sync-research.md). */
#include "memscan.h"
#include "game_version.h"
#include "world_survey.h"
#include <dirent.h>
#include <limits.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

#ifdef GAME_BOTW
void runtime_memscan_start(void) { }
#else
#include "mach_vm_compat.h"

#define SCAN_IDS 4
#define SCAN_CHUNK (4u << 20)
#define MAX_SCAN_BYTES ((uint64_t)12 << 30)

static uint64_t scan_ids[SCAN_IDS];
static FILE *ms_file;

static void ms_log(const char *format, ...) {
    if (!ms_file) {
        const char *profile = getenv("TOTK_PROFILE_DIR");
        if (profile && *profile) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/../memscan.log", profile);
            ms_file = fopen(path, "a");
        }
    }
    va_list arguments;
    va_start(arguments, format);
    if (ms_file) { vfprintf(ms_file, format, arguments); fflush(ms_file); }
    va_end(arguments);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}
static uintptr_t watch_base;
static int watch_ready;

static int read_mem(uintptr_t address, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address, (mach_vm_size_t)size,
                                  (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == size;
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

/* The blob starts with 8-byte IDs at 0x0D9510; take a few of them as scan keys. */
static int load_ids(void) {
    char path[PATH_MAX];
    if (!find_newest_save(path, sizeof path)) {
        ms_log("[memscan] no save found under TOTK_PROFILE_DIR\n");
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char blob[0x80];
    if (fseek(f, 0x0D9510, SEEK_SET) != 0 || fread(blob, 1, sizeof blob, f) != sizeof blob) { fclose(f); return 0; }
    fclose(f);
    for (int i = 0; i < SCAN_IDS; i++) memcpy(&scan_ids[i], blob + i * 8, 8);
    ms_log("[memscan] keys from %s: %016llx %016llx %016llx %016llx\n", path,
            (unsigned long long)scan_ids[0], (unsigned long long)scan_ids[1],
            (unsigned long long)scan_ids[2], (unsigned long long)scan_ids[3]);
    return 1;
}

static uintptr_t hits[SCAN_IDS][64];
static int hit_counts[SCAN_IDS];

static void scan_region(uintptr_t start, uintptr_t size) {
    if (size == 0) return;
    unsigned char *buffer = malloc(SCAN_CHUNK < size ? SCAN_CHUNK : size);
    if (!buffer) return;
    for (uintptr_t offset = 0; offset < size && offset < MAX_SCAN_BYTES; offset += SCAN_CHUNK) {
        size_t want = (size_t)((size - offset) < SCAN_CHUNK ? (size - offset) : SCAN_CHUNK);
        if (!read_mem(start + offset, buffer, want)) continue;
        for (int i = 0; i < SCAN_IDS; i++) {
            if (hit_counts[i] >= 64) continue;
            for (size_t p = 0; p + 8 <= want; p += 8) {
                uint64_t value;
                memcpy(&value, buffer + p, 8);
                if (value == scan_ids[i]) hit_counts[i]++, hits[i][hit_counts[i] - 1] = start + offset + p;
            }
        }
    }
    free(buffer);
}

static void scan_memory(void) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count;
    natural_t depth = 0;
    uint64_t scanned = 0;
    while (true) {
        count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
        if (info.is_submap) { depth++; continue; }
        if ((info.protection & VM_PROT_READ) && size > 0 && scanned < MAX_SCAN_BYTES) {
            scan_region((uintptr_t)address, (uintptr_t)size);
            scanned += size;
        }
        address += size;
        depth = 0;
    }
    ms_log("[memscan] scanned %.1f GB: hits %d/%d/%d/%d\n", scanned / 1e9,
            hit_counts[0], hit_counts[1], hit_counts[2], hit_counts[3]);
}

/* Pick the address where the most of the known IDs sit close together (the list), then watch 256 bytes there. */
static void choose_watch(void) {
    int best_score = 0;
    for (int i = 0; i < hit_counts[0]; i++) {
        uintptr_t base = hits[0][i];
        int score = 1;
        for (int j = 1; j < SCAN_IDS; j++) {
            for (int k = 0; k < hit_counts[j]; k++) {
                uintptr_t other = hits[j][k];
                uintptr_t delta = other > base ? other - base : base - other;
                if (delta < 0x1000) { score++; break; }
            }
        }
        if (score > best_score) { best_score = score; watch_base = base; }
    }
    if (best_score >= 2) {
        watch_ready = 1;
        ms_log("[memscan] watch %#llx (score %d of %d)\n", (unsigned long long)watch_base, best_score, SCAN_IDS);
    } else {
        ms_log("[memscan] no list candidate found (best score %d)\n", best_score);
    }
}

static void *scan_thread(void *unused) {
    (void)unused;
    sleep(15);   /* let the world load so the list exists */
    if (!load_ids()) return NULL;
    scan_memory();
    choose_watch();
    if (!watch_ready) return NULL;
    unsigned char previous[256];
    if (!read_mem(watch_base, previous, sizeof previous)) return NULL;
    unsigned long long changes = 0;
    for (;;) {
        usleep(100000);
        unsigned char current[256];
        if (!read_mem(watch_base, current, sizeof current)) continue;
        int first = -1, differing = 0;
        for (int i = 0; i < 256; i++) {
            if (current[i] != previous[i]) { if (first < 0) first = i; differing++; }
        }
        if (differing) {
            float xyz[3];
            int have = runtime_survey_player(xyz, NULL);
            ms_log("[memscan] change=%llu off=%d bytes=%d pos=%s%.1f,%.1f,%.1f\n",
                    ++changes, first, differing, have ? "" : "?", have ? xyz[0] : 0.0, have ? xyz[1] : 0.0, have ? xyz[2] : 0.0);
            memcpy(previous, current, sizeof previous);
        }
    }
    return NULL;
}

void runtime_memscan_start(void) {
    const char *on = getenv("TOTK_MEMSCAN");
    if (!on || !*on || !strcmp(on, "0")) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, scan_thread, NULL) == 0) pthread_detach(thread);
    ms_log("[memscan] starting (diagnostic, read-only)\n");
}
#endif
