# Virtual Sensors

*Settings → Outputs → Virtual sensors* (shown with *Show advanced options*). Firmware 4.x, Heltec V4.

The node can **send its own values as Ecowitt / Fine Offset or LaCrosse / TFA IT+ sensor frames** on 868.3 MHz. An Ecowitt gateway (GW1100, GW2000, …), a console, an IT+ weather station or another weather node then shows them as an ordinary wireless sensor. You pick the console, app or cloud you like — and it works with almost any sensor the node itself has.

## Use cases

- **Wired Davis anemometer and rain gauge** on a V4 Full board → a virtual **WH65 / WS69** (wind, gust, direction, rain, temperature, humidity, UV, light) at the Ecowitt gateway.
- **BME280** on the node → a virtual **WN32P** (temperature, humidity, pressure).
- A **Bresser or TFA sensor** received by the node → a virtual **WH31** on a free channel, so a console that only knows Ecowitt shows it.
- **Dead spot:** the node sits where it receives a sensor the gateway cannot hear, and sends its values on under the node's **own** ID.
- A **DS18B20 probe** on the board → a virtual WH31.
- Any temperature (and humidity) → a virtual **IT+ TX29 / TX35** for a LaCrosse / TFA IT+ station.
- A test sensor for your own receiver.

## Types and fields

| Type | Fields | Needed | Sent every |
|------|--------|--------|-----------|
| WH31 (channel 1–8) | temperature, humidity | temperature | 61 s |
| WH40 | rain (running total) | rain | 49 s |
| WN32P | temperature, humidity, station pressure | temperature | 64 s |
| WH65 / WS69 | temperature, humidity, wind, gust, wind direction, rain (running total), UV index, illuminance | at least one value; wind needs its direction | 16 s |
| IT+ TX29 (17.241 kbit/s) | temperature, humidity | temperature | 15 s |
| IT+ TX35 (9.579 kbit/s) | temperature, humidity | temperature | 22 s |

The Fine Offset intervals are those of the original sensors, with ±0.25 s jitter. The IT+ originals send every ~4 s; at 4 s a single TX35-style slot would take 12.5 s of the 18 s the node may plan per hour, so the IT+ intervals are the shortest at which **8 slots of the type** still fit (see [Radio and airtime](#radio-and-airtime)). Whether every IT+ station accepts a sensor heard only every 15 / 22 s is **not tested yet**. Up to **8 virtual sensors** — e.g. one WH31 on each of the 8 channels a gateway knows.

**Each field gets a value name** — any value of the station, as in the placeholder list under the fields (click one to insert it):

- main values: `temp`, `hum`, `press` (station pressure), `qnh`, `wind`, `gust`, `dir`, `uv`, `lux`, `solar` …
- derived values: `dew`, `feels`, `rain_day`, `rain_1h` …
- a ★ sensor's value: `<custom ID>_temp`, `_hum`, `_wind`, `_gust`, `_dir`, `_rain`, `_uv`, `_lux` (e.g. `garten_temp`; stored by the sensor's internal ID, so it survives a battery change); a sensor that is only received: `rf<radio ID>_temp`
- only here: `ds1`–`ds3` (the board's DS18B20 probes) and `rain_total` (the station's running rain total from the selected rain source, since the start)

Values are metric. Rain fields expect a **running total** (`rain_total` or a sensor's `_rain`), not a daily sum — the gateway computes its rain from the increase. The total wraps like the originals' counters (WH40 at 6553.6 mm, WH65 at 16 646 mm).

Empty field = not sent (the frame carries the sensor's "no value" code). A field whose value is not available right now (sensor silent, no source) is left out; if a **needed** value is missing (WH31 / WN32P temperature, a mapped rain total), the frame is **skipped** — a rain total must never drop to 0, that would look like a reset.

**The sensor ID is not configurable.** It is derived from the node's MAC and the slot and shown read-only (e.g. `3000023C` = WH31, channel 2, ID 3C — exactly what another node's *Wireless sensors* list shows). Different slots always have different IDs. Changing the type changes the ID; a settings backup restored on another node gets that node's IDs.

## IT+ (TX29 / TX35 style)

The frame of a LaCrosse / TFA IT+ thermo(-hygro) sensor: 5 bytes — model nibble 9, a 6-bit ID, the new-battery flag, the temperature as 3 BCD digits (°C + 40, in tenths), the humidity (1–99 %) or the "no humidity" code `0x6A`, CRC-8 (polynomial 0x31). The temperature is clamped to the originals' range, −40.0 … +59.9 °C. The weak-battery flag is never set.

- **TX29 style** = 17.241 kbit/s (TX29-IT, TFA 30.3159.IT, …), **TX35 style** = 9.579 kbit/s (TX35-IT, TFA 30.3155.WD, …). Pick the one your station listens to.
- **ID:** only 6 bits (0–63), derived like the other types; shown as `170000xx` / `090000xx`, `+100` with a humidity value mapped (as a receiving node lists it). With only 64 IDs, a real sensor nearby may have the same one: the status page warns when this node **receives** an IT+ sensor of the same rate with that ID (either humidity variant) — for that, receive *IT+* at that rate. A station tells IT+ sensors apart only by the ID; use another slot (= another ID) then.
- **Pairing:** for **5 minutes** after a slot is switched on or its type changed (and after every restart of the node) the frames carry the **new-battery flag**, like a real sensor after a battery change. Start the station's sensor search in that time (power-on or the station's search button, see its manual).

## The red line

Virtual sensors send **readings, never commands**:

- Only frames built from the node's own values by the typed sensor encoders (WH31, WH40, WN32P, WH65, IT+). There is **no way to send arbitrary bytes** — no setting, API, MQTT command, downlink or debug hook takes a frame.
- **No actuator, gateway or command frames** exist in the firmware (valves, sockets, …). They are not hidden or switched off: they were never written.
- **Own ID only.** The ID comes from the node; the firmware never sends under a foreign sensor's ID and never re-sends a received frame (no repeater, no capture-replay).
- **Off by default**, every slot separately.

This is enforced by the code's structure and checked by `test/firmware_vsensor.cpp`.

## Radio and airtime

| | |
|---|---|
| Frequency | 868.3 MHz |
| Modulation | FSK, 17.241 kbit/s (IT+ TX35 style: 9.579 kbit/s), ±50 kHz deviation, NRZ, no whitening |
| Frame | 48-bit preamble, sync word `2D D4`, the sensor frame with its own CRC-8 / checksum |
| Power | *Transmit power* 0–14 dBm at the antenna (default 10); the V4's front-end gain is taken into account |
| Airtime | 9 ms (WH31, IT+ TX29) to 14 ms (WH65) per frame; IT+ TX35 13 ms |

The ±50 kHz deviation and the 48-bit preamble are assumptions — not measured on an original Ecowitt or IT+ sensor. Fine Offset frames (WH31, WN32P) were received by a V3 node over the air; an Ecowitt gateway or console has not been tested yet, and IT+ frames are so far only checked against the node's own decoder (host tests).

**Duty cycle:** 868.0–868.6 MHz allows **1 % airtime** (36 s per hour). The node counts its airtime of the last hour in that band — virtual sensors **and LoRaWAN uplinks** — and skips a virtual-sensor frame that would go beyond it. A configuration that would plan more than half the budget (18 s per hour) is refused when saving: 8 × WH31 plan 4.3 s per hour, one WH65 3.2 s — so at most 5 WH65 (16 s). IT+: 8 × TX29 (every 15 s) plan 17.6 s, 8 × TX35 (every 22 s) 17.3 s.

**Sharing the radio:** the SX1262 also receives the wireless sensors and sends LoRaWAN. A frame is sent between the receive phases and never during a LoRaWAN uplink or its receive windows; when a received sensor's packet is due, the frame waits up to 3 s. Reception pauses for about 30 ms per frame. During a firmware upload no frame is sent (*radio busy*). In Davis mode the frames go in the gaps between the Davis packets — see [Together with Davis reception](#together-with-davis-reception). Low-power mode is off while virtual sensors are on.

### Together with Davis reception

In [Davis mode](wireless-sensors.md#davis-wireless-experimental) the Davis receiver owns the radio and follows the ISS's hop sequence: one packet every 2.5625 s (ID 1) … 3.0 s (ID 8). There is no general radio scheduler; instead the virtual sensors use the gap right after a Davis packet:

- Each virtual sensor keeps its own interval. When a frame is due, it waits for the next gap (one waiting frame per sensor).
- Right after a Davis packet that arrived where the hop prediction expected it, the Davis receiver sends **up to 2** waiting frames (oldest first, so 8 sensors are spread over a few gaps) and is back on the **next** hop channel long before the next packet: each frame is counted with its airtime + 60 ms for switching to transmit and back, and the radio must be listening again 300 ms before the earliest moment the next packet can start (prediction − *guard* − packet length). Before every frame this is checked again with the real clock, so a slower radio only ends the gap early.
- **Davis reception has priority:** after a missed packet nothing is sent in that gap, and nothing at all while the receiver searches (ACQUIRE / LOST), has just found the transmitter (the first packet only starts the prediction) or is in raw capture. The hop prediction runs on unchanged.
- A frame still waiting when its sensor's next frame is due is replaced by the newer one and counted as **deferred (Davis)**. With a good Davis signal this stays at 0: two frames per gap carry more than 8 virtual sensors at their shortest interval (8 × IT+ TX29 every 15 s).
- The airtime budget is the same (1 %; waiting frames are already counted in it), and LoRaWAN stays paused in Davis mode.

Not yet tested with a real Davis transmitter: the timing figures (60 ms switching, 300 ms margin) are worked out from the radio set-up, not measured on the chip. The status page shows the longest gap use that was really measured (*Davis gaps*).

**Legal note:** 868.3 MHz with 1 % duty cycle and up to 25 mW ERP is a licence-free SRD band in the EU. **You are responsible for following the radio rules where you use the node** — outside the EU, 868 MHz may not be allowed. Do not use the feature to deceive anybody.

## Status

The status page shows them in their own box *Virtual sensors* right after the wireless sensors: per virtual sensor the frames sent, when the last one went out and how many were skipped (airtime limit / radio busy / value missing) or, in Davis mode, deferred for Davis reception, and the node's 868 MHz airtime of the last hour. In Davis mode a row *Davis gaps* adds the frames sent in gaps and the longest gap use measured (first frame's transmit set-up until Davis reception runs again; expected well below 200 ms). The same in [`/api/info`](api.md) → `virtualSensors` (`skipped.deferredDavis` per slot, `davisGapFrames`, `davisGapMaxMs`).

**Checking it with a Davis station:** Davis mode on, virtual sensors on, wait for TRACK in the *Davis* box. Then: the virtual sensors' *sent* counts rise at their intervals, *deferred (Davis)* stays near 0, the Davis box's sync % and misses are the same as with virtual sensors off (compare a few minutes each), *Davis gaps* stays far below the Davis period (≤ ~200 ms), and a second node or the gateway receives the virtual sensors. A miss right after a gap with a frame would show in the radio trace (`/api/davis/log`) as `MISS` following an `ok` packet — send us that trace.

## Checking it

Another weather node set to *Fine Offset* (or *Auto*; for IT+: *IT+ 17.241 kbit/s* or *IT+ 9.579 kbit/s*) receives the virtual sensor like a real one: it appears in its *Wireless sensors* list with the ID shown in the settings. An Ecowitt gateway is expected to add it in its app as a new sensor of that type (WH31: on the configured channel), and an IT+ station to learn it during its sensor search (see *Pairing* above) — neither is tested yet; reports are welcome.
