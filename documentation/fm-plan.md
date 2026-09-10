# FM-Operator: Untersuchung und Optimierungsplan

Stand: 10. September 2026. Diese Runde untersucht den bestehenden Arbeitsbaum, beide lokalen Build-Artefakte und lesend das erreichbare Gerät unter 192.168.1.121. Es wurden keine Firmwareänderungen vorgenommen, keine MIDI-Ereignisse gesendet und keine Presets verändert. Die Ursache der gemeldeten Regression ist noch nicht nachgewiesen.

## 1. Belastbare Befunde

### Build und erzeugter Code

- `build/ESP32P4_Espressynth.elf` und der bisher verwendete Build unter `%LOCALAPPDATA%/Temp/ESP32P4_Espressynth-codex-build` enthalten identisches Disassembly für `nodes::fmoscillator::render`.
- FM-Render liegt in `.iram0.text`, Adresse `0x4ff022ea`, Größe `0x23ae` (9.134 Byte). WaveGen-Render liegt ebenfalls in IRAM, Größe `0x1ebc` (7.868 Byte). ADSR-Render liegt in Flash. Daraus lässt sich keine konkrete Cache-bedingte Regression ableiten.
- Beide Builds kompilieren `main.cpp` mit `-O2`. Die App-Binärdateien sind jeweils 1.565.104 Byte groß; ihre vollständigen SHA256-Hashes unterscheiden sich. Identischer FM-Code ist kein Nachweis identischer Firmware insgesamt.
- Im FM-Disassembly wird das Phasen-`floorf()` inline unter anderem mit `fcvt.w.s ...,rdn` und `fcvt.s.w` umgesetzt. Die frühere Begründung, einen teuren `floorf`-Bibliotheksaufruf zu eliminieren, war für diesen Build nicht zutreffend.
- Die Frequenzumrechnung enthält tatsächliche Aufrufe von `modff` und `ldexpf`. Im konstanten Pitch-Pfad erfolgt sie einmal pro Block; im Audio-Pitch-Pfad pro Sample. Den gewählten Pfad muss eine Messung belegen.
- `/api/version` des Geräts meldet lediglich `3fee003-dirty`; damit lässt sich der laufende Binärstand nicht eindeutig einem lokalen Build zuordnen.

### Gerät und Preset

- Gelesener Preset: 16 angeforderte Stimmen, vier FM-Operatoren, vier ADSRs, MIDI-Pitch direkt zu allen Operatoren, Glide aus, Self Feedback überall 0.
- Diagnose: 133 Instanzen, `sizeof(NodeInstance)=768`, 214 Blockpuffer, 109.568 Pufferbytes, `instancesInternal=true`. Ein Ausweichen des Instanzarrays nach PSRAM erklärt den derzeit gelesenen Zustand nicht. Diese Angabe beweist nicht den Speicherort jeder separaten Allokation.
- Die beiden Modulator-ADSRs haben Sustain 0; ihre Decay-Zeiten betragen im gelesenen Preset 0,252 und 0,4 Sekunden. Die Carrier-ADSRs halten anschließend weiterhin positive Sustain-Werte. Dadurch bleibt die Stimme insgesamt aktiv, obwohl die Modulator-Ausgänge bereits stumm sind.
- Vier passiv empfangene Telemetrieintervalle zeigten etwa 7,9 % Core-1-Last und etwa 0,1 % pro FM-Widget. Das war eine Ruheaufnahme, kein Lastbenchmark. Overruns (376) und Underruns (10) waren kumulativ und stiegen während dieser Aufnahme nicht.
- Gelesener Preset und Screenshots unterscheiden sich teilweise in Parametern. Für einen A/B-Vergleich müssen deshalb Preset, MIDI-Folge und Messzeitpunkte eingefroren werden.

### Messung

`Graph::renderBlock()` misst mit `esp_timer_get_time()` alle Renderaufrufe eines Widgets über dessen Stimmen hinweg, einschließlich Schleifenverwaltung und möglicher Unterbrechungen. Die Anzeige ist EMA-geglättet; ausgehende Kanten werden separat erfasst. Der Prozentwert ist keine Zeit pro Stimme und keine ausschließlich im DSP ausgeführte Prozessorzeit.

Die vorhandenen `oscPitchConstant`, `oscAmpConstant` usw. werden vom WaveGen gepflegt. Der FM-Operator aktualisiert sie nicht. Damit fehlt ausgerechnet für das untersuchte Widget der Nachweis, welcher schnelle oder langsame Pfad ausgeführt wird.

## 2. Prüfung der FM-Pfade

Quellen: [FmOscillatorNode.h](../main/audio/engine/nodes/FmOscillatorNode.h), [DspPrimitives.h](../main/audio/engine/DspPrimitives.h), [NodeInstance.h](../main/audio/engine/NodeInstance.h), [Graph.h](../main/audio/engine/Graph.h).

1. **Pitch/Tune:** Direkter MIDI-Pitch ohne Glide sollte den vorberechneten Phasenschritt verwenden. Audio-Verbindungen werden auf Konstanz untersucht; wechselnde Werte aktivieren die Frequenzberechnung je Sample. Das ist funktional erforderlich, aber für den gezeigten Patch nicht erwartet.
2. **Amplitude:** Ein konstanter Block wird erkannt. Bei exakt null Amplitude läuft trotzdem die komplette Wellenformberechnung weiter. Der bisherige Silent-Skip prüft nur den separaten Gain-Regler. Das ist ein konkreter vermeidbarer Aufwand bei den beiden Modulatoren nach deren Decay.
3. **FM-Eingang:** Ein Nullblock wird bereits erkannt und verwendet den reinen Carrier-Pfad. Hier fehlt kein Nullsignal-Bypass. Bei konstantem Null-FM und konstantem ADSR werden allerdings jeweils Puffer gescannt, um diese Tatsache erneut festzustellen.
4. **Gain:** Ein konvergierter Smoother wird bereits übersprungen. Trotzdem stehen Amplituden-/Gain-Auswahl und Ausgangsskalierung in der Sample-Schleife. Ob zusätzliche Spezialisierung hilft, muss am erzeugten Code und auf Hardware geprüft werden.
5. **Feedback:** Feedback 0 wählt den unmodulierten Pfad, sofern auch FM inaktiv ist. Bei aktivem FM enthält die gemeinsame Schleife weiterhin Feedback-Abfragen. Eine vollständige Kombination aller Template-Varianten wurde bereits ohne nachgewiesenen Gewinn versucht und wird nicht wieder pauschal eingeführt.
6. **Wellenformen:** Auswahl bei konstantem Pitch erfolgt bereits einmal pro Block. Sinus verwendet eine interpolierte LUT. Phasenprüfung erfolgt sowohl vor dem Sinus-Lookup als auch innerhalb des Lookups; tatsächlichen verbleibenden Aufwand im Disassembly prüfen, bevor ein weiterer Spezialfall entsteht.
7. **Noise:** Externe Phasenmodulation und Feedback sind bereits deaktiviert, weil sie die Noise-Ausgabe nicht beeinflussen. Zufalls- und Pink-Filterzustände müssen bei späterem Silence-Skip weiterhin korrekt behandelt werden.
8. **Gate:** MIDI-Gate setzt Phase und Feedback beim Trigger zurück. Dieser Ereignispfad ist kein dauernder Sample-Kostenfaktor. Retrigger-Verhalten muss bei Optimierungen erhalten bleiben.

## 3. Umsetzung in messbaren Einzelschritten

### P0: Regression reproduzierbar eingrenzen

- Laufende Firmware eindeutig identifizieren: ELF-/App-Hash oder Build-ID mit Datum im Diagnosekanal erfassen; `-dirty` allein reicht nicht.
- Das gelesene Preset als Benchmarkkopie sichern. Definierte Noten und Velocity für 1, 8 und 16 Stimmen; identischer Release-/Retrigger-Ablauf, gleiche Display-/Netzwerklast.
- Attack/Decay, Sustain und Release getrennt messen. Pro Variante mehrere Wiederholungen; identische Noten sind auch für FM-Phasen-Wrap und Feedback relevant.
- FM-Zähler ergänzen: tatsächlich gerenderte Instanzen, konstanter/variabler Pitch, konstante/variable/null Amplitude, aktives/stilles FM, Feedback aktiv, Gain konvergiert. Nur blockweise zählen, außerhalb des Audiotasks ausgeben.
- Widget-Zeit durch tatsächlich gerenderte Stimmen normalisieren. Gesamtrenderzeit, P95/P99, Overrun-Differenz und Graphgeneration mitführen. Ein Benchmark darf nach Watchdog-Stimmenreduktion nicht unbemerkt mit weniger Stimmen weiterlaufen.
- Vorhandenes `tools/benchmark_synth.mjs` als Grundlage nutzen. Sein aktueller letzter `nodeCpu`-Wert wird nach Note-off gelesen; er ist wegen Release/EMA kein sauberer stabiler Hold-Vergleich. Node-Messwerte während der definierten Lastfenster sammeln.

### P1: Stumme Modulatoren billiger weiterführen

Erster Optimierungsversuch: Bei exakt null effektiver Blockamplitude, periodischer Wellenform, konstantem Pitch und Feedback 0 nur den nötigen Oszillator-/Smootherzustand fortschreiben und den Ausgang nullen. Externe FM verändert lediglich die Lesephase und kann hier entfallen.

Die Phase darf nicht eingefroren werden: Im Patch ist Gate am FM-Operator nicht verbunden; beim nächsten ADSR-Attack wäre sonst der Klang anders. Für die erste Variante dieselben einzelnen Float-Phasenschritte und dieselbe Wrap-Reihenfolge behalten. Ein pauschales `phase += frames * increment` ist nicht bit-identisch. Bei aktivem Self Feedback darf die interne Wellenform auch bei stummem Ausgang nicht entfallen. Noise/Pink Noise zunächst im bisherigen Pfad belassen; Gain-Smoothing muss weiterlaufen, sofern es noch nicht konvergiert ist.

Erwarteter Nutzen: weniger Arbeit in den stummen Modulatoren nach Decay. Das erklärt nicht automatisch die Regression und hilft nicht während ihrer hörbaren Attack-/Decay-Phase.

### P2: Den häufigsten hörbaren Pfad gezielt vereinfachen

Nur einen zusätzlichen kompakten Kandidaten messen: konstantes Pitch, konvergierter unverkabelter Gain, Feedback 0. Zunächst Sinus mit variabler ADSR-Amplitude und optionalem externen FM betrachten. Schleifeninvariante Werte und Zeiger lokal halten; verbleibende Loads, Verzweigungen, Stack-Spills und Codegröße im Disassembly vergleichen. Keine Kombinationsexplosion über alle Wellenformen und Portzustände.

Ausgangsskalierung nur dann zusammenfassen, wenn Rundungsabweichungen geprüft und akzeptabel sind. Das Umordnen von Float-Multiplikationen ist nicht generell bit-identisch.

### P3: Konstanzprüfungen und Phasenoperationen

- Zuerst vermeidbare FM-lokale Scans messen. Metadaten für konstante Produzentenausgaben wären später ein eigener Graph-Eingriff: Mehrfachquellen, Skalierung, Views, MIDI-Glide und Zyklen müssen korrekt bleiben. Ein verbundenes Audioport ist nicht allein aufgrund seines Produzententyps konstant.
- Den vorhandenen hardwarekompilierten `floorf` als Referenz behalten. Separate Versuche mit bekannt begrenztem Phasenschritt oder normalisierter LUT-Lesephase nur mit definierten Wertebereichen und geprüftem Extremwertpfad.
- Integerphase, kleinere LUT, globale Compileroptionen, Cacheänderungen und Kernaufteilung gehören nicht in die erste Korrekturrunde; sie ändern andere Eigenschaften oder erschweren die Ursachenanalyse.

## 4. Abnahmekriterien

Jede Änderung wird einzeln gegen P0 auf dem P4 verglichen und bei Regression verworfen. Erfolgreicher Build oder kleinere Binärdatei gelten nicht als Performancebeweis. Keine zugesagte Prozentersparnis vor Messung.

Funktionsprüfung: alle sechs Wellenformen, unverbundenes und angeschlossenes Null-FM, positives/negatives FM, Index 0/Maximum, Feedback 0/aktiv, Gate-Retrigger, MIDI-Glide, variable Amplitude/Gain, Stille und Wiederanlauf. Für P1 insbesondere die Samples nach längerer Stille mit der Referenz vergleichen; bei Feedback die Zustandsentwicklung prüfen. Den normalen Build über das Espressif-PowerShell-Profil und `idf.py build` prüfen.

Ziel: geringere FM-Zeit pro tatsächlich gerenderter Stimme im identischen Patch, ohne schlechtere P99/Overruns und ohne unbeabsichtigte Änderung des Klangs oder Zustandsverlaufs.

## 5. Codeabgleich mit nodes::oscillator (Ergänzung, gleiche Runde)

Zweiter, lesender Durchgang durch [FmOscillatorNode.h](../main/audio/engine/nodes/FmOscillatorNode.h), [OscillatorNode.h](../main/audio/engine/nodes/OscillatorNode.h), [NodeInstance.h](../main/audio/engine/NodeInstance.h), [DspPrimitives.h](../main/audio/engine/DspPrimitives.h), [EngineStats.h](../main/audio/engine/EngineStats.h), [SynthEngine.h](../main/audio/engine/SynthEngine.h) sowie ein repo-weiter Suchlauf nach SIMD/ESP-DSP-Nutzung. Auch diese Runde: keine Firmwareänderung, nur Lesen.

### Fast-Path-Struktur bereits deckungsgleich mit Oscillator

Der block-konstante Fast-Path (`FastLoopCtx` + `renderFastLoop<W>`/`renderSelectedFastLoop<W>`, Wellenform-Dispatch einmal pro Block statt pro Sample) existiert in FmOscillatorNode.h:71-145 bereits 1:1 im Oscillator-Muster (OscillatorNode.h:60-124). §2 Punkt 6 braucht hier also keinen neuen Spezialfall - der ist schon da.

Der einzige strukturelle Mehraufwand gegenüber Oscillator liegt in `renderModulatedLoop()` (FmOscillatorNode.h:112-134): zwei unabhängige Laufzeit-Flags (`c.fmActive`, `c.feedbackActive`, Zeilen 123/125) werden dort pro Sample geprüft, obwohl beide für den ganzen Block feststehen - Oscillators Äquivalent (`renderFeedbackLoop`) hat nur ein solches Flag, weil es nur Feedback kennt. Vor einer weiteren Template-Aufspaltung (§2 Punkt 5 markiert eine vollständige Kombination bereits als ohne nachgewiesenen Gewinn versucht) zuerst im Disassembly prüfen, ob diese zwei Branches bei bekanntlich-aktivem FM/Feedback überhaupt messbare Kosten verursachen, statt erneut pauschal weitere Varianten anzulegen.

### audioPortConstantForBlock(): Scan-Kosten präzisiert

`audioPortConstantForBlock()` (NodeInstance.h:324-340) ist ein linearer Scan, der beim ersten abweichenden Sample abbricht. Ein aktiver Audio-Modulator kostet dadurch nur wenige Vergleiche, aber ein block-konstanter - auch ein konstant-null - Puffer wird jeden Block erneut vollständig gescannt, ohne Gedächtnis über Blockgrenzen hinweg. Das bestätigt §2 Punkt 3 und P3 exakt und benennt den Mechanismus.

Einziges vorhandenes Vorbild für eine produzentenseitige Konstanz-Markierung im Graph: `NodeInstance::RampDescriptor`/`rampForPort()` (NodeInstance.h:365-386), aktuell ausschließlich für ADSR-Ramp-Segmente genutzt. Falls P3 später tatsächlich einen Graph-Eingriff für konstante Produzentenausgaben braucht, ist das der bestehende Ansatzpunkt, keine neue Idee.

### SIMD/ESP-DSP und DspPrimitives.h

Repo-weite Suche (`esp_dsp`, `dsps_`, `xtensa`, `PIE`, `HIFI4`) findet keine echte Verwendung in `main/` - nur unabhängige Treffer wie CHIP_ESP32P4-Fallunterscheidungen in Board-/USB-Code. Deckt sich mit der eigenen Warnung in native-synth-optimization-plan-2026-09-09.md (Abschnitt "Compiler, DSP-Library, zwei Kerne"), so etwas nicht ohne kernelspezifische Messung als Gewinn anzunehmen. Ändert nichts an der P0-P3-Reihenfolge hier.

DspPrimitives.h bietet ausschließlich skalare Pro-Sample-Primitive (`sineLookup`, `pow2Lookup`, `tanhLookup`, `PhaseOsc`), kein `processBlock()`-Äquivalent. Das ist eine engine-weite Eigenschaft, nicht FM-spezifisch, und gehört nicht in die erste FM-Korrekturrunde - deckt sich mit P3's eigener Ausschlussliste (Integerphase, kleinere LUT, Compiler/Cache/Kernaufteilung).

### Vorschlag für P0: FM-Zähler

`enginestats::BlockCounters` (EngineStats.h) pflegt `oscPitchConstant`, `oscTuneConstant`, `oscAmpConstant`, `oscSilentSkipped`, `oscGainConverged` usw. - geschrieben von OscillatorNode.h, durchgereicht über `SynthEngine::BlockStatsSnapshot`/`blockStatsSnapshot()` bis zu `log_p0_baseline_tick()`s ESP_LOGI-Zeile (synth_manager.hpp). FmOscillatorNode.h includet EngineStats.h aktuell nicht einmal.

Vorschlag, exakt an diesem bestehenden Muster gespiegelt, keine neue Konvention: analoge `fm*`-Felder ergänzen -

- `fmPitchConstant`/`fmPitchAudioRate`, `fmTuneConstant`/`fmTuneAudioRate` - an den zu Oscillator identischen Stellen (nach `pitchNeedsAudioRate`/`tuneNeedsAudioRate`, FmOscillatorNode.h:166/169).
- `fmAmpConstant`/`fmAmpAudioRate` - nach `ampConstantForBlock` (FmOscillatorNode.h:236f), nur wenn `ampBuf != nullptr`.
- `fmAmpConstantZero` - Diagnose-Zähler, zusätzlich zu `fmAmpConstant`, wenn `ampConstantValue` bereits ≈0 ist (vorhandene `kSilentGain`-Toleranz aus Zeile 212 wiederverwenden). Zählt vorab, wie oft P1s "stille Modulatoren nach Decay"-Fall tatsächlich eintritt, ohne P1 selbst schon zu bauen - genau der Fall aus §1 ("Modulator-ADSRs Sustain 0").
- `fmSilentSkipped` - der bereits vorhandene, aber bisher uninstrumentierte Gain-Null-Fastpath (FmOscillatorNode.h:213-217).
- `fmGainConverged` - analog zu Oscillator (FmOscillatorNode.h:218-223).
- `fmModActive`, `fmFeedbackActive` - je ein Zähler, wenn `fmActive` (Zeile 199) bzw. `feedbackActive` (Zeile 205) für die Instanz diesen Block wahr sind. Deckt §3/P0s "aktives/stilles FM" und "Feedback aktiv" ab.

Alle Inkremente block-rate (einmal pro `render()`-Aufruf), nicht in den per-Sample-Schleifen - gleiche Kostenklasse wie die bestehende Oscillator-Instrumentierung, kein Eingriff in Klang oder Zustand. Noch nicht umgesetzt; Umsetzung an vier Stellen nötig (EngineStats.h, FmOscillatorNode.h, SynthEngine.h, synth_manager.hpp), symmetrisch zu den vorhandenen `osc*`-Stellen dort.

### Nächster Schritt

Diese Zähler sind Voraussetzung für den Rest von P0: erst mit ihnen lässt sich am Gerät sehen, welchen Pfad eine FM-Instanz tatsächlich nimmt. Danach wie in §3 vorgesehen: Benchmark-Preset/MIDI-Folge einfrieren, `tools/benchmark_synth.mjs` gegen die neuen Zähler laufen lassen, erst danach P1 angehen.
