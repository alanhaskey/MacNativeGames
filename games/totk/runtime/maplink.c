/* Read-only map link: write the player's world position to a small JSON file so an external interactive map can follow
   the player in real time (TOTK 1.0.0 / 1.1.2 / 1.2.1 / 1.4.3, Mac; BotW has no player actor layout here and reports
   nothing). The position comes from the same verified actor lookup the survey uses (world_survey.c, read-only), sampled
   on the present path after a frame is submitted, and written with rename() so a reader never sees a half-written file.

   The file is off unless TOTK_MAPLINK_FILE names it (the launcher sets it to <game data>/maplink.json). TOTK_MAPLINK=0
   disables it. TOTK_MAPLINK_FRAME_STEP sets the sample period in frames (default 6, i.e. 10 Hz at 60 fps).

   {"schema":"macnativegames.maplink/1","ok":true,"game":"totk","version":"1.4.3","frame":N,"t":<unix ms>,
    "x":..,"y":..,"z":..,"yaw":<degrees, 0 = +Z, 90 = +X>,"rot":[9 floats],"layer":null}

   With no player (title screen, loading, menus) the file carries ok=false and no position. Nothing here changes the
   game, and no failure of this file can stop it. */
#include "maplink.h"
#include "gateway.h"
#include "world_survey.h"
#include "game_version.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef GAME_BOTW
void runtime_maplink_init(Module *m) { (void)m; }
void runtime_maplink_frame(uint64_t frame) { (void)frame; }
#else
/* hle_nvn.c: set when the first real world frame is presented (draw-count gate). It is a useful extra signal but not
   reliable for every scene, so the producer also treats a player that has left the main menu's placeholder position
   (the origin, identity rotation) as "in world"; that latch is what the map needs. */
extern int runtime_world_ready(void);
static int world_latch;
static int interior;                 /* inside a separate scene (shrine/divine beast): local coordinates near the origin */
static int have_stable;
static float stable_xyz[3];
static double stable_yaw;
static char path[PATH_MAX];
static int enabled, step = 6;
static uint64_t next_frame;

void runtime_maplink_init(Module *m) {
    (void)m;
    const char *off = getenv("TOTK_MAPLINK");
    if (off && (!strcmp(off, "0") || !strcmp(off, "off"))) return;
    const char *p = getenv("TOTK_MAPLINK_FILE");
    if (!p || !*p || strlen(p) >= sizeof path) return;
    strcpy(path, p);
    const char *s = getenv("TOTK_MAPLINK_FRAME_STEP");
    if (s) { int v = atoi(s); if (v >= 1 && v <= 600) step = v; }
    enabled = 1;
    fprintf(stderr, "[maplink] position file %s, one sample every %d frames\n", path, step);
}

static long long wall_ms(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts) == 0 ? (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 : 0;
}

static void write_json(const char *json) {
    char temporary[PATH_MAX + 8];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return;
    FILE *f = fopen(temporary, "w");
    if (!f) return;
    int ok = fputs(json, f) >= 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) rename(temporary, path);
    else remove(temporary);
}

void runtime_maplink_frame(uint64_t frame) {
    if (!enabled || frame < next_frame) return;
    next_frame = frame + (uint64_t)step;
    char json[640];
    long long t = wall_ms();
    float xyz[3], rot[9];
    int found = runtime_survey_player(xyz, rot);
    if (found && (fabsf(xyz[0]) > 0.05f || fabsf(xyz[2]) > 0.05f)) world_latch = 1;   /* the menu placeholder is (0, -0.15, 0) */
    int in_world = runtime_world_ready() || world_latch;
    if (found && in_world) {
        /* Actor rotation is a row-major 3x3; local forward +Z becomes the third column, so the world heading is
           atan2(forward.x, forward.z). Degrees, clockwise from +Z, which is what a north-up map wants. */
        double yaw = atan2((double)rot[2], (double)rot[8]) * 180.0 / M_PI;
        if (yaw < 0) yaw += 360.0;
        /* Shrines (and other separate scenes) use their own coordinate space centred on the origin: entering one
           teleports the raw player position from world coordinates to something like (0.6, -13, -24). A map cannot
           place that point, so remember the last world position and present it as frozen while the scene is local.
           Entry heuristic: a jump far too large for walking (>300 m between samples) into the small local area.
           Exit heuristic: the raw position leaves the local area again. */
        if (!interior && have_stable) {
            float dx = xyz[0] - stable_xyz[0], dz = xyz[2] - stable_xyz[2];
            if (dx * dx + dz * dz > 300.0f * 300.0f && fabsf(xyz[0]) < 400.0f && fabsf(xyz[2]) < 400.0f) {
                interior = 1;
                fprintf(stderr, "[maplink] interior scene detected; freezing world position %.1f, %.1f, %.1f\n",
                        stable_xyz[0], stable_xyz[1], stable_xyz[2]);
            }
        }
        if (interior && (fabsf(xyz[0]) > 400.0f || fabsf(xyz[2]) > 400.0f)) {
            interior = 0;
            fprintf(stderr, "[maplink] back in world at %.1f, %.1f, %.1f\n", xyz[0], xyz[1], xyz[2]);
        }
        if (!interior) {
            memcpy(stable_xyz, xyz, sizeof stable_xyz);
            stable_yaw = yaw;
            have_stable = 1;
        }
        float ox = interior ? stable_xyz[0] : xyz[0];
        float oy = interior ? stable_xyz[1] : xyz[1];
        float oz = interior ? stable_xyz[2] : xyz[2];
        double oyaw = interior ? stable_yaw : yaw;
        /* Layer from height until a game-state signal is found (see docs/next-features-plan.md): surface is roughly
           0..1200 m, the sky islands start above ~1300 m, the Depths are far below sea level. layerSource stays
           "height" so a consumer can also compare the raw y. */
        int layer = oy < -500.0f ? -1 : (oy > 1300.0f ? 1 : 0);
        char interior_part[160] = "";
        if (interior) {
            snprintf(interior_part, sizeof interior_part,
                     "\"interior\":true,\"positionSource\":\"frozen\",\"rawPosition\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},",
                     xyz[0], xyz[1], xyz[2]);
        }
        snprintf(json, sizeof json,
                 "{\"schema\":\"macnativegames.maplink/1\",\"ok\":true,\"game\":\"totk\",\"version\":\"%s\","
                 "\"frame\":%llu,\"t\":%lld,\"inWorld\":true,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"yaw\":%.1f,"
                 "\"rot\":[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f],"
                 "%s\"layer\":%d,\"layerSource\":\"height\"}\n",
                 TOTK_GAME_VERSION, (unsigned long long)frame, t,
                 ox, oy, oz, oyaw,
                 rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8],
                 interior_part, layer);
    } else {
        snprintf(json, sizeof json,
                 "{\"schema\":\"macnativegames.maplink/1\",\"ok\":false,\"game\":\"totk\",\"version\":\"%s\","
                 "\"frame\":%llu,\"t\":%lld,\"inWorld\":%s,\"layer\":null}\n",
                 TOTK_GAME_VERSION, (unsigned long long)frame, t, in_world ? "true" : "false");
    }
    runtime_gateway_publish(json, strlen(json));   /* served by the in-game gateway, no separate process */
    write_json(json);
}
#endif
