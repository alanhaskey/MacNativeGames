# Map link: the player's position for an external map

An optional, **read-only** output for interactive-map projects: a few times a second the game writes the player's world
position to a small JSON file. Nothing is written into the game, no gameplay state is touched, and the feature does not
gate on a test profile (unlike `TOTK_SURVEY`, which can also move the player).

## Enabling it

The launcher sets `TOTK_MAPLINK_FILE` to `~/Library/Application Support/MacNativeGames/TOTK/maplink.json` and the game
writes it by itself. To turn it off:

```sh
open --env TOTK_MAPLINK=0 -a "Tears of the Kingdom"
```

Other knobs:

| Variable | Meaning |
|---|---|
| `TOTK_MAPLINK_FILE` | where to write; must not be empty for the feature to run |
| `TOTK_MAPLINK` | `0` / `off` disables it even when the file is named |
| `TOTK_MAPLINK_FRAME_STEP` | sample period in frames (default 6 → 10 Hz at 60 fps) |

The file is replaced atomically (`write` to `<name>.tmp`, then `rename`), so a reader never sees a half-written file.
The log line `[maplink] position file <path>, one sample every N frames` confirms it started.

## File contents

```json
{"schema":"macnativegames.maplink/1","ok":true,"inWorld":true,"game":"totk","version":"1.4.3",
 "frame":12345,"t":1761000000123,
 "x":1234.5,"y":123.4,"z":-567.8,"yaw":90.0,
 "rot":[0.0,0.0,1.0, 0.0,1.0,0.0, -1.0,0.0,0.0],"layer":null}
```

| Field | Meaning |
|---|---|
| `ok` | `true` only in the world: the game has presented a real world frame and a player actor was found. `false` on the title screen, in the main menu, while loading and in menus; then only `schema`, `inWorld`, `game`, `version`, `frame`, `t` are present |
| `inWorld` | true once the game is really in the world: the first-world-frame draw-count gate fired, **or** the player left the main menu's placeholder position (the origin). The menu keeps a placeholder player actor at (0, -0.15, 0); this flag keeps it out of the map |
| `frame` | the game's present counter when the sample was taken |
| `t` | host wall clock in Unix milliseconds; compare with the current time to detect a stale file |
| `x` / `y` / `z` | player world position in metres; `y` is height (surface around 0, the Depths below, the sky above) |
| `yaw` | heading in degrees, 0 = +Z, 90 = +X, increasing clockwise when seen from above (north-up map). **Not yet verified in game**; `rot` is the raw source if the sign or axis turns out different |
| `rot` | the actor's row-major 3×3 rotation, as read from guest memory |
| `layer` | reserved: `null` for now. Later 0 = surface, 1 = sky, -1 = Depths |

The position is read on the present path, after the frame is submitted, and it is the same verified actor lookup the
world survey uses (`runtime/world_survey.c`), so no extra reverse engineering is needed per game version.

## Consuming it

Any local program can read the file and forward it to a map. The intended setup:

1. a small server route reads `maplink.json` (or the map project tails it),
2. the browser receives it over SSE/WebSocket or polls,
3. the map converts `(x, z)` to its own coordinates — for the Object Map, `pixelsPerGameUnit = 2` with the origin at the
   centre of the 24000×20000 raster (see the map project's `totk-map-calibration.json`).

`rot`/`yaw` are provided so a heading arrow can be drawn; if the arrow points the wrong way, derive the heading from
consecutive positions instead (`atan2(Δx, Δz)`) and report the discrepancy, or flip the sign in the consumer.

## Status

First version, 1.4.3 Mac build. Verified end to end on 2026-10-21:

- the main menu reports `ok:false, inWorld:false` (its placeholder player at the origin is not published);
- loading a save turns it to `ok:true` with real coordinates, and the bridge serves `status:"live"` (sample age ~30 ms);
- `yaw` tracked the movement direction within about 10° across a scripted cutscene (0 = +Z, 90 = +X), so the
  convention looks right, but it has not been checked in free gameplay yet.

Known open items:

- `yaw` needs one free-gameplay check (walk four directions) before a map treats it as exact.
- `layer` is not classified yet; consumers should treat it as unknown.
- Breath of the Wild has no player actor layout in this runtime yet and reports nothing.
