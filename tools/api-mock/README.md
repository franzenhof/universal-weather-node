# Mock Weather Node

A stand-in for a Universal Weather Node's read-only HTTP API, so you can build and test a display or app without hardware. Python 3 standard library only — nothing to install.

[← API guide](../../docs/api.md)

## Run

```bash
python3 tools/api-mock/mock_server.py                                  # http://127.0.0.1:8080
python3 tools/api-mock/mock_server.py --fixture pcb-v2-imperial --port 9000
python3 tools/api-mock/mock_server.py --host 0.0.0.0 --cycle 300       # reachable from the LAN, real cycle length
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--fixture` | `bresser-7in1-bme280` | Which setup from [`docs/api/examples/`](../../docs/api/examples/) to serve: `bresser-7in1-bme280`, `ecowitt-ws90-wh31-wh51-wh57`, `pcb-v2-full`, `pcb-v2-imperial`, `heltec-v4-wireless` |
| `--port` | `8080` | TCP port |
| `--host` | `127.0.0.1` | Bind address (`0.0.0.0` = other devices in the LAN may connect) |
| `--cycle` | `30` | Measurement cycle in seconds — a real node uses 300; shorter makes changes visible sooner. `0` = the fixture's value. `/api/info` reports the value used |
| `--seed` | random | Seed for reproducible drift |
| `--quiet` | off | No request log |

Then point your UI at `http://localhost:8080`, e.g. the example display: open [`examples/simple-display/index.html`](../../examples/simple-display/index.html) with `?node=http://localhost:8080`, or simply open `http://localhost:8080/` — the mock serves that page at `/` (same origin).

## How It Mirrors the Real Device

- **Same endpoints and documents:** `GET /api/info`, `/api/data`, `/api/history?range=24h|7d` (anything else → `24h`), starting from the example responses that the [contract test](../../docs/api.md#contract-test) generates with the real firmware serializers — same fields, units, absent fields and `null`s for the chosen setup.
- **Same headers:** `Access-Control-Allow-Origin: *` and `Cache-Control: no-store` on the three endpoints, `OPTIONS` preflight answered with `204` and the same `Access-Control-*` headers, `404` for unknown paths.
- **Measurement cycles:** values change only once per cycle; `nextCycleInSec` counts down to the next one, so a client that polls like [recommended](../../docs/api.md#polling) behaves as against a node.
- **Drift:** temperature, humidity and pressure follow a bounded random walk, wind speed/gust/direction vary (gusts 1.3–1.9 × the mean), DS18B20, UV and light drift; derived values (dew point, VPD, cloud base, warnings, compass text, pressure tendency) are recomputed. Occasional showers add rain in 0.2-mm tips and keep `rain`, `rainRate`, `rainSinceStart`, `rain1h`, `rain24h`, `rainToday` … `rainYear`, `lastRain` and the source sensor's `rainTotal` consistent.
- **History:** the example history is shifted to "now"; every cycle updates the open bucket (ageSec 0); like on the node, buckets are fixed `bucketSec` windows (5 min / 1 h) and a cycle in a later window closes the open one (windows without a cycle become gaps), oldest buckets drop out at the ring capacity.
- **Wireless sensors:** `ageSec` cycles between 0 and 16 s (a packet every ~16 s); a sensor listed as `stale` keeps ageing.
- Number precision is limited to about 7 significant digits like the node's 32-bit floats.

## Limitations

- Not a weather model: the drift is random, there is no day/night cycle, and the mock's `rain1h`/`rain24h` only approximate rolling sums.
- `tempTrend1h`, sunshine, Victron, heater/fan and AUX values stay as in the example, `feelsLike` simply follows the temperature and `pressureTrend3h` only drifts slightly.
- Fields never appear or disappear at run time (on a node they do: a sensor goes silent, the clock syncs, 3 h of history fill up) — test those cases with the different fixtures, especially `heltec-v4-wireless`.
- No restart (uptime only grows, the history never resets), no low-memory `500`.
- `/api/settings` and `/api/log*` answer `403`; the dashboard (`/`) is replaced by the example display; `/status`, `/help`, MQTT and mDNS are not provided.
- Single process, plain HTTP; for development only.
