# TinySoundFont component

Upstream: https://github.com/schellingb/TinySoundFont
Pinned commit: `853a0a171759f1ddba0de1442133a75912bbeffa`
License: MIT (`LICENSE`)

`include/tsf.h` carries an ESP32-P4-specific performance patch: render-loop,
pitch-position, and low-pass state use `float` rather than `double`. ESP32-P4
has hardware single-precision floating point; the original double operations
were software-emulated and cost roughly 19% of one 128-frame/44.1-kHz audio
budget per active SF2 voice in hardware measurements. With the float patch,
the measured incremental cost is about 1.3% per voice.

Sample positions are stored relative to each SF2 region's integer base offset.
This is required for pitch accuracy: a float absolute position several million
samples into a bank loses enough fractional precision to quantize the playback
increment (observed A4 near 422 Hz instead of 440 Hz). The local position keeps
sub-sample interpolation precise while retaining hardware-FPU performance.

The implementation is compiled with `-O3` and
`TSF_RENDER_EFFECTSAMPLEBLOCK=128`. Large allocations prefer PSRAM via the
wrappers in `tiny_soundfont.cpp`.

Measured on Waveshare ESP32-P4-NANO, Creative Labs 4 MB GM SF2:

| Active TSF voices | Before | After |
|---:|---:|---:|
| 4 | 79.5% | 9.0% |
| 8 | 153.8% | 14.1% |
| 16 | — | 24.5% |
| 24 | — | 34.7% |
| 32 | — | 44.7% |

No render deadline overruns were observed through 32 voices after the patch.
