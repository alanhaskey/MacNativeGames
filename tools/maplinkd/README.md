# maplinkd — local HTTP/SSE bridge for the map link (development / reference implementation)

> **Players do not need this.** The game app now serves the same contract itself from inside the game process
> (`games/totk/runtime/gateway.c`): it starts with the game, writes `<data>/gateway.json`, and stops with the game.
> This Python bridge is kept for fixtures, tests and tools that run without the game. If the game is running, it owns
> 17862 first; pass `--port` to run this bridge next to it.

The game writes the player's position to `~/Library/Application Support/MacNativeGames/TOTK/maplink.json`
(10 Hz, atomic replace; see [docs/maplink.md](../../docs/maplink.md)). `maplinkd` turns that file into one stable
contract any local project can read: the Zelda Wiki map, a Python script, an OBS browser source, a phone viewer.

- read-only, loopback only (`127.0.0.1:17862`), no dependencies (Python 3.9+, standard library only)
- one producer file in, four endpoints out; the game exiting becomes a `status`, not an error

## Run it

```sh
python3 tools/maplinkd/maplinkd.py                    # 127.0.0.1:17862, default file
python3 tools/maplinkd/maplinkd.py --file /tmp/maplink.json --port 17862 --verbose
```

Install it as a login agent (survives reboots, restarts on crash):

```sh
tools/maplinkd/install_launchd.sh install
tools/maplinkd/install_launchd.sh status
tools/maplinkd/install_launchd.sh uninstall
```

## Endpoints

| Endpoint | Returns |
|---|---|
| `GET /v1/meta` | bridge version, `rateHz`, capabilities (`trail`/`layer`/`reverse` all false in P1) |
| `GET /v1/state` | the current snapshot; **always 200**, `status` says what is happening |
| `GET /v1/stream` | SSE: `hello`, `state` per sample, `status` on change, `: ping` heartbeat every 15 s |
| `GET /v1/health` | 200 while `status == live`, otherwise 503 |

```sh
curl -s http://127.0.0.1:17862/v1/state | python3 -m json.tool
curl -N http://127.0.0.1:17862/v1/stream        # follow the live stream
```

```js
const source = new EventSource("http://127.0.0.1:17862/v1/stream");   // CORS: localhost:3000 allowed
source.addEventListener("state", (event) => {
  const player = JSON.parse(event.data);
  if (player.status === "live") draw(player.x, player.z, player.yaw);
});
```

## State contract

```json
{"schema":"macnativegames.maplink/1","ok":true,"inWorld":true,"status":"live","ageMs":84,
 "frame":123456,"t":1761000000123,"game":"totk","version":"1.4.3",
 "x":1234.5,"y":123.4,"z":-567.8,"yaw":90.0,
 "headingSource":"actor","layer":null}
```

| `status` | Meaning |
|---|---|
| `live` | in world and the sample is at most `--stale-ms` old (default 1000 ms) |
| `not-in-world` | the game is on the title screen, in the main menu, or loading (`ok:false`; `inWorld` is false) |
| `stale` | the file exists but the newest sample is old; the fields are the last known values |
| `disconnected` | the file is missing, unreadable, or the producer has been silent for `--disconnect-ms` (default 10 s) |

`reason` is added for `disconnected` (`producer-missing`, `producer-silent`, `producer-invalid: …`). Coordinates are
game coordinates; mapping them to pixels or lng/lat is the consumer's job.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--file` | `~/Library/Application Support/MacNativeGames/TOTK/maplink.json` (or `TOTK_MAPLINK_FILE`) | producer file |
| `--host` / `--port` | `127.0.0.1` / `17862` | keep it on loopback |
| `--stale-ms` / `--disconnect-ms` | `1000` / `10000` | status thresholds |
| `--cors-origin` | `http://localhost:3000`, `http://127.0.0.1:3000` | repeatable; browser origins allowed to read the API |
| `--verbose` | off | log requests to stderr |

## Tests

```sh
python3 -m unittest discover -s tools/maplinkd/tests -v
```

The suite writes fixtures and drives the real HTTP server: missing file, live, not-in-world, stale, silent producer,
invalid JSON, wrong schema, meta, CORS allow/deny, 404 and the SSE hello/state handshake.

## Notes

- Bind to loopback only. The payload contains the position and game version, nothing from saves or keys.
- `maplinkd` never writes to the game; it only reads the producer file.
- If the port is busy (`EADDRINUSE`), either stop the other instance (`launchctl bootout gui/$UID/com.macnativegames.maplinkd`)
  or pass `--port`.
- The bridge is independent of the game process: start it any time, it goes `disconnected` → `live` when the game starts
  writing, and keeps the bridge's ring of consumers untouched.
