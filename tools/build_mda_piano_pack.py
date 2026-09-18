#!/usr/bin/env python3
"""Build the MDA Piano LittleFS sample pack from PicoFaceCP headers."""

import argparse
import re
import struct
import zlib
from pathlib import Path

MAGIC = b"MDAPACK\0"
VERSION = 1
HEADER = struct.Struct("<8sIIII")
ENTRY = struct.Struct("<8sIIIIIII")
KEYGROUP = struct.Struct("<iiiii")

INSTRUMENTS = (
    ("Rd I", "mdaEPianoData.h", "epianoData", None, None, 32000),
    ("Rd II", "Rd_IIData.h", "Rd_IIData", "Rd_IIKeygroups.h", "Rd_IIKgrp", 32000),
    ("Wr", "WrData.h", "WrData", "WrKeygroups.h", "WrKgrp", 32000),
    ("Clv", "ClvData.h", "ClvData", "ClvKeygroups.h", "ClvKgrp", 32000),
    ("Piano", "PnoData.h", "PnoData", "PnoKeygroups.h", "PnoKgrp", 32000),
    ("CP", "CPData.h", "CPData", "CPKeygroups.h", "CPKgrp", 32000),
)


def array_values(path: Path, symbol: str) -> list[int]:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\s*\]\s*=\s*\{{(.*?)\}};", text, re.S)
    if not match:
        raise ValueError(f"array {symbol} not found in {path}")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.S)
    return [int(value) for value in re.findall(r"-?\d+", body)]


def keygroups(path: Path, symbol: str) -> list[tuple[int, int, int, int, int]]:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\s*\d+\s*\]\s*=\s*\{{(.*?)\}};", text, re.S)
    if not match:
        raise ValueError(f"keygroups {symbol} not found in {path}")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.S)
    groups = []
    for row in re.findall(r"\{([^{}]+)\}", body):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        if len(values) != 5:
            raise ValueError(f"invalid keygroup in {path}: {row}")
        groups.append(tuple(values))
    return groups


def default_keygroups(source_path: Path) -> list[tuple[int, int, int, int, int]]:
    text = source_path.read_text(encoding="utf-8")
    groups = [[0, 0, 0, 0, 0] for _ in range(33)]
    fields = {"root": 0, "high": 1, "pos": 2, "end": 3, "loop": 4}
    for index, field, value in re.findall(r"kgrp\[\s*(\d+)\s*\]\.(root|high|pos|end|loop)\s*=\s*(-?\d+)", text):
        idx = int(index)
        if idx < len(groups):
            groups[idx][fields[field]] = int(value)
    if groups[-1][3] == 0:
        raise ValueError("Rd I keygroups not found in mdaEPiano.cpp")
    return [tuple(group) for group in groups]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="PicoFaceCP directory at commit 4c49010")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    include = args.source / "include"

    items = []
    for name, data_file, data_symbol, group_file, group_symbol, sample_rate in INSTRUMENTS:
        pcm = array_values(include / data_file, data_symbol)
        groups = (default_keygroups(args.source / "src" / "mdaEPiano.cpp") if group_file is None
                  else keygroups(include / group_file, group_symbol))
        pcm_bytes = struct.pack(f"<{len(pcm)}h", *pcm)
        group_bytes = b"".join(KEYGROUP.pack(*group) for group in groups)
        items.append((name, sample_rate, pcm, pcm_bytes, groups, group_bytes))

    metadata_size = HEADER.size + ENTRY.size * len(items)
    payload = bytearray()
    entries = bytearray()
    for name, sample_rate, pcm, pcm_bytes, groups, group_bytes in items:
        pcm_offset = metadata_size + len(payload)
        payload.extend(pcm_bytes)
        while (metadata_size + len(payload)) % 4:
            payload.append(0)
        group_offset = metadata_size + len(payload)
        payload.extend(group_bytes)
        entries.extend(ENTRY.pack(name.encode("ascii").ljust(8, b"\0"), sample_rate, 3,
                                  pcm_offset, len(pcm), group_offset, len(groups),
                                  zlib.crc32(pcm_bytes) & 0xFFFFFFFF))

    total_size = metadata_size + len(payload)
    header = HEADER.pack(MAGIC, VERSION, len(items), total_size, zlib.crc32(payload) & 0xFFFFFFFF)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + entries + payload)
    print(f"wrote {args.output}: {total_size} bytes, {len(items)} instruments")


if __name__ == "__main__":
    main()