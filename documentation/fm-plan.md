# FM-Operator: Untersuchung und Optimierungsplan

Stand: 12. September 2026. Die ursprüngliche Untersuchung und der Plan bleiben unten als Entscheidungsgrundlage erhalten. Die Abschnitte 6 und 7 dokumentieren die inzwischen umgesetzten Änderungen und die Messungen auf dem ESP32-P4 mit dem Preset `Benchmark`.

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

### Verbindliches Benchmark-Preset (Ergänzung vom 12. September 2026)

Für alle weiteren Performance- und Speichermessungen wird das Preset `Benchmark` verwendet. Es enthält sechs FM-Operatoren und sechs DX-Envelopes. Die maximale und zugleich höchste zu prüfende Stimmenzahl ist 16; Messungen mit 32 Stimmen entfallen. Die bisherigen Messungen am Preset mit vier FM-Operatoren und vier ADSRs bleiben als historische Befunde erhalten, sind aber nicht die Vergleichsbasis für neue A/B-Messungen.

- Der Aufbau ist DX7-E-Piano-artig: drei parallele Zwei-Operator-Stränge. Von links nach rechts modulieren FM 1, FM 3 und FM 5 jeweils die nachfolgenden Carrier FM 2, FM 4 und FM 6. Die drei Carrier-Ausgänge werden am Master summiert. Diese Beschreibung bezeichnet die Strukturähnlichkeit und nicht den Anspruch auf eine exakte Emulation eines bestimmten Yamaha-Presets.
- Jeder der sechs FM-Operatoren besitzt eine eigene DX-Envelope am Amplitudeneingang. MIDI-Pitch wird gemeinsam an alle Operatoren verteilt; Gate und Velocity steuern die DX-Envelopes. Verkabelung, Operator-Reihenfolge und Carrier-Summe sind Teil der Benchmarkdefinition und dürfen zwischen A/B-Läufen nicht verändert werden.
- Primäre Lastpunkte: 1, 4, 8, 12 und 16 gleichzeitig aktive Stimmen. Die Punkte 1, 8 und 16 sind für jeden Kandidaten verpflichtend; 4 und 12 dienen zur Prüfung der Skalierung.
- Bei 16 Stimmen werden je 96 FM-Operator- und DX-Envelope-Instanzen gerendert. Die FM-Zustände umfassen 6.912 Byte Nutzdaten und sind im aktuellen Stand direkt in den Instanzen eingebettet; die früheren 96 separaten Heap-Allokationen entfallen. Die DX-Zustände umfassen 8.064 Byte. Das sind Teilwerte, nicht der gesamte Graphspeicher.
- Die Verkabelung bildet bei 16 Stimmen 48 Zwei-Operator-Stränge. Im gespeicherten Preset besitzt jedoch nur der erste Carrier einen wirksamen FM-Index; die beiden anderen Carrier haben FM-Index 0. Der normale Benchmark enthält deshalb 16 aktive Modulator-zu-Carrier-Pfade. Ein zusätzlicher, klar gekennzeichneter `true3`-Lasttest setzt die beiden anderen Carrier vorübergehend ebenfalls auf FM-Index 1,89 und prüft damit 48 aktive FM-Pfade. Nach diesem Test wird das gespeicherte Preset erneut geladen.
- Vor der ersten Messung werden Preset-JSON beziehungsweise Preset-Hash, Verkabelung und alle Parameter festgehalten. Jede Änderung daran erzeugt eine neue Baseline.
- Je Lastpunkt werden identische MIDI-Noten, Velocity, Notendauer und Wiederholungsfolge verwendet. Getrennte Messfenster erfassen Idle, Attack/Decay, Sustain, Release und Retrigger.
- Die tatsächlich aktive Stimmenzahl muss im Messprotokoll stehen. Ein Lauf, bei dem der Watchdog die Stimmenzahl reduziert, ist als Vergleichslauf ungültig.
- Pro Lauf werden Gesamt-Renderzeit, P95/P99, Overrun-Differenz, Heap/PSRAM vor und nach Graphaufbau sowie FM- und DX-Zeit pro tatsächlich gerenderter Instanz erfasst.

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
- Das Preset `Benchmark` als unveränderte Benchmarkkopie sichern. Definierte Noten und Velocity für 1, 4, 8, 12 und maximal 16 Stimmen; identischer Release-/Retrigger-Ablauf, gleiche Display-/Netzwerklast.
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

## 6. Umgesetzter Stand und Hardwaremessung vom 12. September 2026

Die Optimierungsrunde ist umgesetzt und auf dem Gerät unter `192.168.1.121` geprüft. Vergleichsbasis ist das Preset `Benchmark` mit SHA256 `28829ab52a95add3fb4ece4fd4569975d4e696f42d30f4310574dd771e3f25a0`, sechs FM-Operatoren, sechs DX-Envelopes und 16 angeforderten Stimmen.

### Änderungen

- `tools/benchmark_synth.mjs` wählt das Benchmark-Preset selbst, begrenzt Eingaben auf maximal 16 Noten und misst Idle, Attack/Decay, Sustain, Release und Retrigger. Preset-Hash, tatsächliche Stimmenzahl, Speicher, Quantile, Overruns sowie FM-/DX-Pfade werden im JSON-Ergebnis festgehalten.
- FM-Telemetrie unterscheidet konstanten und audiogetriebenen Pitch/Tune/Amplitude, Nullamplitude, Silent-Skip, konvergierten Gain, externe Modulation und Feedback. DX-Telemetrie unterscheidet Hold, lineare Rampe, Segmentgrenze und dynamische Shape-Konfiguration.
- `DxEnvelopeScheduler` hält nur noch das aktive Segment statt drei vorberechneter Segmente. Der lineare Blockpfad verarbeitet reine Rampen ohne Scheduler-Prüfung pro Sample. Unverkabelte Shape-Ports überspringen die acht Portprüfungen; Null-Holds verwenden `memset`.
- FM-Phasenkonvertierung verwendet eine inline kompilierte `floorf`-Reduktion statt `fmodf`. Die Konvertierung wurde mit einer Million zufälliger Offsets im Bereich -6 bis +6 Zyklen gegen die alte Variante geprüft; die 32-Bit-Ergebnisse waren identisch. Die Square-Duty-Grenze behandelt 10 V nun korrekt als 100 Prozent statt als zyklischen Nullpunkt.
- Große typabhängige Zustände, einschließlich FM, werden nur für den passenden Knotentyp angelegt. Kleine exklusive Zustände teilen eine korrekt per Placement-New aktivierte Inline-Union. `NodeInstance` sinkt dadurch von 848 auf 376 Byte. `DxEnvelopeState` sinkt von 108 auf 84 Byte.
- Die Status-Telemetrie bleibt außerhalb des Audiotasks. Ihre beiden großen JSON-Puffer liegen statisch in BSS. Eine Zwischenversion legte beide Puffer auf den kleinen `app_main`-Stack und verursachte den beobachteten Reset-Loop; diese Ursache wurde per Panic-Backtrace bestätigt und behoben. Acht HTTP-Prüfungen über 24 Sekunden sowie die anschließenden vollständigen Benchmarkläufe blieben stabil.

### Speicherergebnis

Vor der Änderung musste der Graph wegen internen RAMs von 16 auf 12 Stimmen reduziert werden: 148 Instanzen zu je 848 Byte, 236 Blockpuffer und 120.832 Pufferbytes. Der umgesetzte Build hält alle 16 Stimmen intern: 196 Instanzen zu je 376 Byte, 308 Blockpuffer und 157.696 Pufferbytes. Während der finalen Messung blieben 91.091 Byte internes RAM frei. Trotz 48 zusätzlicher Instanzen sinkt die Summe aus Instanzarray und Blockpuffern von 246.336 auf 231.392 Byte.

### Zwischenstand vor den weiteren engineweiten Optimierungen

Blockbudget: 2.902 µs. Je Lastpunkt wurden zwei Telemetrieintervalle pro Phase ausgewertet.

| Noten | Attack CPU | Attack P95/P99 | Sustain CPU | Sustain P99 | Ergebnis |
|---:|---:|---:|---:|---:|---|
| 1 | 10,9 % | 384/640 µs | 10,6 % | 576 µs | ohne Overrun |
| 4 | 26,0 % | 896/1.152 µs | 25,4 % | 1.152 µs | ohne Overrun |
| 8 | 54,0 % | 1.728/2.304 µs | 51,3 % | 1.984 µs | Attack/Sustain ohne Overrun |
| 12 | 81,5 % | 2.624/3.520 µs | 79,0 % | 2.944 µs | nahe Echtzeitgrenze, vereinzelte Overruns |
| 16 | 110,9 % | 3.584/4.224 µs | 107,2 % | 3.968 µs | Speicher passt, Echtzeitbudget wird überschritten |

Der alte 8-Noten-Lauf lag bei 51,6 Prozent, P95 1.856 µs, P99 2.432 µs und Peak 2.906 µs. Der neue Sustain-Lauf liegt bei 51,3 Prozent und P99 1.984 µs; der neue Attack-Lauf bei P95 1.728 µs, P99 2.304 µs und Peak 2.601 µs. Der wesentliche Gewinn dieser Runde ist Speicher und geringere Zeitstreuung, nicht eine große Senkung der mittleren FM-Last.

Ein zusätzlicher, vollständig spezialisierter Schleifenpfad für externe FM ohne Self-Feedback wurde auf Hardware ausprobiert und verworfen. Er reduzierte den modulierenden FM-Widgetwert um etwa 1 bis 1,5 Prozentpunkte, vergrößerte aber den Hot-Code um rund 5,5 KiB und erhöhte die gesamte Core-1-Last bei 16 Stimmen um etwa 7 bis 8 Prozentpunkte. Diese Variante ist nicht Bestandteil des Endstands.

## 7. Weitere engineweite Optimierungen und Abschlussmessung

Nach dem Zwischenstand aus Abschnitt 6 wurden Speicherbewegungen im Graph, Metadatenübergaben zwischen DX-Envelope und FM-Operator sowie die häufigsten FM-Schleifen untersucht. Jede Änderung wurde einzeln auf dem ESP32-P4 gebaut, geflasht und mit dem Benchmark-Preset gemessen.

### Umgesetzte Änderungen

- Der `FmOscillatorState` liegt nun direkt in der typabhängigen Union von `NodeInstance`. Die separate Allokation und Freigabe für jede FM-Instanz entfallen. Bei 96 FM-Instanzen spart das 6.912 Byte separate Heap-Allokationen und verbessert die Datenlokalität. `sizeof(NodeInstance)` bleibt 376 Byte, weil die größte Union-Alternative bereits größer war.
- Polyphone direkte Verbindungen vom MIDI-Pitch zum FM-Operator verwenden den ohnehin vorhandenen skalaren Kontrollwert statt eines identischen Blockpuffers pro Operator und Stimme. Der Pitch-Blockpuffer bleibt erhalten, wenn `Voices = 1`, weil nur dann Glide aktiv sein kann und der Verlauf samplegenau sein muss. Indirekte oder nicht eindeutig als polyphoner `MidiIn` erkannte Quellen bleiben konservativ gepuffert.
- Der Graph erkennt den engen Fall `DX Envelope -> FM Amplitude` ohne Monitor, Zusatzskalierung sowie Pitch-/Tune-Audiopuffer. Die DX-Envelope übergibt bei Hold und linearer Rampe konstante beziehungsweise Rampenmetadaten, ohne den vollständigen Ausgabepuffer zu materialisieren. Andere Topologien verwenden weiterhin den allgemeinen Pufferpfad.
- Der übliche externe Phasenoffset von maximal ±16 Zyklen wird mit einer direkten Signed-5.27-Konvertierung berechnet. Extremwerte verwenden weiterhin den allgemeinen Pfad. Der Testbereich berücksichtigt, dass die Float-Eingabe bereits weniger Präzision als die verworfenen fünf niederwertigen Bits besitzt.
- FM-Amplitude, Gain und die Ausgangsskalierung 5 werden im konstanten Pfad einmal pro Block zusammengefasst. Im Rampenpfad wird wenigstens `gain * 5` aus der Sample-Schleife gezogen. Ein Hosttest mit vier Millionen Zufallswerten und den Gains 1 sowie 0,189 ergab eine maximale Float-Abweichung von `4,768e-7`; 632 von vier Millionen konvertierten PCM-Samples unterschieden sich um genau einen Integer-Schritt, kein Sample um mehr.
- Das Benchmarkwerkzeug lädt das Preset vor jedem Stimmenpunkt neu. Es protokolliert pro Phase `voiceMax`, Graphgeneration und die tatsächlich wirksame Mindeststimmenzahl; Änderungen machen den Lauf ungültig. Dadurch können Watchdog-Reaktionen oder Zustände eines vorherigen Lastpunkts Ergebnisse nicht mehr unbemerkt verfälschen.

### Speicherergebnis

Bei 16 Stimmen enthält der Graph weiterhin 196 Instanzen zu je 376 Byte. Die Zahl der Blockpuffer sinkt von 308 auf 212, die Pufferbelegung von 157.696 auf 108.544 Byte. Die vermiedenen 96 Pitchpuffer sparen 49.152 Byte pro Graph. Während der Abschlussmessung meldete das Gerät 146.395 Byte freien internen Speicher, einen größten freien Block von 45.056 Byte und ein bisheriges Minimum von 53.460 Byte. Gegenüber dem Stand mit 308 Puffern stieg der freie interne Speicher um ungefähr 57 bis 60 KiB; darin sind auch die entfallenen separaten FM-Allokationen enthalten.

### Isolierter Abschlusslauf

Blockbudget: 2.902 µs. Vor jedem Lastpunkt wurde `Benchmark` neu geladen. Alle Punkte blieben bei 16 erlaubten Stimmen, unveränderter Graphgeneration und ohne DMA-Underrun gültig.

| Noten | Attack CPU | Sustain CPU | Retrigger CPU | Sustain P95/P99 | Renderzeit Sustain | Ergebnis |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 9,9 % | 9,8 % | 9,8 % | 384/576 µs | 6,96 % | ohne Overrun |
| 4 | 22,3 % | 20,6 % | 21,2 % | 768/1.088 µs | 17,28 % | ohne Overrun |
| 8 | 36,2 % | 34,6 % | 36,7 % | 1.216/1.472 µs | 31,00 % | ein Retrigger-Overrun |
| 12 | 51,4 % | 49,4 % | 51,9 % | 1.664/2.176 µs | 45,15 % | zwei Retrigger-Overruns |
| 16 | 66,2 % | 63,7 % | 66,8 % | 2.112/2.688 µs | 59,28 % | sechs Sustain-Overruns, keine Underruns |

Der sauber isolierte Ausgangsstand vor dieser Runde lag im Sustain bei 13,1, 34,1, 62,7, 90,2 und 120,3 Prozent für 1, 4, 8, 12 und 16 Noten. Der Abschlussstand liegt bei 9,8, 20,6, 34,6, 49,4 und 63,7 Prozent. Insbesondere der 16-Noten-Mittelwert liegt nun innerhalb des Echtzeitbudgets; Sustain-P99 bleibt mit 2.688 µs ebenfalls unter 2.902 µs. Seltene Einzelblöcke überschreiten das Budget, werden jedoch ohne DMA-Underrun aufgefangen.

Der zusätzliche `true3`-Test mit allen drei Carrier-Indizes auf 1,89 belastet bei 16 Noten 48 aktive externe FM-Pfade. Er sank von 99,7 Prozent CPU, 95,41 Prozent Renderzeit, 286 Overruns und sieben DMA-Underruns auf 78,9 Prozent CPU, 74,70 Prozent Renderzeit, 33 Overruns und keinen DMA-Underrun. Danach wurde das unveränderte Preset `Benchmark` wieder geladen.

Ein eigener Glide-Test bestätigte die Pufferentscheidung auf dem Gerät. Mit `Voices = 1`, aktiviertem Glide und zwei Sekunden Glidezeit besaß der Graph 38 Blockpuffer; während des Übergangs meldeten alle sechs FM-Operatoren `pitchAudio`, anschließend wieder `pitchConst`. Mit `Voices = 16` und absichtlich gesetztem Glide-Flag besaß der Graph 212 Blockpuffer und meldete ausschließlich `pitchConst`, weil Glide in diesem Modus nicht aktiv ist. Nach dem Test wurden der unbenannte Testspeicher und der aktive Graph wieder auf den Inhalt des unveränderten Presets `Benchmark` gesetzt (`Voices = 16`, Glide aus).

### Ergebnis

Das Preset `Benchmark` läuft nun mit maximal 16 Stimmen im Mittel in Echtzeit und ohne beobachtete DMA-Underruns. Der größte Speichergewinn stammt aus den für polyphonen Direkt-Pitch überflüssigen Blockpuffern; Mono-Glide bleibt vollständig erhalten. Die größten CPU-Gewinne stammen gemeinsam aus der DX-Metadatenübergabe, der schnelleren Phasenoffset-Konvertierung und dem Herausziehen konstanter Ausgangsmultiplikationen aus den FM-Sampleschleifen. Der bestehende Audio-Watchdog muss für diesen Stand nicht gelockert werden.

## 8. Korrektur der verzögerten DX-Attack

Der erste Stand der DX-Metadatenübergabe enthielt einen Fehler: Während einer als Metadaten übertragenen Attack-Rampe materialisierte die DX-Envelope absichtlich keinen Ausgangspuffer. `audioPortConstantForBlock()` untersuchte trotzdem zuerst den gemeinsam genutzten Puffer aus dem vorherigen Block. Direkt nach dem Gate-On enthielt dieser noch Nullen, weshalb der FM-Operator die gesamte Attack fälschlich als konstante Stille behandelte. Erst am Segmentende schrieb der DX-Grenzpfad echte Samples; dadurch setzte der Ton schlagartig bei hohem Pegel ein und konnte knacken.

Die Konstanzprüfung priorisiert nun ein vorhandenes Rampen-Tag vor jedem Pufferscan. Der FM-Operator erhält dadurch ab dem ersten Attack-Block `ampRampStart` und `ampRampStep`. Ein Hardwaretest mit sechs DX-Envelopes, sechs FM-Operatoren und auf zwei Sekunden verlängertem Attack zeigte während aller beobachteten Attack-Intervalle sechs aktive DX-Rampen und sechs FM-Amplitudenpfade mit `ampAudio`; DMA-Underruns blieben bei null. Danach wurden Testdaten und aktiver Graph wieder auf das unveränderte Preset `Benchmark` zurückgesetzt.
