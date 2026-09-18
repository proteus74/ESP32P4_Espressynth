# MDA Piano component

This component adapts the `mdaEPiano` engine and Reface CP effect chain from
[PicoVintageSynthCollection](https://github.com/Michi71/PicoVintageSynthCollection),
commit `4c49010f5b0215902679ca9bef35a446c2f6b847`.

The imported and adapted code is licensed under GPL-3.0-or-later. See
`LICENSE` and the SPDX/copyright headers in the source files.

## Sample pack

The six instruments (Rd I, Rd II, Wr, Clv, Piano, CP) are stored in
`/littlefs/samples/mda-piano.pack`. The firmware validates the format,
version, bounds, and CRC32 before exposing the immutable PCM/keygroup data
from PSRAM.

To regenerate the pack from the pinned upstream checkout:

```powershell
python tools/build_mda_piano_pack.py path\to\PicoFaceCP littlefs_data\samples\mda-piano.pack
```

The upstream repository does not clearly document the original license or
provenance of every generated sample set. Their inclusion was explicitly
accepted for this project; downstream distributors should verify that the
sample assets meet their distribution requirements.