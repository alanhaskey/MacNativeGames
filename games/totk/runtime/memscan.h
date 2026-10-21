#pragma once
#include "runtime.h"

/* Diagnostic only (TOTK_MEMSCAN=1): read a few stable 8-byte IDs from the newest save's processed-object blob,
   look for them in this process's memory, and log changes at the best candidate while the player plays. Read-only;
   no game code runs differently. Used to design the live chest/object event interface. */
void runtime_memscan_start(void);
