#pragma once
#include "runtime.h"

/* Built-in local HTTP/SSE gateway (gateway.c): starts with the game, serves the same read-only contract as the
   Python bridge (docs/game-interface.md), and stops when the game exits. No Python, no extra process, no command
   for the player. Loopback only; disable with TOTK_GATEWAY=0. A discovery file (<data>/gateway.json) tells external
   programs the actual URL/port. */
void runtime_gateway_start(void);
void runtime_gateway_stop(void);
/* Called by runtime/maplink.c on every sample: the latest position JSON (one line) becomes the served snapshot. */
int runtime_gateway_publish(const char *json, size_t length);
int runtime_gateway_port(void);
int runtime_gateway_latest_position(double xyz[3]);
