#!/usr/bin/env python3
"""Rewrites an Ogg file's logical-bitstream serial number to a fixed constant and recomputes
each page's CRC, so ffmpeg/libvorbis's per-encode-random serial doesn't break byte-for-byte
determinism (PLAN.md §1 rule 5 / §11 golden_scan.sh). ffmpeg has no CLI flag for a fixed
serial, so this patches the container after encoding rather than weakening the determinism gate.
"""
import sys

FIXED_SERIAL = 1


def _crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if (r & 0x80000000) else (r << 1) & 0xFFFFFFFF
        table.append(r)
    return table


_CRC_TABLE = _crc_table()


def ogg_crc32(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((crc >> 24) & 0xFF) ^ b]
    return crc


def fix_serial(path: str) -> None:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    offset = 0
    n = len(data)
    while offset < n:
        if data[offset:offset + 4] != b"OggS":
            raise ValueError(f"{path}: expected 'OggS' at offset {offset}")
        page_segments = data[offset + 26]
        segment_table = data[offset + 27:offset + 27 + page_segments]
        payload_len = sum(segment_table)
        page_len = 27 + page_segments + payload_len

        data[offset + 14:offset + 18] = FIXED_SERIAL.to_bytes(4, "little")
        data[offset + 22:offset + 26] = b"\x00\x00\x00\x00"
        crc = ogg_crc32(bytes(data[offset:offset + page_len]))
        data[offset + 22:offset + 26] = crc.to_bytes(4, "little")

        offset += page_len

    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        fix_serial(arg)
