# Audio benchmark 2026-09-08

## Setup

- ESP32-P4 revision 1.3, CPU configured at 360 MHz
- 44.1 kHz, 128 samples per block, 2902 us block budget
- WM8960 output enabled; control and telemetry over Ethernet WebSocket
- MIDI notes injected through the existing `midiTest` WebSocket message
- Firmware built with `CONFIG_AUDIO_SYNTH_MAX_VOICES=32`

Run a repeatable test with Node.js 24 or newer:

```powershell
node tools/benchmark_synth.mjs --host=192.168.1.121 --voices=32 --seconds=30
```

## Mixed preset

`Test8Voices` contains two oscillators, two ADSRs, a SoundFont player with up
to 32 internal voices, output routing, and bypassed Reverb Light. Results are
the worst percentile/peak observed across each set of one-second windows.

| Active voices | Core 1 average | Worst P99 | Worst peak | Overruns |
|---:|---:|---:|---:|---:|
| 4 | 29.3% | 960 us | 1767 us | 0 / 5 s |
| 8 | 50.7% | 1600 us | 2485 us | 0 / 5 s |
| 10 | 61.4% | 1920 us | 2746 us | 0 / 5 s |
| 12 | 72.2% | 2304 us | 3283 us | 2 / 5 s |
| 13 | 77.4% | 2432 us | 3373 us | 2 / 5 s |
| 14 | 82.9% | 2624 us | 3701 us | 5 / 6 s |
| 15 | 88.3% | 2752 us | 3636 us | 6 / 6 s |
| 16 | 93.6% | 2944 us | 3760 us | 17 / 6 s |
| 17 | 99.4% | 3136 us | 4178 us | 419 / 4 s |
| 18 | 104.9% | 3264 us | 4382 us | 1220 / 4 s |
| 20 | 116.0% | 3648 us | 4696 us | 1103 / 4 s |
| 22 | 126.9% | 3904 us | 5246 us | 1011 / 4 s |

This preset is reliable through 10 voices. Twelve through sixteen voices play,
but produce occasional deadline misses. Seventeen and above are not real-time
safe. At 16 voices, the largest measured widget costs were the sine oscillator
(26.3%), square oscillator (20.4%), SoundFont player (21.2%), and both ADSRs
(6.9% combined).

## Lean oscillator preset

`BenchmarkOsc32` was created on the device with one sine oscillator, one ADSR,
MIDI input, and direct WM8960 output. It requested and received all 32 voices.

The 30-second, 32-note held-chord run measured:

- Core 1 average/median/maximum: 75.5% / 75.5% / 75.5%
- Worst P95/P99: 2304 us / 2304 us
- Worst peak: 2624 us (278 us below the block budget)
- Overruns: 0
- MIDI drops: 0
- Graph rebuilds during hold: 0
- Internal heap after test: 282607 bytes free, 131072-byte largest block
- Synth task stack high-water reserve: 30252 bytes

The 32-voice target is therefore achieved for a conventional single-oscillator
voice. The mixed preset is CPU-bound because every voice runs two oscillators
plus SoundFont synthesis; increasing only the compile-time cap cannot make that
specific graph real-time safe at 32 voices.

## Findings during the run

- The CPU-watchdog rebuild originally allocated the replacement beside the
  high-polyphony live graph, causing a requested 32-to-31 reduction to collapse
  to 16 from temporary RAM pressure. Watchdog and panic rebuilds now pre-free
  the old graph.
- A reduced graph's MIDI allocator still used the preset voice request rather
  than the effective graph cap. It now uses the effective cap, preventing voice
  indices outside the allocated instance pool.
- Timing telemetry now reports per-second P50/P95/P99 values using a 64-us,
  128-bin histogram in addition to peak and overrun counters.