#pragma once
#include "runtime.h"
/* Instant collection events for GUID-tracked objects (chests, bubbuls, Addison signs, sage wills).
   A background thread locates the in-memory sorted GUID array (the same values the save stores) and watches it;
   when a collection inserts a GUID, an event is published for the gateway's SSE endpoint. Read-only. */
void runtime_progress_events_start(void);
/* Copy the next event after *cursor into json (NUL-terminated). Returns 1 when an event was copied, 0 when none.
   *cursor is advanced. Safe to call from the gateway client thread. */
int progress_events_next(long long *cursor, char *json, size_t capacity);
