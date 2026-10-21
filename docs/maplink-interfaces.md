# Map link interfaces — plan v1 (frozen for P1)

> Status: **planned**. Decisions below were made 2026-10-21. The game-side exporter exists as an **uncommitted prototype**
> (working, built, off-switch `TOTK_MAPLINK=0`); the bridge and consumers are **design only** — no code written yet.

Goal: any of the player's projects — the Zelda Wiki map, a Python script, an OBS overlay, a phone viewer — reads the
player's live position through one small, stable contract, without knowing anything about the game runtime.

## 0. Approved decisions

| Decision | Choice | Consequence |
|---|---|---|
| Primary interface | **B: local bridge daemon `maplinkd`** | every project reads one HTTP/SSE contract; direct file read (C) stays as a fallback for a single project |
| P1 scope | **position + heading + status** | no trail, no layer classification, no auto layer switching in P1 |
| Reverse control (map → game) | **design only, not built** | the write path stays behind the existing test-profile gates; documented in §7 |
| Bridge implementation | Python 3 standard library, single file, launchd agent | no dependencies, independent of the map project; portable to Node later |
| Safety | read-only, loopback only, env-gated | nothing writes into the game; `TOTK_MAPLINK=0` disables the producer |

## 1. Interface families

| # | Interface | Direction | Transport | Who consumes it | Status |
|---|---|---|---|---|---|
| A1 | Position file (`maplink.json`) | game → local disk | JSON, atomic rename, 10 Hz | the bridge; a single project that wants no daemon | prototype done |
| A2 | Position datagrams (optional, P4) | game → localhost | UDP 127.0.0.1:17861, JSON line per sample | low-latency tools | design |
| **B** | **Bridge `maplinkd`** | local → local | HTTP on 127.0.0.1:17862 + SSE | **every project** | **implemented (P0)** |
| C | Direct file read | game → project | direct read of A1 | Zelda Wiki alternative, scripts | design |
| D | Reverse control (P3, not now) | project → game | command channel | map click → in-game marker/teleport | out of scope |

## 2. Data contract v1 (shared by A1 and B)

```json
{
  "schema": "macnativegames.maplink/1",
  "ok": true,
  "inWorld": true,
  "status": "live",
  "ageMs": 84,
  "frame": 123456,
  "t": 1761000000123,
  "game": "totk",
  "version": "1.4.3",
  "x": 1234.5, "y": 123.4, "z": -567.8,
  "yaw": 90.0,
  "headingSource": "actor",
  "layer": null,
  "rot": [0, 0, 1, 0, 1, 0, -1, 0, 0]
}
```

| Field | Type | Meaning |
|---|---|---|
| `schema` | string | `macnativegames.maplink/1`; consumers reject other majors |
| `ok` | bool | `true` only in world (world frame seen **and** a player actor found; the menu's placeholder player is excluded) |
| `inWorld` | bool | the producer's world-ready flag, passed through |
| `status` | enum | bridge-added: `live` \| `not-in-world` \| `stale` \| `disconnected` |
| `ageMs` | int | bridge-added: ms since `t` |
| `frame` | int | game present counter at sampling |
| `t` | int | host Unix milliseconds |
| `game` / `version` | string | `totk` / `1.0.0`\|`1.1.2`\|`1.2.1`\|`1.4.3` |
| `x` / `y` / `z` | float (m) | world position; `y` is height |
| `yaw` | float (deg) | heading, 0 = +Z, 90 = +X, clockwise seen from above. **To be verified in game (P1 task)** |
| `headingSource` | enum | `actor` (from `rot`) \| `motion` (derived from Δposition) \| `unknown` |
| `layer` | int \| null | P1: always `null`; P2: `1` sky, `0` surface, `-1` Depths |
| `rot` | float[9] | raw actor rotation, row-major (calibration source) |

Status rules: `live` = `ok` and `ageMs ≤ 1000`; `stale` = file exists but older; `disconnected` = file missing or
producer gone > 10 s; `not-in-world` = `ok:false`.

## 3. Interface B — `maplinkd` endpoints (P1 set, frozen)

`GET /v1/meta`
```json
{"schema":"macnativegames.maplink/1","bridge":"1.0.0","rateHz":10,"producer":"file",
 "capabilities":{"trail":false,"layer":false,"reverse":false},"uptimeMs":4321000}
```

`GET /v1/state` — the data contract above, always HTTP 200.

`GET /v1/stream` — Server-Sent Events:
```
event: hello   data: {"schema":"macnativegames.maplink/1","bridge":"1.0.0","rateHz":10}
event: state   data: {...}
event: status  data: {"status":"stale","ageMs":2300}
```
Heartbeat `: ping` every 15 s; clients reconnect after 1000 ms.

`GET /v1/health` — `200` when the producer is fresh, `503 {"ok":false,"reason":"producer-missing"}` otherwise.

Common: `Content-Type: application/json; charset=utf-8`, `Cache-Control: no-store`, CORS allowlist default
`http://localhost:3000` + `http://127.0.0.1:3000`. No write endpoints.

Reserved for P2/P3 (documented, not served in P1): `GET /v1/trail?seconds=…`, reverse-control endpoints.

## 4. Heading (P1 deliverable detail)

The actor rotation gives a candidate `yaw`; it is not yet verified against the game. P1 therefore includes a short
in-game calibration and a defined fallback:

1. sample `yaw` while walking in four cardinal directions, compare with `atan2(Δx, Δz)`;
2. if the error is a constant offset/sign, fix the formula in the producer (one-line change + rebuild);
3. if it is unstable, the bridge derives the heading from consecutive positions and reports `headingSource:"motion"`;
   the arrow still works, with a small lag.

Acceptance: walking north on the map moves the arrow north within one sample (100 ms), including after a pause.

## 5. Consumers

| Consumer | Reads | P1 work |
|---|---|---|
| Zelda Wiki (Next.js) | B via same-origin BFF `/api/player/stream` (proxy SSE, no CORS), fallback C | marker + heading arrow + follow toggle + status pill |
| Python / scripts | B `/v1/state` or A1 | none |
| OBS browser source | B `/v1/stream` | none (plain EventSource) |

The map projection stays in the consumer: `gameCoordinatesToMapCoordinate({x,z})` with the existing calibration
(`pixelsPerGameUnit = 2`, origin at the centre of the 24000×2000 raster) is already correct for markers.

## 6. Phases

| Phase | Scope | Acceptance | Estimate |
|---|---|---|---|
| **P0** | Freeze contract; fixtures (live/stale/not-in-world/disconnected); `maplinkd` with contract tests | fixtures drive the bridge; a fake consumer asserts every status; no game needed | 0.5 d |
| **P1** | Zelda Wiki: BFF route + marker + heading + follow + status; in-game yaw calibration | game loaded: marker follows within 200 ms, arrow matches movement; closing the game turns the pill `disconnected` within 2 s; `TOTK_MAPLINK=0` leaves the page in `disconnected` | 1 d |
| P2 | Trail (`/v1/trail`), layer classification (surface/Depths/sky), auto layer switch | path drawn; layer switches within 1 s of a transition | 1 d |
| P3 | Reverse control (marker / optional teleport) | opt-in, backup-first, test-profile gate | 1–2 d + review |
| P4 | BotW, UDP producer (A2), LAN/mobile viewer | as requested | — |

## 7. Risks and rules

- Read-only by construction: the producer copies floats out of guest memory; it never writes. Teleport is the only write
  path and stays behind the existing test-profile/save-backup gates (not built).
- Version coupling: actor chain verified for 1.4.3; other versions have constants but are untested here.
- Rebuild cost: every runtime change costs ~3 min (cached) to ~17 min (cold) plus the macOS scan.
- Upstream style: environment-gated, documented, default-off policy respected via `TOTK_MAPLINK=0`.
- Privacy: loopback only; payload contains no save data, no keys, no paths beyond the local data folder.

## 8. Current state

| Item | State |
|---|---|
| Producer `runtime/maplink.c` + hooks + launcher default | committed on branch `totk-to-wiki`, built into `/Applications/Tears of the Kingdom.app` (1.4.3) |
| Bridge `maplinkd` | **implemented** in `tools/maplinkd/`: `/v1/meta`, `/v1/state`, `/v1/stream` (SSE), `/v1/health`; 12 contract tests pass; launchd installer included |
| Docs | this file + `docs/maplink.md` (producer contract) + `tools/maplinkd/README.md` |
| Zelda Wiki changes | none yet (branch `totk-to-wiki` created); reference patch in `/tmp/maplink-wiki-preview/` |
