# Storage layout (firmware 4.x)

Frozen for 4.0.0 (work package B11, 2026-11-27). After the release, every change to what is listed here needs a migration — in the one place made for it (see [Migrations](#migrations)). The partition table cannot change at all over the air: it stays as it is for every 4.x release.

## Flash: partition table

`partitions.csv`, 16 MB (Heltec V4, ESP32-S3R2):

| Partition | Type | Offset | Size | Holds |
|-----------|------|--------|------|-------|
| `nvs` | data / nvs | 0x9000 | 20 KB | Wi-Fi state of the core / WiFiManager, PHY calibration (stock) |
| `otadata` | data / ota | 0xE000 | 8 KB | which app slot boots |
| `app0`, `app1` | app / ota_0, ota_1 | 0x10000, 0x610000 | 6 MB each | firmware (4.0.0: 2.7 MB) |
| `cfgdata` | data / nvs | 0xC10000 | **128 KB** | settings and small state — [below](#cfgdata-nvs) |
| `coredump` | data / coredump | 0xC30000 | **128 KB** | ESP-IDF core dump (ELF) of the last crash — [Crash report](#crash-report) |
| `history` | data / nvs | 0xC50000 | **1.5 MB** | chart history and custom series |
| `files` | data / spiffs | 0xDD0000 | **2.2 MB** | reserved, unused by 4.0.0 |

**Before B11:** `cfgdata` 64 KB, `coredump` 64 KB, `history` 3.8 MB (to the end of the flash), no `files`.

Why:

- **App slots** stay at 6 MB: the firmware is 2.7 MB, so there is room for twice as much (dashboard, assets) while OTA keeps two complete slots.
- **`cfgdata` 64 → 128 KB:** 4.0.0 uses about 12 KB (status: `/api/info` `system.settingsStoreUsed`), but templates, alarm rules, logical sensors, virtual sensors and the reset history all live here and only grow. NVS needs a free page for its garbage collection and wears less with room; 128 KB is cheap on a 16-MB flash and cannot be enlarged later.
- **`coredump` 64 → 128 KB:** the ELF dump holds every task's TCB and used stack (some 20 tasks, stacks up to 24 KB; the size of a real dump on the V4 is still to be measured). 64 KB is the stock size; a dump that does not fit is not written at all, and the table cannot change later — so twice the room.
- **`history` 3.8 → 1.5 MB:** the history needs at most 201 KB of live data (three tiers + 32 custom series, `src/history_store.cpp`). With 1.5 MB each 4-KB page is erased about 2.4 times a day at most (110+ years for 100 000 cycles).
- **`files` (new, 2.2 MB, reserved):** room for a file system later (dashboard assets, logs, larger traces) without touching the table — the only way to get one after the release. Subtype `spiffs` as LittleFS uses it; 4.0.0 never reads or writes it.

A node with the old table needs one USB flash of the `-factory` image (it erases the settings: backup first, restore after). An OTA upload of a `-factory` image with another table is refused (`src/ota_check.h`).

## cfgdata (NVS)

One NVS namespace per owner. Every own format carries a version (`src/storage_layout.h` is the list):

| Namespace / key | Content | Format | Version |
|-----------------|---------|--------|---------|
| `config` / one key per setting | `Config` (`src/config.cpp`: `loadConfigInto()` / `saveConfig()`) | NVS int / string / blob per key | key `cfgver` = `STORED_CONFIG_VERSION` (1) |
| `config` / `lsens` | logical sensor table (★ sensors) | blob, byte 0 = version, fixed records | `STORED_LOGICAL_SENSORS_VERSION` (1) |
| `config` / `netsens` | network sensor slots | blob [version][`NetSlotCfg` × 4] | `STORED_NETWORK_SENSORS_VERSION` (1) |
| `config` / `vsens` | virtual sensors (switch, power, 8 slots) | blob [version][on][dBm][text, one line per slot] | `STORED_VIRTUAL_SENSORS_VERSION` (1) |
| `config` / `series` | custom chart series | blob [version][text, one line per series] | `STORED_SERIES_VERSION` (1) |
| `config` / `https` | HTTPS certificate + private key (PEM; the key never leaves the node except in a backup with secrets) | blob [version][u16 LE cert length][u16 LE key length][cert][key] | `STORED_HTTPS_VERSION` (1) |
| `config` / `httpsgen` | the self-signed certificate + key the node generated (HTTPS without a usable uploaded pair; never in a backup) | same format as `https` | `STORED_HTTPS_VERSION` (1) |
| `config` / `dcus0`, `dcus1`, `rfcal`, `harfids`, `haseen`, … | custom OLED screens, other strings | NVS strings | with `cfgver` |
| `alarms` / `rules` | alarm rules | blob, magic `ALR2` + check | 2 (in the magic) |
| `alarms` / `chan` | notification channels | blob [version][`AlarmChannels`] | `STORED_ALARM_CHANNELS_VERSION` (1) |
| `cupload` / `cfg` | custom HTTP uploads | blob [version][`CustomUpload` × 2] | `STORED_CUSTOM_UPLOADS_VERSION` (1) |
| `records` / `v1` | records (min / max per period) | blob, magic `REC1` + check | 1 (in the magic) |
| `rainstat` / `periods` | rain / sunshine statistics (with *keep rain* on) | blob, u16 version + u16 size | 2 (`RAIN_BLOB_VERSION`) |
| `diag` / `resets` | reset history + last crash | blob [version][fixed little-endian fields] | `STORED_RESET_HISTORY_VERSION` (1) |

**Reset settings** (Settings → System, downlink `0xFF`, MQTT `factoryReset`; `src/settings_reset.h`) clears the namespaces `config` (incl. both HTTPS pairs, uploaded and generated, ★ table, series, virtual / network sensors), `alarms` and `cupload`, then writes back only the network access, admin password, language and LoRaWAN keys; `records`, `rainstat`, `diag`, the history partition, the core dump and the stock `nvs` stay. **Erase everything** (web only) erases `cfgdata`, `history`, `coredump` and the stock `nvs` partition (Wi-Fi, LoRaWAN session) and forgets the RTC RAM totals: the node starts like a new one. Both first **seal** the storage (`src/storage_write.h`): every own writer opens NVS through `StorageWrite`, which refuses after the seal; the seal waits for open sessions — no save racing a reset brings data back. See [settings.md#reset](../settings.md#reset).

**`history` partition:** namespace `history`, keys `fine` / `coarse` / `daily` — header `UWH` + version (`STORED_HISTORY_TIER_VERSION`, 4), then the samples and the custom series. Written every 30 min and before a planned restart.

**Settings backup file** (JSON, not flash): `"version": 2` (`SETTINGS_BACKUP_VERSION`); version 1 files (from 2.x / 3.x) are read without their radio-ID references.

**Wi-Fi** credentials are in the stock `nvs` partition (ESP-IDF's format).

**RTC RAM** (survives every reset but a power loss) is *not* part of the frozen layout: the linker places those variables (rain totals, rain since start, AUX pulse total, the reset heartbeat), so a firmware that adds one may move the others. Each carries a magic and a check, so a moved one is read as invalid once after such an update: the rain totals then come from their NVS copy, the reset history notes "uptime unknown" once.

## Migrations

`src/storage_layout.h` / `src/storage_migrate.cpp` — the one place where a later firmware converts older formats:

- **Blobs** with a version byte are read through `storedBlobLoad()`: the current version is taken; an **older** one goes to `storageMigrateBlob(format, from, …)`; a **newer** one (a downgrade) is not read and not overwritten until the user saves settings. Every loader reads a stored blob up to `STORED_BLOB_READ_MAX` (32 KB; history tiers `STORED_HISTORY_TIER_READ_MAX`, 256 KB), not only up to its current format's size — so an older version that was *larger* (a format that later shrank) still reaches the hook; a current blob is then checked against its exact / maximum size.
- **Downgrade, stores written on their own:** the reset history (`diag`), the chart history tiers and the records also keep a newer firmware's blob untouched — the older firmware keeps its own values in RAM only (logged at the start) until the user deletes that store: *Reset values → chart history* or *all records*; the reset history stays untouched until *Erase everything*. **Exception:** the rain / sunshine statistics (`rainstat`): a newer blob is not read and is replaced at the next save (their totals live in RTC RAM as well and are needed for the daily rain; week / month / year start again).
- **The `config` key set:** `loadConfigInto()` reads `cfgver` first; an older one goes to `storageMigrateConfig(from)` before any key is read (renamed / re-scaled keys).
- **Formats with a magic** (alarm rules, records, rain, history tiers) keep their own version check in their decoder; a new version adds its conversion there or calls the hook.

4.0.0 is the first frozen layout: both hooks are empty. **Migrations must be idempotent:** after a downgrade the older firmware's next settings save writes its own `cfgver` and key set, so on the re-upgrade the hook runs again over keys it had already converted — a rename must only move a key when the new key is absent, never copy a stale old key over the user's value. A change after the release: bump the version in `storage_layout.h`, add a `case` for the previous version to the hook (with a host test that converts a real blob of the old version), and note it in the changelog.

What is deliberately **not** migrated: nothing from 2.x / 3.x is converted in flash — a node comes to 4.x with a USB flash that erases `cfgdata`; settings move with the backup file.

## Crash report

What the node keeps about its restarts (`src/reset_history.h`):

- **Reset history** — the last 10 starts: cause (`esp_reset_reason()`: power-on, reset button, software restart with its reason, crash, task / interrupt watchdog, brownout, USB …), how long the node had run before, and the time (when the clock was set). Uptime and time come from a heartbeat in RTC RAM that the main loop refreshes once a second — after a watchdog reset it says when the loop last ran. One NVS write per start.
- **Last crash** — after a panic or a watchdog reset the ESP-IDF panic handler writes an ELF **core dump** to the `coredump` partition (all tasks' stacks and registers). At the next start the node reads its summary: task, program counter, backtrace (8 addresses), the panic details (an `assert` names file, line and function; `abort()`; the task watchdog names the hung tasks) or the CPU exception (`LoadProhibited`, …), the firmware version and its ELF SHA-256. A dump of another firmware (left over when a newer one could not be written) is not used.

Where to see it: status page (*Last restart*, *Restarts before*, *Last crash*), `/api/info` → `system.resets`, `system.lastCrash`, and Settings → System → *Diagnostics* → *Crash report*.

**Function names** need the symbols of the build, which the node does not carry (they would double the image). Decode the addresses with the `firmware.elf` of exactly that build (`elfSha` tells which). Keep the `firmware.elf` of every build you install (`.pio/build/heltec_wifi_lora_32_V4/firmware.elf` — the release files in `firmware/` are only the `.bin` images):

```
xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf 0x42013a2c 0x42013b5f 0x4200d7e1
```

**Core dump download** (admin, Settings → System → Diagnostics, or `GET /api/diag/coredump`): the raw image as stored. It holds the RAM at the crash — passwords too, so do not post it publicly. Decode:

```
esp-coredump info_corefile --core-format raw -c uwn-coredump-<host>-<elfSha>.bin firmware.elf
```

The dump stays until the next crash overwrites it.

**Test crash** (Settings → System → Diagnostics → *Test crash*; `POST /api/diag/crash`): admin login, same-origin request and the typed word `CRASH`. `panic` writes to address 0 in the main loop (a `StoreProhibited` exception with a backtrace), `watchdog` stops the main loop until the task watchdog resets the node after 120 s.

### Task watchdog coverage

Timeout 120 s with a panic (so a hang leaves a core dump), set in `setup()`.

| Task | Watched | How |
|------|---------|-----|
| `loopTask` (core 1: measuring, radio, LoRaWAN, display) | yes | fed once per `loop()`; unsubscribed only for the config portal, which ends in a restart |
| `IDLE0` (core 0) | yes | stock: catches a task on core 0 that never blocks |
| `webApi` (core 0: server start / stop, SSE pushes, captive DNS, deferred restart) | yes (B11) | fed every pass (20–50 ms) |
| `wxUpload` (core 0: weather networks, notifications, custom uploads, InfluxDB, update check, REST sensors) | yes (B11) | fed before every network step; each request is bounded by its timeouts (connect 5 s, TLS 8 s, read 8 s) |
| `httpd` (core 0: the page / API handlers) | while a handler runs (B11) | subscribed per request, fed with every received or sent chunk — a long upload or page is fine while data moves; idle between requests it is not watched (it waits in `select()`) |
| `davis` (core 1, Davis receiver) | no | blocks on radio interrupts by design; a stall shows as missed packets (status page), not a hang of the node |
| `IDLE1` (core 1) | no | stock Arduino setting: the loop task on core 1 seldom yields to it |
