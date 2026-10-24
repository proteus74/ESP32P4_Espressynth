# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden in dieser Datei dokumentiert.

## [Unreleased]

- Audio: Der I2S-TX-Kanal von ES8311, WM8960 und NS4168 wird bei eingebauter Synth-Engine erst mit dem ersten Engine-Block eingeschaltet. Damit entfallen die 40–87 "Underruns" beim Booten (der DMA-Ring lief leer, bevor die Engine startete, es ging nur Stille verloren). Auf dem CrowPanel getestet (0 Underruns beim Booten); ES8311/WM8960 kompiliert, nicht auf Hardware.
- Audio-Latenz: neue Standardwerte DMA-Ring 3 × 128 Frames (vorher 6 × 240) bei Render-Block 128 (vorher in der lokalen `sdkconfig` 512). Note-Ein bis Ton sinkt von ca. 38 ms auf ca. 11 ms (berechnet), der freie interne RAM steigt um ca. 80 KB; auf dem CrowPanel keine DMA-Underruns beim Spielen, auch nicht mit parallelen Web-Downloads. Messung und Begründung: `documentation/audio-latency.md`, Testskript `tools/audio_latency_test.py`.
- Browser-Vorschau (`audio-engine.js`) an die Firmware angeglichen:
  - Filter und Curtis-Filter laufen jetzt als exakte Ports von `FilterNode.h`/`CurtisFilterNode.h` (lineares Q, 6-dB-Stufen, TPT-Kaskade mit Sättigung).
  - Reverb Light ist jetzt der Schroeder-Hall der Firmware statt eines Faltungshalls mit Zufalls-Impuls.
  - FM-Oszillator mit echter Phasenmodulation, Duty-Cycle, SuperSaw und Feedback wie `FmOscillatorNode.h`.
  - LFO: Phase wirkt wieder, Rate bis 500 Hz, Ausgang auf 0–10 V geklemmt; Wellenformen mit Firmware-Phasenlage.
  - Oszillator/Hammond/FM: Tonhöhe bei Pitch-/Tune-Kabel korrekt, Gain-Kabel mit richtigem Pegel; Hammond war 5× zu laut.
  - Bypass für Delay, Chorus und ESP-EQ; Graphic-EQ-Bypass umgeht auch den Ausgangspegel.
  - Divide, Frequency Divider, Logic OR, Value Mapper, Bitcrusher und Output Switch rechnen wie die Firmware.
  - SoundFont-Filter-Resonanz korrigiert, Gain-Eingang per Kabel möglich.
- Neuer Paritätstest `tools/audio_engine_parity` (Playwright, offline gerendert).
- Neue Browser-Einstellung „Load samples in the browser“ (Settings, nur lokal gespeichert): ist sie aus, laden Wave Player, SoundFont Player und MDA Piano beim Seitenaufruf keine Samples – ein blinkender „Load Samples“-Button neben dem Lautsprecher lädt sie bei Bedarf nach.

## [0.31]

- Firmware-Version zentral in `main/version.h` gepflegt und in WebUI (Info-Dialog) sowie CLI (`version`-Kommando) angezeigt.
