# DavisSensorReceiver

Receiver and decoder for the wireless packets of **Davis Instruments Vantage Pro2 / Vantage Vue ISS**
and the **anemometer transmitter kit 6332** (EU 868 MHz versions) with a **Semtech SX1262** and
[RadioLib](https://github.com/jgromes/RadioLib) 7.x — written for
[universal-weather-node](../../README.md), kept self-contained like `FineOffsetSensorReceiver` and
`TfaWeatherSensorReceiver` next to it. MIT license, own code: the GPL / CC-BY-SA projects listed
under *Sources* were read for protocol facts only, nothing was copied.

> **Status: experimental.** Decoder checked against 32 published sample packets (host tests,
> `test/firmware_davis.cpp`); the hop tracker against a simulated transmitter (drift, gaps, frequency
> offset, other IDs). **Not yet tested with a real ISS.**

## Parts

| File | What | RadioLib |
|------|------|----------|
| `DavisDecoder.h/.cpp` | bit reversal, CRC, all message types, rain-counter wrap, intervals | no (host-testable) |
| `DavisHopper.h/.cpp` | hop-following state machine: ACQUIRE (with offset scan) → TRACK (PLL) → LOST, RAW capture, optional software AFC | no (host-testable) |
| `DavisRadio.h/.cpp` | SX1262 FSK set-up, retune, read with the interrupt time stamp | yes |

```cpp
DavisRadio::setRadio(&radio);            // the application's SX1262
DavisHopper hop;  DavisRadio dr;
hop.configure(DavisHopperConfig(), micros64());
dr.begin(hop.tuneMHz(), 93.8f);
for (;;) {
    uint8_t b[DAVIS_PACKET_LEN]; float rssi; int64_t t;
    if (dr.read(b, rssi, t)) { DavisRxResult r = hop.onPacket(b, t); /* r.pkt: values */ }
    while (hop.poll(micros64()).kind != DAVIS_EV_NONE) {}
    dr.tune(hop.tuneMHz());              // only when it changed
    // sleep until hop.nextDeadlineUs() or the next packet interrupt
}
```

## Protocol (as implemented)

| Item | Value | Source / confidence |
|------|-------|---------------------|
| Modulation | 2-GFSK, BT 0.5, **19.2 kbit/s** (measured 19.23) | DavisRFM69; davis-rx IQ measurement |
| Deviation | ±9.6–10 kHz (receiver set to 9.9 kHz) | VPTools SDR captures, davis-rx |
| Frame | ~18 bits carrier, preamble `AA AA AA AA`, sync **`CB 89`**, 10 bytes, short carrier trailer; ~7.8 ms | VPTools packet-samples |
| Bit order | **every byte LSB first** — an MSB-first receiver sees each byte bit-reversed | DavisRFM69 (`reverseBits`), davis-rx |
| Bytes | `[0]` type<<4 · battery-low<<3 · (ID−1); `[1]` wind mph; `[2]` wind direction; `[3..5]` value; `[6..7]` CRC; `[8..9]` `FF FF` | DavisRFM69 wiki "Message Protocol" |
| CRC | **CRC-16/CCITT** (poly 0x1021, init 0, no reflection) over bytes 0–5 = bytes 6–7 (big-endian); a repeater's CRC also covers bytes 8–9 | wiki; verified on 32 samples |
| Interval | **(40 + ID) / 16 s**: ID 1 = 2.5625 s … ID 8 = 3.0 s | DavisRFM69 `(41 + id)/16` with id = ID−1; davis-rx IQ |
| EU channels (hop order) | **868.0589, 868.2993, 868.5396, 868.1792, 868.4196 MHz** (measured, 2026) — alternative: 868.0667, 868.2971, 868.5275, 868.1819, 868.4123 (DavisRFM69 2014) | kobuki/VPTools `FRF_EU`; dekay/DavisRFM69 |
| Hop sequence | channel n → n+1 (mod 5), one packet per channel | DavisRFM69 `nextChannel()` |
| Wind speed | byte 1, mph, every packet | wiki |
| Wind direction | VP2: 9 + (raw−1)·342/253 ° (0 → 0°, 255 → 355°); Vue: raw·1.40625 + 0.3 ° | meteostick, wiki |
| 0x2 supercap / 0x7 solar cell (Vue) | 10-bit raw = (b3<<2)\|(b4>>6), **/300 V** | meteostick — scaling unverified (wiki: /100) |
| 0x4 UV | raw/50; `b3 = FF` = no sensor | wiki, meteostick |
| 0x5 rain rate | raw = ((b4&0x30)<<4)\|b3; 0x3FF = no rain; b4 bit 6 set: raw s/tip, clear: raw/16 s/tip | meteostick (with labelled examples) |
| 0x6 solar | raw × 1.757936 W/m²; `b3 = FF` = no sensor | wiki, meteostick |
| 0x8 temperature | 12-bit two's complement in 0.1 °F (b3, upper nibble of b4) if b4 bit 3 (digital sensor) | meteostick, wiki |
| 0x9 gust | b3 = highest mph of the last 10 min, b5>>4 = index | wiki |
| 0xA humidity | raw = ((b4>>4)<<8)\|b3; digital (b4 bit 3): raw/10 %; analog: raw·−0.301 + 710.23 | wiki, meteostick |
| 0xE rain | b3 & 0x7F = bucket-tip counter, wraps at 128; `0x80` = no collector | wiki, meteostick |
| 0x3, 0xC | unknown (0x3 follows the sun per davis-rx; 0xC from the anemometer kit) | — |
| Message order (VP2) | 8 E 5 4 / 8 E 5 9 / 8 E 5 A / 8 E 5 A / 8 E 5 6 → temperature and rain every 4 packets (~10 s), humidity ~25 s, UV / solar / gust ~51 s | wiki (kobuki) |

Not implemented: the wind-speed error-correction table of weewx-meteostick (only changes high speeds
at some angles by 1–4 mph), analog (thermistor) temperature, soil/leaf stations (type 0xF), repeaters
(packets via a repeater are recognised and ignored).

## Receiving with the SX1262

* No FSK frequency-error register: RadioLib's `getFrequencyError()` returns 0 outside LoRa (checked in
  RadioLib 7.7.1), so there is no hardware AFC. The demodulator only decodes within about **±4 kHz** of
  the transmitter (davis-rx), while ISS crystals were measured 25–33 kHz off and drifting with
  temperature (0.3–0.75 kHz/°C). Hence: a configurable base offset, an **offset scan** while searching
  (6-kHz steps; the transmitter passes the parked channel every 5 packets, so one dwell is 5 periods
  + 0.4 s) and an optional **shoulder-probing AFC** while tracking (every 4th slot ±6 kHz, step 1.5 kHz).
* Timing: the packet time is the DIO1 "RX done" interrupt (`esp_timer`), consistently; the next
  packet is expected one period after the last one on the next channel, accepted within −guard…+window,
  and a PLL refines the period from the error (±200 ppm limit).
* Sensitivity: the receiver bandwidth (default 93.8 kHz) does not change the ±4 kHz tolerance — that is
  set by the deviation — narrower filters only lose less to noise.

## Uncertainties (to verify with a real station)

* The EU channel table (two tables available; the measured one is the default).
* Transmitter frequency offset of the individual ISS and its drift.
* Supercap / solar-cell scaling (/300 vs /100), the meaning of types 0x3 / 0xC.
* Rain-rate light/heavy bit polarity (meteostick and the wiki disagree; meteostick's examples used).
* Vue-specific differences beyond the wind direction (Vue sends types 2, 3, 7 additionally).
* Anemometer transmitter 6332: same format; its own ID; it may send types 8 / A / C.

## Sources

* dekay/DavisRFM69 — <https://github.com/dekay/DavisRFM69> and its wiki (RF Protocol, Message Protocol), CC-BY-SA
* kobuki/VPTools — <https://github.com/kobuki/VPTools> (`DavisRFM69.h` measured EU table, `docs/packet-samples`), CC-BY-SA
* matthewwall/weewx-meteostick — <https://github.com/matthewwall/weewx-meteostick> (`bin/user/meteostick.py`: message decoding, sample packets), GPL-3.0
* gsa700/davis-rx — <https://github.com/gsa700/davis-rx> (SX1262 findings: bit order, CRC range, frequency drift, AFC), GPL-3.0
* bobanddang/esp32-davis-sx1262 — <https://github.com/bobanddang/esp32-davis-sx1262> (SX1262 modem parameters)
* madscientistlabs blog (original reverse engineering), wxforum.net
* rtl_433 has no Davis ISS decoder (checked 2026-11).
