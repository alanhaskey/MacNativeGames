#pragma once
#include "runtime.h"

/* Optional read-only map link for the selected TOTK version (maplink.c): a small JSON file with the player's world
   position, written on the present path a few times a second so an external interactive map can follow the player.
   Nothing is written into the game and no gameplay state is touched. It is off unless the environment names a file
   (the launcher sets TOTK_MAPLINK_FILE to <data>/maplink.json; TOTK_MAPLINK=0 turns it off). */
void runtime_maplink_init(Module *main_module);
void runtime_maplink_frame(uint64_t frame);
