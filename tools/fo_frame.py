#!/usr/bin/env python3
"""Check and decode one Fine Offset (Ecowitt / Froggit) 868 MHz frame by hand.

Usage:  python3 tools/fo_frame.py 20002AA492159D047FB5
        (the hex bytes after "|" in a radio-trace line; spaces allowed)

Prints every step: CRC-8 (poly 0x31, init 0) over the bytes before the CRC
byte, SUM-8 (sum of all bytes before the sum byte, modulo 256), and the
fields of the WN20 and WH40 rain gauges. Other families: only the checks.
The layout is the one documented in docs/fineoffset-frames.md.
"""
import sys

LENGTHS = {0x20: 10, 0x40: 9}   # frame length incl. CRC and SUM


def crc8(data, poly=0x31, init=0x00):
    c = init
    for byte in data:
        c ^= byte
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def main():
    raw = bytes.fromhex("".join(sys.argv[1:]).replace(" ", ""))
    if not raw:
        sys.exit(__doc__)
    fam = raw[0]
    n = LENGTHS.get(fam)
    if n is None:
        sys.exit(f"family 0x{fam:02X}: not handled here (WN20 0x20, WH40 0x40)")
    b = raw[:n]
    print("bytes   " + " ".join(f"{i:>2}" for i in range(n)))
    print("        " + " ".join(f"{x:02X}" for x in b))
    crc = crc8(b[:n - 2])
    s = sum(b[:n - 1]) & 0xFF
    print(f"CRC-8 over bytes 0..{n - 3}: 0x{crc:02X}, frame byte {n - 2}: 0x{b[n - 2]:02X} -> {'ok' if crc == b[n - 2] else 'FAIL'}")
    print(f"SUM-8 over bytes 0..{n - 2}: 0x{s:02X}, frame byte {n - 1}: 0x{b[n - 1]:02X} -> {'ok' if s == b[n - 1] else 'FAIL'}")
    rain = b[5] << 8 | b[6]
    if fam == 0x20:
        print(f"WN20  ID      bytes 2-3   0x{b[2]:02X}{b[3]:02X} -> sensor ID {b[2] << 8 | b[3]:04X} (shown as 2000{b[2]:02X}{b[3]:02X})")
        print(f"      battery byte 4      0x{b[4]:02X} = {b[4]} x 20 mV = {b[4] * 20} mV")
        print(f"      rain    bytes 5-6   0x{b[5]:02X}{b[6]:02X} = {rain} x 0.1 mm = {rain / 10:.1f} mm (running total)")
        print(f"      unknown byte 7      0x{b[7]:02X}")
    else:
        print(f"WH40  ID      bytes 1-3   0x{b[1] & 0x0F:X}{b[2]:02X}{b[3]:02X} (shown as 40{b[1] & 0x0F:02X}{b[2]:02X}{b[3]:02X})")
        print(f"      battery byte 4      0x{b[4]:02X} & 0x1F = {b[4] & 0x1F} x 0.1 V = {(b[4] & 0x1F) / 10:.1f} V")
        print(f"      rain    bytes 5-6   0x{b[5]:02X}{b[6]:02X} = {rain} x 0.1 mm = {rain / 10:.1f} mm (running total)")


if __name__ == "__main__":
    main()
