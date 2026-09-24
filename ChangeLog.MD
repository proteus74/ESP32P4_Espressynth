# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden in dieser Datei dokumentiert.

## [Unreleased]

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

## [0.31]

- Firmware-Version zentral in `main/version.h` gepflegt und in WebUI (Info-Dialog) sowie CLI (`version`-Kommando) angezeigt.
