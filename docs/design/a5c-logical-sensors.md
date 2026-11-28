# A5c · Logical sensors — design (draft for review)

Status: draft 2026-11-26, to be agreed with the maintainer before building.

## Problem

A wireless sensor is referenced by its **radio ID** everywhere:

| Where | Today |
|---|---|
| ★ list, order | `Config::rfIds` ("Mine", order = wireless 1…n) |
| Names | `Config::rfNames` (`ID:name\|…`) |
| Minimum signal | `Config::rfMinRssi` (`ID:dBm\|…`, 8aaebc0) |
| Value sources | `Config::srcId[6]` |
| Alarms | `AlarmRule::rfId` |
| Placeholders (OLED screens, custom uploads, alarm texts) | `{rf17000008_temp}` or `{rfN_temp}` (slot) |
| MQTT | `<prefix>/rf/<ID>` |
| Home Assistant | `unique_id` `<client>_rf_<ID>_<entity>`, device `<client>_rf_<ID>` |
| Loxone | `rf<slot>_temp` (slot number, shifts when the ★ order changes) |
| InfluxDB / `/metrics` | tag / label `id=<ID>` |
| `/api/data` | `rfSensors[].id` |

The radio ID changes with every battery change (IT+, most Ecowitt / Froggit) and, for IT+, may even be shared by two sensors. After a battery change every one of these places has to be redone, and Home Assistant gets new entities (history breaks).

## Three layers (agreed 2026-11-26)

| | Example | Changeable | Used for |
|---|---|---|---|
| **Internal ID** | see D1 | never | everything stored inside the node; HA `unique_id` |
| **Custom ID** (*Kennung*) | `17000008` → `garten` | yes, deliberately | everything towards other systems and what the user types: placeholders, MQTT topic, REST API, InfluxDB tag, `/metrics` label, Loxone |
| **Display name** (*Bezeichnung*) | `Garten Süd` | yes, any time | display only: web UI, OLED, HA friendly name, InfluxDB / metrics extra `name` |

The **radio ID** becomes an attribute of the logical sensor, like the minimum signal. A battery change re-binds the radio ID; custom ID, name and every reference stay.

Only **★ sensors** are logical sensors. Received-only sensors (neighbours) stay rows with their radio ID until taken over (★).

## Decisions

**D1 — format of the internal ID** (decided 2026-11-26). **Random, and shaped unlike any custom ID:** `L` + 6 hex digits (e.g. `L3F9A2C`), drawn at take-over, checked unique, never changed or reused. A custom ID may not have this shape. Why: if internal ID and custom ID looked alike, code using the wrong one would go unnoticed until someone changes a custom ID; with different shapes a mix-up shows at once, and the host tests always use custom IDs that differ from the internal IDs. HA `unique_id` / device id use the internal ID.

**D2 — custom ID.** Proposed on take-over: the radio ID (`17000008`). Rules: `a-z A-Z 0-9 _ -`, 1–16 characters, unique (case-insensitive), not the name of a built-in placeholder (`temp`, `hum`, `wind`, …). Changing it: the form lists what changes outside (MQTT topic, InfluxDB series, Loxone inputs); HA keeps the entity (D1), the node re-publishes discovery with the new topic and clears the old retained topic.

**D3 — Loxone** (decided 2026-11-26): `<custom ID>_temp`, `<custom ID>_humidity` … like every other output — the most consistent. Today's `rf<slot>_…` (position in the ★ list) goes away; the update renames the Loxone inputs once (`rf1_temp` → `<custom ID>_temp`) — an update note in the changelog and the migration guide. Received-only sensors (no custom ID) keep being sent, as `rf<radio ID>` (`rf17000023_temp`) — see *Naming*.

**D4 — placeholders.** Stored with the internal ID and a marker, e.g. `{@17000008_temp}`; the forms show and accept `{<custom ID>_temp}` (custom ID → internal on save, back on display). Changing a custom ID therefore updates every screen, upload and alarm text. Old forms keep working: `{rf17000008_temp}` (radio ID) and `{rfN_temp}` (slot) are resolved as today and converted to the stored form on the next save of that form. *Available placeholders* lists `{<custom ID>_…}` with the name as a tooltip.

**D5 — battery change.** The sensor table offers *"new sensor 17000023 replaces Garten?"* when a new sensor of the same protocol and kind (IT+ with / without humidity; Fine Offset model; Bresser type) appears while a ★ sensor has been silent for its freshness window. One click: radio ID re-bound, the new ID removed from *received*. IT+ frames with the *new battery* flag may do it automatically (the take-over logic in `storeItPlus()` exists). Also a manual *re-bind* field per logical sensor.

**D6 — storage.** One NVS blob `lsens`, fixed array of 32:

```
struct LogicalSensor {           // ~70 bytes -> about 2.3 KB for 32
    char     internalId[8];             // internal ID "L3F9A2C", "" = free
    char     customId[17];
    char     displayName[25];    // UTF-8, as rf_names today
    uint32_t radioId;            // current binding, 0 = none yet
    int8_t   minRssiDbm;            // 0 = none
};
```

The ★ order is the array order (replaces `rfIds`). `rfNames` and `rfMinRssi` stay for received-only sensors (by radio ID); entries of ★ sensors move into `lsens`. `srcId[]` and `AlarmRule::rfId` hold an index-free reference: the internal ID (string, 12 B) instead of a `uint32_t` — or a 16-bit handle into the table (smaller, but must be stable when sensors are removed → keep slots, never compact). **Recommended: the string**, simpler to migrate and to read in a backup.

*Built (step 1, 2026-11-26):* the table is a global `gLogicalSensors` next to `Config`, not inside it — `Config` (~7 KB) is copied whole (settings POST `next`, portal, `loadConfig()`), and none of those copies need the ★ list; `saveConfig()` writes the blob too (only when it changed), so it is still "saved with the settings". Blob: layout byte 1, count, 55-byte records (≤ 1762 B). References: `Config::sourceInternalId[6][8]`, `AlarmRule::sensorInternalId[8]` (alarm blob "ALR2"; an "ALR1" blob keeps its rules without the sensor), `SeriesDef::sensorInternalId` (stored `name|L3F9A2C|custom ID`, board `name|B|custom ID`). Backup v2: `sensors` array.

**D7 — outputs.**
- MQTT: `<prefix>/rf/<custom ID>`; payload gets `customId`, `internalId`, `displayName`, `radioId`.
- HA: `unique_id` / device id with the internal ID (D1); friendly name = name; state topic = the custom ID topic.
- InfluxDB: tags `sensor=<custom ID>`, `name=<name>` (a renamed sensor starts a new series under `name` — group by `sensor`; documented), field / tag `radio_id`.
- `/metrics`: labels `sensor`, `name`, `radio_id`.
- `/api/data` `rfSensors[]`: `id` stays the radio ID (compatibility); new `customId`, `internalId`, `displayName` for ★ sensors.
- OLED, dashboard, status page: the name (custom ID as a small second line where there is room).

*Built (step 2b, 2026-11-26):* as above; received-only sensors first appeared under the bare radio ID, now as `rf<radio ID>` (MQTT topic, Loxone, InfluxDB `sensor`, `/metrics` `sensor`; see *Naming*). The MQTT per-sensor payload is the `rfSensors[]` item plus `radioId`. HA: the announced list (`harfids`) holds internal IDs; per connection the node remembers custom ID, name and radio ID it announced per sensor — any change re-announces all entities (same `unique_id`) and sends an empty retained payload to the old state topic; radio IDs left in `harfids` by an earlier build get their old configs cleared once. Alarm text `{sensor}` = name, else custom ID; `/api/alarms` `customId` next to `sensorId` (radio ID). Concurrency: `gLogicalSensors` is written only by the web server task, under a recursive FreeRTOS mutex held for the write alone; readers on the other tasks copy the fields they need under it (`lsCopyByRadioId`, `lsCopyByInternalId`, `lsMineIds`, `lsSnapshot`, `rfLabelOf`, `rfNameOf`) — see lsensors.h.

**D8 — no migration** (decided 2026-11-26): until 4.x is released, old data on the devices is not carried over — keeps it simple. After the update the logical sensor table starts empty: ★ list, names, minimum signals and value sources by radio ID are not converted, and a 2.x settings backup does not bring them (everything else in it does). Home Assistant gets new entities once. The migration guide says so. Once 4.x versions are published, later changes to this layout need a migration again.

**Not solved by this:** two sensors with one radio ID (still the IT+ humidity signature, cddbf07, and the minimum signal, 8aaebc0).

## Tests

- Host: custom ID rules; placeholder conversion both ways incl. old forms; re-bind keeps every reference; HA config generation (unique_id stable across custom ID change and re-bind).
- On the V4: set up from an empty table; rename and change the custom ID with an OLED screen, a custom upload, InfluxDB and HA; battery change on the IT+ sensor (re-bind offer).

## Naming

Decided 2026-11-26 (mechanical rename, nothing released yet): one term per concept in code, JSON / API, the short external tags and the UI.

| concept | code | JSON / API | external tags (InfluxDB, `/metrics`) | UI de | UI en |
|---|---|---|---|---|---|
| radio ID | `radioId` | `id` (`rfSensors[]`), `radioId` | `radio_id` | Funk-ID | radio ID |
| internal ID | `internalId` | `internalId` | never sent | Interne ID | internal ID |
| custom ID | `customId` | `customId` | `sensor` (= the custom ID) | Kennung | custom ID |
| display name | `displayName` | `displayName` | `name` | Bezeichnung | name |
| minimum signal | `minRssiDbm` | - | - | Mindestsignal (column *Min. dBm*) | minimum signal |
| re-bind | `lsRebindRadioId()` | - | - | Neu zuordnen | re-bind |
| twin hint | - | `idTwin` | - | Funk-ID doppelt | radio ID shared |

Code: functions keep the `ls` prefix (= logical sensor) with spelled-out suffixes (`lsFindByInternalId`, `lsFindByCustomId`, `lsFindByRadioId`, `lsCopyByInternalId`, `lsCopyByRadioId`, `lsCustomIdProblem`, `lsSetCustomId`, `lsRebindRadioId`, `lsNewInternalId`, `lsIsInternalId`, `lsPlaceholdersToStored`, `lsPlaceholdersToCustomId`); table `LogicalSensorTable`, global `gLogicalSensors`; references `Config::sourceInternalId`, `AlarmRule::sensorInternalId`, `SeriesDef::sensorInternalId`. Settings form fields `rfname_<radio ID>`, `rfminrssi_<radio ID>`, `rfcustomid_<internal ID>`, `rfrebind_<internal ID>`. NVS keys stay short (`srciid0..5`, 15-character limit). fr / es / pt: *ID radio, ID interne, ID personnalisé, désignation, réassocier*; *ID de radio, ID interno, ID personalizado, denominación, reasignar*; *ID de rádio, ID interno, ID personalizado, designação, reassociar*.

**Received-only sensors towards other systems: `rf<radio ID>`.** A sensor that is not ★ has no custom ID and appears as `rf` + its 8-digit radio ID: MQTT topic `<prefix>/rf/rf17000023`, Loxone `rf17000023_temp`, InfluxDB tag / `/metrics` label `sensor="rf17000023"`. The bare radio ID would collide with a ★ sensor whose custom ID is `17000023` (the proposed default for a take-over of another sensor). `lsCustomIdProblem()` refuses `rf` + hex digits as a custom ID, so the two forms can never be equal; `rf<radio ID>` is also the legacy placeholder form `{rf17000023_temp}`. Host tests cover the collision case (Loxone, InfluxDB, `/metrics`, MQTT).
