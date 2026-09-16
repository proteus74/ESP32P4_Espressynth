# Plan: SuperSaw im FM-Operator

Der FM-Operator erhält eine eigenständige, JP-inspirierte SuperSaw aus sieben echten Sägezahn-Phasen (Center plus drei symmetrisch verstimmte Paare), steuerbares `SuperSaw Detune` und `SuperSaw Mix` sowie ein bewusst auf sieben gleich breite Bereiche erweitertes Waveform-CV-Mapping. Die Implementierung bleibt auf den FM-Operator begrenzt, erhält bestehende Presets durch Defaults und bewahrt die optimierten normalen FM-Pfade. Zusammen mit der Erweiterung wird der vollständige Echtzeit-Audiopfad gezielt für den ESP32-P4 profiliert und durchoptimiert; Optimierungen gelten erst nach Messung auf echter Hardware als abgeschlossen.

## Phase 1: Typen, Parameter und Kompatibilität

1. `WaveType::SuperSaw` und den String `supersaw` ergänzen, aber den Parser typabhängig absichern: Nur `FmOscillator` darf den neuen Wert aus Preset oder Live-Update übernehmen; `Oscillator` und `Lfo` fallen bei diesem Wert weiterhin auf `Sine` zurück. Dadurch bleiben deren nicht erweiterte Render-Switches vollständig und sicher.
2. `WidgetParams` und `FmOscillatorState` um `superSawDetuneCv` und `superSawMixCv` mit rückwärtskompatiblen Defaults von jeweils 5 V erweitern. Zusätzlich sechs schlanke `uint32_t`-Phasenakkumulatoren für die Seitenstimmen speichern; der bestehende `PhaseOsc` bleibt Center-Stimme, verwendet intern bereits einen `uint32_t`-Akkumulator und behält seine normale Phasenkontinuität.
3. FM-Preset-Parsing, Build-Übernahme und `setParam()` um `superSawDetune`/`superSawMix` ergänzen. Gate-Rising-Edge setzt Center-, Seitenphasen und Feedback gemeinsam zurück; alte Presets ohne neue Felder laden unverändert mit den Defaults.

## Phase 2: Native DSP

**Integer-Phasenakkumulatoren:** Alle sieben SuperSaw-Phasen werden als 32-Bit-Integer (`uint32_t`) geführt. Eine vollständige Periode entspricht dem Wertebereich $0 \ldots 2^{32}-1$. Die Fortschreibung besteht im Sample-Hotpath nur aus `phase += phaseIncrement`. Der nach dem C++-Standard definierte Überlauf eines unsigned Integers führt automatisch den Wrap-around modulo $2^{32}$ aus. Dadurch entfallen pro Sample zusätzliche `if`-Abfragen, `fmodf()`, Modulo-Operationen und Gleitkomma-Normalisierung. Phasenoffsets für FM und Feedback werden ebenfalls in diese Integer-Darstellung umgerechnet und durch unsigned Addition zyklisch angewendet.

4. In `PhaseOsc` eine kleine öffentliche Saw-Auswertung für einen gegebenen Integer-Phasenwert bereitstellen, ohne Noise-/Pink-Noise-Zustand zu duplizieren. Darauf im FM-Knoten einen dedizierten SuperSaw-Sampler aufbauen, statt den normalen `PhaseOsc` siebenmal pro Sample aufzurufen. Center- und Seitenphasen verwenden dieselbe 0.32-Festkommadarstellung, damit Auswertung, FM-Offset und Wrap-around identisch arbeiten.
5. Die sieben Frequenzen als Center plus symmetrische Paare mit maximal ±7, ±14 und ±28 Cent definieren. `superSawDetune` bildet 0-10 V auf 0-100 % dieser Spreizung ab; Frequenzverhältnisse werden einmal pro Block berechnet, nicht pro Sample. Bei Audio-Rate-Pitch werden nur die sieben Inkremente pro Sample neu abgeleitet.
6. `superSawMix` so definieren: 0 V = nur Center; 10 V = alle sieben Saws gleich gewichtet. Für einen normierten Parameter `m` haben Center und jede Seitenstimme vor der Summennormalisierung Gewichte 1 und `m`; Division durch `1 + 6m` hält den Pegel begrenzt. Der gemeinsame externe FM-Phasenoffset wirkt auf alle sieben Stimmen; Self-Feedback verwendet die bereits normalisierte Gesamtsumme des vorherigen Samples.
7. Einen dedizierten SuperSaw-Pfad in beide Pitch-Zweige von `nodes::fmoscillator::render()` integrieren: blockkonstanter Pitch mit sieben einmal pro Block vorberechneten `uint32_t`-Inkrementen sowie Audio-Rate-Pitch. Im inneren Sample-Loop werden die Phasen ausschließlich per unsigned Integer-Addition fortgeschrieben. Die vorhandenen sechs templatisierten Fast-Paths für alle anderen Wellenformen bleiben unverändert. Der Silence-Fast-Path schreitet Center und alle sechs Seitenphasen durch `phase += phaseIncrement * frames` in einem Bulk-Schritt fort; wegen der modulo-$2^{32}$-Arithmetik ist dies zum schrittweisen Fortschreiben bitgenau äquivalent.
8. `waveTypeFromCv()` nur im FM-Knoten auf sieben gleiche Bänder ändern: Sine, Square, Triangle, Sawtooth, Noise, Pink Noise, SuperSaw. Damit ist die gewünschte CV-Änderung explizit; das 6-Band-Mapping von normalem Oszillator und LFO bleibt unverändert.

## Phase 3: Ports, UI und Presets

9. FM-Eingänge `superSawDetune` auf Index 9 und `superSawMix` auf Index 10 ergänzen; `audioInPortCount` auf 11 setzen und beide in `isControlOnlyInput` aufnehmen. Alle Bitprüfungen verwenden `synth::portBit()`. `kMaxAudioInPorts = 22` muss nicht verändert werden.
10. Im Frontend eine FM-spezifische Waveform-Buttonliste mit SuperSaw-Label und passendem Icon einführen; WaveGen und LFO verwenden weiter ihre bisherigen Listen. Das UI-Mapping des FM-Waveform-CV folgt denselben sieben Bereichen wie die Firmware.
11. FM-State, Node-Erstellung, Preset-Serialisierung/-Anwendung und Live-Updates um `superSawDetune` und `superSawMix` erweitern. Zwei Slider/Portzeilen im vorhandenen 0-10-V-Muster ergänzen. Die vollständigen Zeilen für `SuperSaw Detune` und `SuperSaw Mix` sind ausschließlich sichtbar, wenn `waveType === "supersaw"`; bei Sine, Square/PWM, Triangle, Sawtooth, Noise und Pink Noise bleiben sie ausgeblendet. Beim Wechsel zu SuperSaw werden beide Zeilen unmittelbar eingeblendet, beim Verlassen unmittelbar ausgeblendet. Ist der jeweilige CV-Port verbunden, bleibt die sichtbare Zeile erhalten, aber der zugehörige Slider wird deaktiviert. Dieses Verhalten folgt dem wellenformabhängigen UI-Muster des Duty-Cycle/PWM-Reglers. Preset-Laden, Live-Wellenformwechsel und Verbindungs-Refresh müssen Sichtbarkeit und Sliderzustand neu synchronisieren.
12. Die Web-Audio-Vorschau um sechs zusätzliche Saw-Oszillatoren erweitern. Sie teilen Pitch, FM und Feedback mit der Center-Stimme, erhalten die Detune-Centwerte über ihre `detune`-AudioParams und werden über Center-/Side-Gains gemäß Mix-Definition summiert. Die zwei neuen CV-Ports werden in Connect/Disconnect/Resolve und `setParam()` aufgenommen. Die bestehende Browser-FM bleibt eine dokumentierte Approximation; die Firmware ist klanglich maßgeblich.

## Phase 4: Vollständige ESP32-P4-Optimierung der Audio-Engine

13. Den gesamten nativen Renderpfad vom Graph-Dispatch über alle aktiven Nodes bis zu den Hardware-Ausgabepuffern auf dem ESP32-P4 messen. Vor jeder Optimierung Baseline-Werte für mittlere Last, Spitzenlast, Blocklaufzeit, Overruns, aktive Stimmen sowie internen RAM und PSRAM erfassen. Optimiert wird profilergeführt; reine Quellcode-Vermutungen gelten nicht als Nachweis.
14. Die Blockfrist aus `kBlockSize / 44100` als harte Echtzeitgrenze behandeln. Bei der Standard-Blockgröße 128 stehen rund 2,90 ms pro Audioblock zur Verfügung. Für definierte Referenzpresets gilt als Ziel: keine Underruns und mindestens 20 % zeitliche Reserve, also eine gemessene Spitzenlast von höchstens 80 % der Blockfrist. Extrempresets oberhalb dieser Grenze müssen kontrolliert durch die vorhandene Voice-Limit-/Überlastlogik abgefangen werden.
15. Alle Sample-Hotpaths für den ESP32-P4 prüfen und geeignete Funktionen mit `IRAM_ATTR` im Instruction RAM halten. Innerhalb der Sample-Loops sind virtuelle Aufrufe, Stringvergleiche, Logging, Locks, Queuezugriffe, Heap-Allokationen, Divisionen, `powf()`, `fmodf()` und andere vermeidbare Bibliotheksaufrufe ausgeschlossen. Wellenform- und Pfadauswahl erfolgt höchstens einmal pro Block.
16. Blockkonstante Werte konsequent vorberechnen: Detune-Verhältnisse, sieben Phaseninkremente, Mix-Gewichte, Ausgangsskalierung, FM-/Feedback-Skalierung und CV-Zustände werden außerhalb des inneren Sample-Loops berechnet. Templatisierte bzw. spezialisierte Schleifen erhalten die bestehenden Fast-Paths für konstante Amplitude, lineare Rampen, stumme Signale und inaktives FM/Feedback.
17. Echtzeitkritischen Zustand und Blockpuffer im internen RAM halten und cachefreundlich anordnen. PSRAM bleibt auf große, nicht samplekritische Daten beschränkt. `FmOscillatorState` wird auf Größe, Alignment und zusammenhängende Zugriffe geprüft; die sechs zusätzlichen Seitenphasen bleiben kompakte `uint32_t`-Werte ohne vollständige `PhaseOsc`-Duplikate.
18. Der Render-Task bleibt frei von dynamischer Speicherverwaltung und blockierenden Synchronisationsmitteln. Preset-Parsing, Graph-Aufbau, Dateioperationen, WebSocket-Verarbeitung und andere nicht echtzeitfähige Arbeiten verbleiben außerhalb des Audio-Tasks. Übergaben an den Renderpfad verwenden weiterhin die vorhandenen begrenzten Queues und blockweisen Zustandswechsel.
19. Compiler und Build für den ESP32-P4 im Performance-Profil prüfen: ESP-IDF-Performanceoptimierung aktivieren, generierten RISC-V-Code der heißesten SuperSaw-/FM-Schleifen stichprobenartig kontrollieren und sicherstellen, dass Integer-Phasenfortschreibung, konstante Skalierungen und Schleifenstruktur ohne unerwartete Helper-Aufrufe kompiliert werden. Optimierungen dürfen keine globale `fast-math`-Änderung erzwingen, wenn dadurch DSP-Verhalten oder Stabilität unkontrolliert verändert wird.
20. Neben SuperSaw auch die angrenzenden gemeinsamen Engine-Pfade auf vermeidbare Kosten prüfen: Graph-Dispatch, Port-Summierung, CV-Konvertierung, Gain-/Ramp-Verarbeitung, Voice-Schleifen und Ausgabemischung. Änderungen erfolgen nur mit Vorher-/Nachher-Messung und Regressionstest; Klangverhalten, Preset-Kompatibilität und bestehende Nodes dürfen nicht verändert werden.

## Phase 5: Diagnose und Dokumentation

21. Einen FM-Engine-Counter für aktive SuperSaw-Blöcke ergänzen und den vorhandenen Statistikpfad um Blocklaufzeit, Spitzenlast und Underruns erweitern, damit Hardware-Benchmarks SuperSaw und den vollständigen Audiopfad eindeutig bewerten können. Kommentar und Dokumentation nennen sieben Teiloszillatoren, Detune-/Mix-Semantik, 7-Band-CV-Grenzen und die gemessene CPU-/Speicherlast.
22. Das vorhandene Preset `SuperSaw` ist das verbindliche Referenzpreset für Funktions-, Klang-, Last- und Stresstests. Es wird unverändert geladen und nicht durch den Testlauf überschrieben. Vor jeder Messreihe werden Presetname, geladene Presetdaten, Firmware-Version, Blockgröße und Stimmenzahl protokolliert, damit Vorher-/Nachher-Werte reproduzierbar bleiben. Falls das Preset nur im Dateisystem des Geräts und nicht im Repository liegt, wird die verwendete Version vor dem Test über die Preset-API exportiert und zusammen mit den Messergebnissen archiviert. Zusätzliche synthetische Grenzfall-Presets dürfen ergänzend verwendet werden, ersetzen `SuperSaw` aber nicht als Haupttest.

## Relevante Dateien

- `main/audio/engine/NodeTypes.h`: `WaveType`, String-Parsing und FM-spezifische Zulassung.
- `main/audio/engine/PresetParser.h`: neue FM-Parameter in `WidgetParams`.
- `main/audio/engine/NodeState.h`: Detune/Mix und sechs Seitenphasen in `FmOscillatorState`.
- `main/audio/engine/DspPrimitives.h`: günstige Saw-Auswertung aus Integerphase.
- `main/audio/engine/nodes/FmOscillatorNode.h`: 7-Band-Mapping, SuperSaw-DSP, Fast-/Slow-Path, Gate/Build/Param/Preset.
- `main/audio/engine/SynthEngine.h`: Render-Task, Blocklaufzeit, Überlastschutz und Audioausgabe.
- `main/audio/engine/GraphBuilder.h`: Speicherplatzierung und Trennung von Graph-Aufbau und Echtzeitpfad.
- `main/audio/engine/Graph.h`: typabhängig sichere Waveform-Live-Updates.
- `main/audio/engine/PortTables.h`: Portindizes 9/10, Count und Control-only-Klassifikation.
- `main/audio/engine/EngineStats.h`: optionale SuperSaw-Aktivitätsmetrik.
- `main/audio/engine/SynthConfig.h`: Sample-Rate, Blockgröße und Ressourcenlimits.
- `data/static/js/main.js`: FM-spezifische Buttons, State, Slider, Preset-/Live-Handling und Portdarstellung.
- `data/static/js/audio-engine.js`: Web-Audio-SuperSaw und neue CV-Verbindungen.
- `documentation/fm-plan.md`: DSP-/CPU-Dokumentation und Messwerte nach Validierung.
- `tools/benchmark_synth.mjs`: bestehenden Hardware-Benchmark wiederverwenden, nicht zwingend ändern.

## Verifikation

1. JavaScript statisch prüfen: `node --check data/static/js/main.js` und `node --check data/static/js/audio-engine.js`.
2. Firmware gemäß Repository-Vorgabe in PowerShell bauen: `C:\esp\v6.1-rc1\esp-idf\export.ps1`, danach `idf.py build`.
3. Preset-Roundtrip testen: altes FM-Preset ohne neue Felder, neues Preset mit `waveType: "supersaw"`, Detune/Mix speichern, neu laden und per Live-Update verändern. Zusätzlich absichern, dass `supersaw` bei WaveGen/LFO nicht in einen ungültigen Renderpfad gelangt.
4. DSP-Grenzfälle auf Hardware hören/messen: Mix 0 V entspricht Center-Saw; Detune 0 V kollabiert phasengleich auf Saw; Mix 10 V verteilt gleich; Gate-Retrigger ist deterministisch; Wechsel zu/von SuperSaw erzeugt keine alte Ausgabe. Den `uint32_t`-Überlauf nahe `UINT32_MAX` gezielt testen: schrittweise Addition und Bulk-Fortschreibung müssen modulo $2^{32}$ dieselbe Endphase liefern, ohne separaten Wrap-Branch oder Modulo im Sample-Loop.
5. Waveform-CV an Grenzwerten prüfen: 0, 10/7, 20/7, 30/7, 40/7, 50/7, 60/7 und 10 V wählen die sieben erwarteten FM-Wellenformen. WaveGen behält sechs Bänder.
6. UI-Sichtbarkeit prüfen: Detune- und Mix-Zeilen sind bei allen sechs anderen Wellenformen nicht vorhanden bzw. ausgeblendet und erscheinen gemeinsam nur bei SuperSaw. Das gilt bei Button-Auswahl, Waveform-CV-Wechsel und Laden eines Presets. Angeschlossene Detune-/Mix-CV-Kabel lassen die Zeilen sichtbar, deaktivieren aber nur den jeweiligen Slider; nach dem Trennen wird der gespeicherte Sliderwert wieder bedienbar.
7. FM und Self-Feedback separat sowie gemeinsam testen; beide müssen auf die gesamte normalisierte SuperSaw wirken. Neue Detune-/Mix-CV-Kabel müssen Firmware und Browser-Vorschau aktualisieren und beim Trennen auf den Sliderwert zurückfallen.
8. Das vorhandene Preset `SuperSaw` auf dem Zielgerät laden und zuerst vollständig funktional prüfen: Wellenform, Detune, Mix, UI-Sichtbarkeit, CV-Steuerung, FM, Self-Feedback, Gate-Retrigger und Preset-Roundtrip. Die Tests verwenden die unveränderte gespeicherte Presetversion; fehlende Grenzfälle werden mit separaten temporären Presets geprüft.
9. Mit `tools/benchmark_synth.mjs` auf Zielhardware das Preset `SuperSaw` bei 1, 8 und 16 Stimmen messen und, soweit das Preset einen direkten Vergleich erlaubt, Sawtooth gegen SuperSaw vergleichen. CPU-Last, Blockzeit, Overruns, interne-RAM-Nutzung und automatische Voice-Reduktion dokumentieren. Jedes Szenario erhält eine Baseline vor und ein Ergebnis nach der Optimierung.
10. Mit dem Preset `SuperSaw` einen mindestens zehnminütigen Stresstest auf dem ESP32-P4 durchführen. Akzeptanz: null Audio-Underruns, keine Heap-Allokation oder blockierende Operation im Render-Task und mindestens 20 % gemessene Zeitreserve im vereinbarten realistischen Referenzzustand. Bei extremer 16-stimmiger Mehrfach-SuperSaw darf die vorhandene Schutzlogik kontrolliert die Stimmenzahl reduzieren.
11. Build-Map und Disassembly der heißesten Schleifen prüfen: SuperSaw-Zustand und kritische Blockpuffer liegen im vorgesehenen internen RAM, Hotpath-Funktionen im IRAM, und die inneren Schleifen enthalten keine unerwarteten Divisionen, Float-Modulo-, Potenz- oder Runtime-Helper-Aufrufe.
12. Das Preset `SuperSaw` manuell bei tiefen, mittleren und hohen Noten auf Pegelsprünge, Clipping und Aliasing prüfen. Aliasing wird in dieser Ausbaustufe nicht durch bandbegrenzte Saws gelöst, darf gegenüber sieben parallel geschalteten vorhandenen Sawtooth-Oszillatoren aber nicht zusätzlich verschlechtert sein.

## Entscheidungen

- Bestätigt: echte sieben Stimmen plus Detune/Mix, nicht die billigere 3-Stimmen-Variante.
- Alle sieben SuperSaw-Stimmen verwenden `uint32_t`-Phasenakkumulatoren; Float-Phasen und manuelle Wrap-around-Abfragen sind im Sample-Hotpath ausgeschlossen.
- Die Optimierung umfasst den vollständigen nativen Echtzeit-Audiopfad auf dem ESP32-P4, nicht nur den neuen SuperSaw-Code. Abschlusskriterium sind reproduzierbare Hardwaremessungen, nicht lediglich ein erfolgreicher Build.
- Keine Heap-Allokationen, Locks, Datei-/Netzwerkzugriffe oder Logging-Aufrufe im Render-Task; samplekritische Daten bleiben im internen RAM und Hotpath-Code nach Möglichkeit im IRAM.
- Das vorhandene Preset `SuperSaw` ist das verbindliche Hauptpreset für Abnahme und Performancevergleich; zusätzliche Testpresets decken nur Grenzfälle ab.
- Bestätigt: FM-Waveform-CV wird bewusst auf sieben gleich breite Bänder geändert; bestehende FM-Patches mit verkabeltem Waveform-CV können dadurch eine andere Wellenform wählen.
- SuperSaw bleibt exklusiv im FM-Operator; WaveGen und LFO erhalten weder Taste noch DSP-Pfad.
- Defaults für alte Presets: Detune 5 V, Mix 5 V. Neue Parameter werden unter `superSawDetune` und `superSawMix` gespeichert.
- Das Klangmodell ist JP-inspiriert, keine bitgenaue oder proprietäre JP-8000-Emulation.
- Enthalten: Firmware-DSP, Ports, Presets, Live-Updates, UI und Browser-Vorschau.
- Nicht enthalten: bandbegrenzte/antialiased Saw-Oszillatoren, Stereo-Spread, exakte JP-8000-Detune-Kennlinie oder Änderungen am normalen WaveGen/LFO.
- Der im Quellbaum nicht definierbare `WIDGET_ENGINES`-Verweis ist ein bestehender separater Befund; nur berühren, falls die SuperSaw-Integration ihn direkt benötigt.