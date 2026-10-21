#pragma once
#include "runtime.h"
#include <stdatomic.h>

/* Test-profile-only control, executed on the game's frame-end thread after its GPU fence. */
extern _Atomic int runtime_survey_pending;
extern const int32_t runtime_survey_sync[2];
void runtime_survey_init(Module *main_module);
/* Read-only player state for integrations (runtime/maplink.c): world position and the actor's 3x3 rotation copied out of
   guest memory. No writes, no test-profile requirement, never runs the forceSetMatrix path. Returns 1 when a player actor
   was found and the values passed the range check, 0 otherwise (title screen, loading, menus). rot may be NULL. */
int runtime_survey_player(float xyz[3], float rot[9]);
int runtime_survey_command(const char *line);
void runtime_survey_tick(uint64_t frame);
void runtime_survey_cancel_user(void);
