# Fine Offset frames, decoded by hand (WN20 and WH40 rain gauges)

How the node gets a rain total out of an Ecowitt / Froggit radio packet, step by step, so that anyone can check it, for example against a console. The frames are real: they come from a tester's [radio trace](wireless-sensors.md) (rc.8, Heltec V4, 15 minutes on 21 Nov 2026). [`tools/fo_frame.py`](../tools/fo_frame.py) repeats every step for any frame you paste:

```
python3 tools/fo_frame.py 20002AA492159D047FB5
```

## 1. A line of the radio trace

```
3509.329 19:56:05 FSK17 -82 dBm OK Fine Offset WN20 ID 20002AA4 (CRC ok, sum ok) R 553.3mm bat 2920mV bat ok | 10B 20002AA492159D047FB5
```

Everything before `|` is what the firmware made of the packet. After it come the length the firmware used (`10B`) and the raw bytes in hex, exactly as the radio delivered them after the sync word. To check the decoding you only need the hex part.

## 2. Is the packet intact? CRC-8 and SUM-8

Every Fine Offset sensor frame ends in two check bytes:

- **CRC-8** (polynomial 0x31, start value 0) over all bytes before it
- **SUM-8**, the sum of all bytes before it, modulo 256

The first byte is the sensor family (`0x20` WN20, `0x40` WH40), and it fixes the frame length: WN20 10 bytes, WH40 9 bytes.

| Byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|------|---|---|---|---|---|---|---|---|---|---|
| WN20 | `20` | `00` | `2A` | `A4` | `92` | `15` | `9D` | `04` | `7F` | `B5` |
| meaning | family | – | ID | ID | battery | rain | rain | unknown | CRC | SUM |

- CRC-8 over bytes 0–7 = `0x7F` = byte 8 ✓
- `20+00+2A+A4+92+15+9D+04+7F` = `0x2B5`, modulo 256 = `0xB5` = byte 9 ✓

A frame that fails either check is dropped (the trace shows `FAILED`).

## 3. The fields

| Field | Bytes | Value | Result |
|-------|-------|-------|--------|
| ID | 2–3 | `2AA4` | sensor `20002AA4` (family + ID, as the Ecowitt app shows it) |
| Battery | 4 | `0x92` = 146 | 146 × 20 mV = **2920 mV** (2 × AAA) |
| Rain | 5–6 | `0x159D` = 5533 | 5533 × 0.1 mm = **553.3 mm** |
| ? | 7 | `0x04` | not known yet |

The WH40 has the same rain field:

```
40018EB8911C9AA674   ->  40 | 01 8E B8 | 91 | 1C 9A | A6 | 74
                         family | ID (20 bit) | battery (& 0x1F) × 0.1 V = 1.7 V | 0x1C9A = 7322 → 732.2 mm | CRC | SUM
```

## 4. Why a running total, and why 0.1 mm

- **Running total:** the tester's WN20 sent 18 valid frames in the 15 minutes (every 48 s); every one in the file reads `159D` = 553.3 mm. It was not raining. A "tips since the last packet" field would read 0 in dry weather; this one keeps its value, so it is a running counter. The WH40 next to it behaves the same way (732.2 mm in every frame). As 16-bit values they wrap after 6553.5 mm.
- **0.1 mm:** the rtl_433 project documents the field as "rain bucket tip count, 0.1mm increments" for both WH40 and WN20 (protocol 113, `ambientweather_wh31e.c`) and decodes it as `raw × 0.1` mm, "Total Rain". Our decoder does the same.

**What the packets do not prove:** how the sensor counts each tip into that total. The same rtl_433 file says "0.3 mm per tip" in its WN20 header. Our note that the WN20 adds 0.3 mm per tip and 0.4 mm on every sixth tip (1.9 mm per 6 tips) comes from the Ecowitt wiki (section WN20, read 21 Nov 2026). It cannot be seen in a dry-weather trace.

## 5. Checking it yourself

1. Start a radio trace (log page, *Radio trace*, filter *only this ID* with the WN20's ID, 15 min).
2. Tip the WN20's bucket slowly by hand, say 12 times, at least one transmission (48 s) apart, or let it rain.
3. Read off bytes 5–6 of each frame with `tools/fo_frame.py`.

If the counter rises by `3, 3, 3, 3, 3, 4` (0.1 mm steps) per tip, the 0.3 / 0.4 mm pattern is confirmed. If it rises by 1 per tip, the field counts tips and the factor is different. If it rises by 3 every time, the sensor counts a plain 0.3 mm per tip. With the console next to it you can also compare the console's WN20 total (at gain 1.00) with the counter in the frame.
