# Audio latency: the sweet spot (render block 128, DMA 3 x 128)

Goal: the shortest delay from a MIDI note to sound, with no DMA underruns while playing and as
little RAM as possible. Result: **`AUDIO_SYNTH_BLOCK_SIZE = 128`, `AUDIO_I2S_DMA_DESC_NUM = 3`,
`AUDIO_I2S_DMA_FRAME_NUM = 128`** (now the Kconfig defaults).

| | before | now |
|---|---|---|
| Render block | 512 samples (11.6 ms) | 128 samples (2.9 ms) |
| I2S DMA ring | 6 x 240 = 1440 frames (32.7 ms) | 3 x 128 = 384 frames (8.7 ms) |
| Note to sound (calculated, avg) | ~38 ms | ~11 ms |
| Underruns while playing (measured) | 0 | 0, also with web-download stress |
| Free internal RAM (measured) | 90 KB | 171 KB |

## Where the latency comes from

The render task (`SynthEngine.h`) loops: drain the MIDI queue, render one block, convert to 16-bit,
`i2s_channel_write(..., portMAX_DELAY)`. The write blocks until the DMA ring has room, which paces
the whole loop at one block per `block / 44100` seconds. A note-on is applied at the **start** of the
next block. For one MIDI event (USB-MIDI, 44.1 kHz):

| Stage | Formula | Before | Now |
|---|---|---|---|
| MIDI transport (USB, 1 ms polling) | | 1-2 ms | 1-2 ms |
| Wait for the next block start | 0 .. block time, avg half | 0-11.6 (avg 5.8) | 0-2.9 (avg 1.5) |
| DMA ring ahead of the new block | (ring - up to one descriptor) / 44100 | 27-33 ms | 5.8-8.7 ms |
| Codec / amplifier filter | | ~0.5 ms | ~0.5 ms |
| Render time itself | hidden in the ring while it stays under one block time | 0 | 0 |

The ring is the largest part (about 75 % before). Render time only adds latency when it exceeds the
block time; then the ring runs dry (underrun). BLE-MIDI adds its connection interval (typically
7.5-30 ms), and a preset's envelope attack comes on top. The latency numbers are **calculated** from
the code, not measured acoustically.

## Why the ring must not be smaller than ~3 x 128

A render block that takes longer than its time budget is absorbed by the ring. The measured worst
cases (block 128, budget 2.9 ms):

- Playing 5-note chords: peak render time 2.2 ms (75 % of budget), p99 1.7 ms.
- Same plus two parallel web downloads: peak 3.7-4.3 ms, 4 to 6 `Audio-Overrun` (budget misses) per
  20 s, none of them became an audible underrun with a ring of 3 x 128 or larger.

A ring of 2 x 128 (5.8 ms) also ran without underrun in the test, but with less than the observed
4.3 ms spike as reserve. 3 x 128 keeps about twice the worst measured spike as headroom.

The ring needs at least 2 descriptors (one playing, one being filled).

## Measurements

Board: CrowPanel 5" (ESP32-P4 rev v1.3, NS4168 output), 44.1 kHz, the preset loaded on the board,
5-note chord pattern (note on every 120 ms, 1 s hold, max. 4 voices active), 20-25 s per run.

| Block / DMA | Ring | Underruns playing | Underruns playing + stress | Peak render / budget | Free internal RAM |
|---|---|---|---|---|---|
| 512 / 6 x 240 | 32.7 ms | 0 | not measured | 5.1 / 11.6 ms | 90 KB |
| 256 / 4 x 128 | 11.6 ms | 0 | not measured | 3.3 / 5.8 ms | 145 KB |
| 128 / 4 x 128 | 11.6 ms | 0 | 0 (6 budget misses) | 2.2 (stress 3.7) / 2.9 ms | 170 KB |
| **128 / 3 x 128** | **8.7 ms** | 0 | **0** (4 budget misses) | 2.9 / 2.9 ms | 171 KB |
| 128 / 2 x 128 | 5.8 ms | not measured | 0 (4 budget misses) | 2.9 / 2.9 ms | 171 KB |

Audio-core load (share of the block budget): 12 % at block 512, 13 % at 256, 14 % at 128 (without
stress), 20-22 % with stress. Smaller blocks cost a bit more CPU (per-block overhead), but the
budget is not close to exhausted.

RAM: the block size scales the graph's block buffers (32 buffers: 64 KB at 512, 32 KB at 256,
16 KB at 128) and more internal buffers; the DMA ring itself shrinks from 5.8 KB to 3 KB. Together
the free internal RAM rose by about 80 KB.

## Underruns at boot: harmless gap, fixed

Before, every boot counted **40-87 underruns** (about 0.23 s dry time, the same for every ring size).
Cause: the codec managers enabled their I2S TX channel in `init()`, but the synth engine writes its
first block only later (after `Synth::init()` and the preset load). The DMA ring ran empty in
between and the driver counted each recycled descriptor as an underrun, although only silence was
lost. The first measured underrun happened at t = 1.9 s, before Wi-Fi was up (about 9 s).

With the synth engine built in (`CONFIG_APP_ENABLE_SYNTH`), ES8311, WM8960 and NS4168 now leave TX
disabled, and `SynthEngine` enables each channel right before its first block. Measured on the
CrowPanel: **0 underruns during boot** (before 86). ES8311 and WM8960 keep RX running, so BCLK/WS/MCLK
stay on; the ES8311 and WM8960 paths compile but were not run on hardware. Without the synth engine
the channels are enabled in `init()` as before.

## Reproduce

`tools/audio_latency_test.py` plays the chord pattern over the web server's WebSocket (`midiTest`
messages), reads the underrun lines from the serial log, and reads the engine statistics
(`cpuLoad` broadcast: load, p50/p99/peak render time, underruns, budget misses, active voices) and
`/api/diagnostics/memory`:

```
python tools/audio_latency_test.py <board-ip> <com-port> 20 22 --reset --stress
```

To test another setting, change `CONFIG_AUDIO_SYNTH_BLOCK_SIZE`, `CONFIG_AUDIO_I2S_DMA_DESC_NUM` and
`CONFIG_AUDIO_I2S_DMA_FRAME_NUM` in `sdkconfig` (or menuconfig), build and flash (~3 min), then run
the script.

## Limits of the measurement

- One board (CrowPanel, NS4168). The tables above use a light preset (at most 4 active voices). The
  heavier `FM8` preset (8 voices) reached 37-43 % audio-core load, 50-60 budget misses per 20 s and a
  peak render time of 4.4 ms (6.7 ms with the web-download stress) at block 128, and still had **0 DMA
  underruns** with 3 x 128, but only about 2 ms of reserve under stress. A preset heavier than that, or
  more load from other tasks, needs 4 x 128 (11.6 ms). Re-run the test with your heaviest preset
  before shrinking further.
- Not tested on the JC8012P4A1 (ES8311) or the Waveshare kit; they use the same driver code and the
  same ring settings.
- No acoustic measurement of the real note-to-sound delay; the latency column is calculated.
