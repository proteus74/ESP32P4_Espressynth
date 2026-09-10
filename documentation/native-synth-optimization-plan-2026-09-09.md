# Optimierungsplan für die native Synth-Engine

Stand: 9. September 2026. Untersucht wurde der aktuelle Arbeitsbaum auf Basis von `35e1305`, einschließlich der bereits vorhandenen, nicht eingecheckten Audio-Änderungen. Diese Änderungen wurden nicht verändert. JavaScript, JS-Audio-Engine und WebUI waren nicht Teil der Untersuchung.

Die Untersuchung umfasst die nativen Node-Typen, DSP-Primitiven, Graphaufbau und Rendering, MIDI-/Stimmenverwaltung, Sample-/SoundFont-Wiedergabe, Effektadapter sowie den Audio-Ausgabepfad. Es wurden keine Firmwareänderungen implementiert und keine neuen Messungen auf dem ESP32-P4 ausgeführt. Die Prioritäten beruhen auf Quellcode, Screenshot, vorhandenem Benchmarkbericht und mathematischer Prüfung der LUT-Näherungen. CPU-Einsparungen sind erst nach Hardwaremessung belastbar.

## 1. Ergebnis und Ausgangslage

Die größten aussichtsreichen Einsparungen liegen in der wiederholten Arbeit pro Sample und Stimme sowie im Transport konstanter Steuersignale. Die LUT-Größe ist ein zusätzlicher RAM-Hebel, aber nicht automatisch ein CPU-Hebel.

Aktuelle Konfiguration: 44.100 Hz, 128 Samples pro Block, 360 MHz, maximal 32 Stimmen, Compileroptimierung für Performance, 128 KiB L2-Cache. Das ergibt 2.902,49 µs Zeitbudget pro Block. Quellen: [SynthConfig.h](../main/audio/engine/SynthConfig.h), [sdkconfig](../sdkconfig). Vorhandene Build-Metadaten nennen ESP-IDF v6.1-rc1; sie belegen nicht den Stand der laufenden Firmware.

Im Screenshot kosten die vier WaveGens zusammen **46,9 Prozentpunkte des Blockbudgets**, rechnerisch etwa **1.361 µs/Block**. Der Filter zeigt 8,2 %, Gain 2,9 %, die drei ADSRs zusammen 4,1 %. Diese Anzeigen liefern eine sinnvolle Priorisierung, aber keinen kontrollierten Vergleich der Algorithmen.

Der vorhandene [Benchmark vom 8. September](audio-benchmark-2026-09-08.md) zeigt ebenfalls hohe Oszillatorkosten. Er betrifft andere Presets und ist keine Messung dieses Screenshots oder sämtlicher aktueller Änderungen.

Bereits vorhanden und deshalb nicht als neue Optimierung anzurechnen:

- Sinus-LUT mit 4.096 statt früher 16.384 Einträgen; interpolierte Pow2-/VCA-LUTs, neue tanh-LUT.
- Blockweiser Frequenzpfad im WaveGen; im aktuellen Arbeitsbaum zusätzlich Erkennung konstanter Audio-Pitch-Puffer.
- Frequency Divider mit gecachten Oktavabständen statt teurer Logarithmusberechnung.
- Sparse Input-Puffer: Viele unverbundene und reine ControlRoute-Ports verwenden bereits Skalare. MIDI-Pitch bekommt weiterhin einen Puffer für Glide.
- Ruhe-Erkennung über ADSRs, einmaliges Löschen stillgelegter Outputs, rotierendes Node-Profiling.
- Filter-Koeffizienten nur bei Änderung bzw. mit konfigurierbarem Stride; Sinus-LFO interpoliert bereits zwischen Blockgrenzen.
- Hammond mit Integerphase, aktiver Drawbar-Liste und ausgesetztem Smoothing bei angenähert konstanten Werten.
- WAV-/SF2-Dateicaches; Laden im Hintergrundtask. Normale Graphwechsel geben alte Graphen an den Loader zur Freigabe weiter.

## 1a. Umsetzungsstand P0 und P1/WaveGen (ergänzt 9. September 2026)

P0 und der WaveGen-Teil von P1 (siehe Abschnitt 3, Unterabschnitt "P1: WaveGen-Schleife vereinfachen") sind im Quellcode umgesetzt, noch nicht auf dem P4 gemessen und noch nicht committed. Geändert bzw. neu: [EngineStats.h](../main/audio/engine/EngineStats.h) (neu), [Graph.h](../main/audio/engine/Graph.h), [OscillatorNode.h](../main/audio/engine/nodes/OscillatorNode.h), [DspPrimitives.h](../main/audio/engine/DspPrimitives.h), [SynthEngine.h](../main/audio/engine/SynthEngine.h). `idf.py fullclean && idf.py build` erfolgreich, keine neuen Compiler-Warnungen, Firmware-Binärgröße 0x161ac0 Byte (54 % der App-Partition frei). Die folgenden Punkte sind ausschließlich Quellcode-Umsetzung - CPU-Einsparungen sind weiterhin erst nach Hardwaremessung belastbar (siehe Einleitung).

**P0, Baseline-Instrumentierung:**

- `Graph::renderBlock()` misst jetzt drei Blockphasen statt einer einzigen: Aktivitätsermittlung/Puffer-Init, gesamter Renderdurchlauf (Node-DSP + Edge-Summing über alle Widgets), Output-Kopie - EMA-geglättet wie das bestehende `widgetLoadPercent`, mit konstant wenigen zusätzlichen Timer-Reads pro Block (kein O(Widgetzahl)-Zuwachs).
- Der bestehende rotierende Pro-Widget-Profiler misst zusätzlich das Edge-Summing des jeweils profilierten Widgets separat (`widgetEdgeLoadPercent`) - schließt die in Abschnitt 7 benannte Lücke ("Eingangspuffer vorbereiten und ausgehende Verbindungen summieren liegen außerhalb"), ohne die bewusste Rotation aufzugeben.
- Instanzzähler pro Block: gesamt / tatsächlich gerendert / trotz Ruhe gerendert (renderWhenIdle- bzw. LFO-Global-Fälle) - in `EngineStats.h`.
- Fast-Path-Zähler für den WaveGen: Pitch konstant/audio-rate, Tune konstant/audio-rate, Silent-Skip, Gain-Converged.
- Alle Werte per Atomics veröffentlicht und zusätzlich alle 2 s nativ als `ESP_LOGI` geloggt (kein Browser/keine JS-Engine nötig, siehe Abschnitt 8) - in [SynthEngine.h](../main/audio/engine/SynthEngine.h). Bewusst nicht ans WebUI/`synth_web_api.hpp` angebunden, da JS/WebUI laut Einleitung nicht Teil der Untersuchung waren.
- **Nicht umgesetzt:** ein fester, wiederholbarer nativer MIDI-Ereignisablauf/Benchmarkmodus (Abschnitt 8). Die Instrumentierung liefert die Rohdaten dafür, der eigentliche Testharness fehlt noch.
- **Hinweis, nicht Teil dieser Umsetzung:** Beim Lesen von `SynthEngine.h` zeigte sich, dass Mittelwert/P95/P99/Maximum sowie Deadline-Überschreitungen (`TimingSnapshot`, `consumeTimingSnapshot()`, `renderOverrunCount()`) und die separate MIDI-/Liveparam-Phasenmessung in `taskLoop()` bereits vor dieser Umsetzung vorhanden waren. Die Annahme in Abschnitt 7, dies fehle noch vollständig, war insofern ungenau - nur die Aufschlüsselung *innerhalb* von `Graph::renderBlock()` fehlte tatsächlich.

**P1, WaveGen-Schleife (Abschnitt 3, Unterabschnitt "P1: WaveGen-Schleife vereinfachen", Punkte 1-7):**

1. Wellenform/Konstanz einmal pro Block wählen: für Pitch war das bereits vorhanden, jetzt konsistent auch für Tune (siehe Punkt 6) genutzt.
2. `phaseIncrement = hz/sampleRate` einmal pro Block vorberechnen: umgesetzt über neues `PhaseOsc::processWithIncrement()` - ersetzt die Multiplikation pro Sample in `advance()` durch eine einzige Division pro Block plus reine Additionen in der Schleife.
3. Separate innere Schleifen je Wellenform: **nicht umgesetzt.** Der Plan verlangt hierfür selbst vorab eine Disassembly-Prüfung und Messung auf dem P4 ("Vor Übernahme die tatsächlich erzeugten Schleifen im Disassembly prüfen und auf dem P4 messen") sowie Vorsicht vor Instruktionscache-Verschlechterung durch zu viele Varianten - beides von hier aus nicht zu leisten.
4. Gain-/Amplituden-/Portzeiger vor der Schleife auflösen: umgesetzt - `ampBuf`/`gainBuf` werden einmal pro Block aufgelöst statt über `portSample()`s Null-Check pro Sample.
5. Smoothing nach Konvergenz stoppen, Zustand explizit auf Ziel setzen: umgesetzt - neues `OnePoleLowpass::snapTo()`, der Gain-Smoother wird bei erkannter Konvergenz einmalig exakt auf das Ziel gesetzt, die Schleife überspringt danach `process()` und verwendet den konstanten Zielwert.
6. Pitch-Konstanzpfad kurzfristig auf Tune erweitern: umgesetzt - Tune durchläuft jetzt dieselbe `audioPortConstantForBlock()`-Prüfung wie Pitch. Schließt die in Abschnitt 2 benannte Lücke ("Für Tune gibt es diese Erkennung noch nicht").
7. Integer-Phasenakkumulator als separate Variante messen: **nicht umgesetzt**, aus demselben Grund wie Punkt 3 (Disassembly/Hardwaremessung vorausgesetzt).

Die im Anschluss beschriebene Alternative (Floatphase mit Subtraktion von 1 statt `floorf()`) wurde bewusst **nicht** umgesetzt - der Plan weist selbst darauf hin, dass der kombinierte Pitch-/Tune-Bereich aktuell nicht zuverlässig unter Nyquist begrenzt ist und ein pauschaler Ersatz von `floorf()` deshalb falsch wäre.

Nächster Schritt gemäß Abschnitt 8: auf Hardware flashen, den neuen `ESP_LOGI`-Log ("P0 baseline: ...") mit dem Screenshot-Preset laufen lassen und damit den in Abschnitt 2 skizzierten kontrollierten Nachweis (acht Stimmen, MIDI direkt vs. Divider /1/2/4) tatsächlich mit Zahlen unterlegen.

**P1, zweite Tabellenzeile (Chorus/WavePlayer/Mapping-Fade), nachträglich ergänzt:**

- Chorus: ein gemeinsamer Ringpuffer statt zweier identisch gespeister Delay-Lines (beide Kanäle erhielten schon vorher dasselbe Mono-Sample) - ein Schreibstrom, zwei interpolierte Lesetaps, ~17,6 KiB/Instanz gespart. LFO-Schritt sowie Offset/Depth in Samples jetzt bei Parameteränderung vorberechnet statt pro Sample neu berechnet. [ChorusNode.h](../main/audio/engine/nodes/ChorusNode.h).
- WavePlayer: `playing=false`, sobald die 20-ms-Release-Fade vollständig abgeklungen ist (statt bis zum Dateiende weiterzulesen), Restblock danach zusammenhängend genullt. [WavePlayerNode.h](../main/audio/engine/nodes/WavePlayerNode.h).
- ValueMapper: `1/(point2In-point1In)` bei Parameteränderung gecacht statt einer Division pro Sample im Audio-Pfad. [ValueMapperNode.h](../main/audio/engine/nodes/ValueMapperNode.h).
- Multiplexer (aus Abschnitt 5s Detailtabelle mit übernommen, da eng verwandt): Kehrwert der Fade-Länge gecacht statt Division pro Sample; nach Fade-Ende (auch mitten im Block) direkter Buffer-Copy statt `portSample()`-Schleife. [MultiplexerNode.h](../main/audio/engine/nodes/MultiplexerNode.h).

**Abschnitt 6, Korrektheitsbefunde - Status siehe dort inline:** Punkte 1 (Glide-Konstanzlücke) und 3 (ESP-Effekt-Pegel) sind eindeutige Bugs und wurden behoben. Punkt 2 (Curtis LP24) ändert hörbar den Klang - auf Nutzerentscheidung hin ebenfalls gefixt (Frequenzgang/Resonanzcharakter von LP24 unterscheidet sich danach von vorher). Punkt 4 (Stimmenruhe-Domäne) bewusst zurückgestellt, siehe dortige Begründung.

Build nach jedem Schritt dieser Runde geprüft (`idf.py build`), zuletzt grün nach dem Curtis-LP24-Fix. Nichts davon auf Hardware gehört oder gemessen.

## 1b. Umsetzungsstand P2-P5 (ergänzt 9. September 2026, dritte Runde)

Wie in 1a/hier: Quellcode-Umsetzung, kein Hardwaretest. Nach jedem Schritt `idf.py build` grün. Bei P2-P5 wurde bewusst nur umgesetzt, was ohne die im Plan selbst verlangten Regressionstests bzw. Hardwaremessung als sicher gelten kann; alles andere ist unten explizit als zurückgestellt vermerkt, mit Begründung statt stillschweigendem Auslassen.

**P2 (Konstante Signale und Pufferarbeit):**

- Umgesetzt: `AudioInputLink`/`AudioOutputLink` überspringen den Gain-Durchlauf für einen Bus, der nachweislich still ist (keine Kante, kein Ungleich-Null-Default) - render-seitiger Teil von "Nur verwendete Link-Busse ... transportieren". [LinkNodes.h](../main/audio/engine/nodes/LinkNodes.h).
- Geprüft und als unsicher verworfen: "MIDI-Pitch ohne laufenden Glide benötigt keinen pro Block gefüllten Sample-Puffer" (Graph::renderBlock's ControlRoute-Füllschleife überspringen). Audit von `inPorts[port][i]`-Rohzugriffen zeigt: [EspAdditionalEffects.h](../main/audio/engine/EspAdditionalEffects.h) liest `in[port][i]` direkt, nur durch einen Nullptr-Check abgesichert, nicht durch einen Connected-Check wie `portSample()`. Ein übersprungener Fill würde dort stille, veraltete Pufferinhalte statt aktueller Werte liefern. Ohne vollständige Prüfung aller ESP-Effekt-Adapter (nicht abgeschlossen) nicht sicher umsetzbar.
- **Konstant-Fastpath verallgemeinert und ausgeweitet (vierte Runde, nach Nutzerentscheidung "mach mit P2 weiter"):** Die frühere Einschätzung "kein Nettogewinn ohne die größere Architektur" war zu pessimistisch - ein einfacherer Weg gefunden, der weder Additionsreihenfolge ändert noch neue Infrastruktur braucht. `audioPortConstantForBlock()` (bisher Oscillator-lokal für Pitch/Tune) liegt jetzt zentral in [NodeInstance.h](../main/audio/engine/NodeInstance.h)s `noderender`-Namensraum, zusammen mit neuem `allPortsConstantForBlock()`. Kernidee: für einen nicht audio-verbundenen Port ist "konstant" bereits durch `audioConnectedPortMask` bekannt (O(1), kein Scan nötig - Graph::renderBlock füllt solche Ports ohnehin mit einem Wert fürs ganze Blocks); nur echte audio-rate-Ports brauchen einen Scan. Wenn alle Ports eines Nodes konstant sind, wird dieselbe Formel in derselben Port-Reihenfolge **einmal statt pro Sample** ausgewertet - bit-identisches Ergebnis, keine Umsortierung. Das schließt nebenbei auch automatisch die Glide-Lücke aus Abschnitt 6/Befund 1 (ein Scan über den tatsächlichen Pufferinhalt erkennt eine laufende Rampe korrekt als "nicht konstant", unabhängig von `audioConnectedPortMask`) - der frühere manuelle `controlRampPortMask`-Zusatz in Adder/Multiplier/Divide/Abs/FrequencyDivider/Detune/ValueMapper ist dadurch überflüssig geworden und wurde durch den saubereren, allgemeineren Check ersetzt. Angewendet auf: [ArithmeticNodes.h](../main/audio/engine/nodes/ArithmeticNodes.h) (Adder/Multiplier/Divide/Abs/FrequencyDivider), [DetuneNode.h](../main/audio/engine/nodes/DetuneNode.h), [ValueMapperNode.h](../main/audio/engine/nodes/ValueMapperNode.h). Zusätzlich neu: Oscillators "amplitude"-Port (typischerweise eine ADSR-Hüllkurve) bekommt dieselbe Behandlung - im Sustain (oder unverbunden) liefert die Hüllkurve denselben Wert für den ganzen Block, wodurch der bisher pro Sample laufende `vcaGain()`-Aufruf entfällt ([OscillatorNode.h](../main/audio/engine/nodes/OscillatorNode.h), neue `oscAmpConstant`/`oscAmpAudioRate`-Zähler in [EngineStats.h](../main/audio/engine/EngineStats.h)). **Hardware-Ergebnis (fünfter Log, gleicher Patch, 16 Stimmen):** `widget-3` dsp ≈14,7 % (von ≈16,6 % zuvor) - nochmal ein realer, auf Hardware bestätigter Rückgang. Die neuen Amp-Zähler fehlten im ersten Log-Durchlauf (Struct in `EngineStats.h` angelegt, aber nie bis zu `SynthEngine.h`s Atomics/`synth_manager.hpp`s Log-Ausgabe durchgezogen) - nachgezogen, Build grün.
- **Pufferansicht statt Kopie ✅ umgesetzt (fünfte Runde, nach Nutzerentscheidung "mach mit dem großen Rest weiter"):** Das ist die riskanteste Änderung dieser gesamten Session - Speicher-Ownership, nicht nur Performance. Bewusst als eigener, additiver Pass NACH dem bestehenden, funktionierenden Build-Ablauf umgesetzt (nicht in die bestehende Allokations-/Topologie-Logik eingewoben), damit nichts Bestehendes umgebaut werden musste - im schlimmsten Fall wird ein gerade erst allozierter Puffer sofort wieder freigegeben und der Zeiger umgehängt, ein einmaliger Build-Zeit-Kosten, kein Laufzeit-Risiko.

    Bedingungen, unter denen ein Zielport jetzt direkt auf den Ausgabepuffer seiner Quelle zeigt statt eine eigene Akkumulator-Kopie zu führen (alle müssen gelten, sonst bleibt der Port unangetastet): genau eine eingehende Kante (keine Mehrfachquelle), `scale == 1.0` (kein globales Fan-in mit 1/√N-Skalierung), keine Monitor-Tap-Kante (die live umschaltbare Stimme darf nicht zur Build-Zeit eingefroren werden), und - der Kern der Sicherheitsprüfung - sowohl Quell- als auch Ziel-Widget liegen im sauber topologisch sortierten Präfix von `renderOrder` (nicht im am Ende angehängten Zyklus-Fallback), mit der Quelle nachweislich vor dem Ziel. Neues `NodeInstance::viewPortMask` markiert solche Ports; `Graph::renderBlock()` überspringt für sie sowohl das Nullen (Aktivierung: der Puffer *ist* die frische Ausgabe der Quelle, kein eigener Akkumulator) als auch das Aufsummieren der zugehörigen Kante (sonst würde `dstBuf[i] += srcBuf[i]` denselben Speicher zu sich selbst addieren); `Graph::destroy()` gibt einen View-Port nicht frei (das Gedächtnis gehört der Quellinstanz). [NodeInstance.h](../main/audio/engine/NodeInstance.h), [Graph.h](../main/audio/engine/Graph.h), [GraphBuilder.h](../main/audio/engine/GraphBuilder.h).

    Audit vor der Umsetzung: kein Node-`render()` schreibt jemals in sein eigenes `inPorts[]` (nur `Graph::renderBlock`s eigene Füll-/Glide-Logik tut das, ausschließlich für nicht-audio-verbundene bzw. genau die davon ausgeschlossenen Ports) - Aliasing kann also nicht durch eine In-Place-Mutation eines Nodes korrumpiert werden. Multiplexer/Switch lesen nur (`portSample`/direkter Kopier-Pfad aus P1), unempfindlich dagegen ob ihr Eingang eine echte Kopie oder eine Ansicht ist. Neue Build-Diagnosezeile `buffer views %u` zählt, wie viele Ports tatsächlich aliasiert wurden - sichtbar im Log für jeden geladenen Preset.

    **Hardware-Klangprüfung (sechste Runde):** Auf einem deutlich komplexeren Preset getestet (117 Instanzen, 40 Stimmen - zuvor bei nur 10 Stimmen bereits an der Kapazitätsgrenze, also viel mehr Kanten/Verzweigungen als der einfache 16-Stimmen-Testpatch). Ergebnis laut Nutzer beim gezielten Hören: "klingt genauso wie vorher, keine Aussetzer, keine hängenden Töne, keine falschen Töne, Sound ist richtig." Das ist eine echte, wenn auch nicht erschöpfende Bestätigung der Pufferansicht-Korrektheit auf Hardware. Separat aufgefallen (Kapazitätsfrage, nicht Korrektheit): dieses 40-Stimmen-Preset überschreitet weiterhin regelmäßig das Render-Zeitbudget (bis 3718 µs bei 2902 µs) - die anfänglich beobachteten Aussetzer waren teils durch `CONFIG_AUDIO_I2S_DMA_FRAME_NUM` (Nutzer-Änderung 240→256, mehr DMA-Ring-Headroom laut README) abgefedert, nicht durch die eigentliche CPU-Überschreitung behoben; bei noch mehr Stimmen/Effekten ist erneutes hörbares Klicken plausibel. `sdkconfig` ist gitignored, die 256 bleibt vorerst lokal beim Nutzer.
    
    **Weiterhin nicht durch gezieltes Hören/Testen abgedeckt:** eine echte Feedback-Schleife (der Builder erlaubt sie laut eigenem Kommentar als "defensive fallback"), Live-Umschalten eines Multiplexer/Switch während des Betriebs, und ein Preset mit aktivem Monitor-Tap-Wechsel zwischen Stimmen.
- **Teilgraph-Pruning ✅ umgesetzt (siebte Runde):** Erreichbarkeitsanalyse rückwärts von den drei Hardware-Sinks und jeder aktiven Monitor-Tap-Kante (beide Enden - Quelle UND Ziel, da die Web-/Display-seitige Nutzung eines Meter-/Scope-Widgets außerhalb dieser Untersuchung lag und ich lieber zu vorsichtig als zu aggressiv pruune) über die Widget-Kanten, per Fixpunkt-Worklist (robust gegenüber Zyklen - ein Widget in einem Zyklus wird zusammen mit dem Rest des Zyklus als gebraucht markiert, sobald irgendeins davon erreicht wird). Neues `WidgetSlot::needed` (Default `true`, sicherer Fallback); `Graph::renderBlock()` überspringt ein nicht gebrauchtes Widget komplett (Render UND seine eigenen ausgehenden Kanten) - sicher per Konstruktion, da ein Widget nur dann als "nicht gebraucht" markiert wird, wenn keine seiner Kanten irgendwo Gebrauchtes erreicht. [Graph.h](../main/audio/engine/Graph.h), [GraphBuilder.h](../main/audio/engine/GraphBuilder.h). Neue Build-Log-Zeile `%u of %u widgets pruned`.

    Geprüft, dass dies NICHT mit der "Multiplexer-Zweige nicht einfrieren"-Warnung aus dem Plan kollidiert: ein aktuell nicht ausgewählter Multiplexer-Eingang hat trotzdem eine echte Kante zum (gebrauchten) Multiplexer-Widget in dieser statischen Topologie - bleibt also unabhängig vom Laufzeit-Auswahlzustand als gebraucht markiert. Pruning betrifft ausschließlich Widgets ohne JEDEN Pfad zu einem Ausgang/Monitor, nicht "gerade inaktive" Zweige. MidiIn/MidiSplit/Knob/Button-Auswertung läuft über `Graph::handleMidiEvent`/ControlRoutes, nicht über `render()`/Kanten - von diesem Pruning unberührt.

    **Noch nicht auf Hardware getestet.** Braucht einen Preset mit tatsächlich ungenutzten Zweigen (z. B. ein Effekt-Widget ohne Verbindung zum Ausgang) um überhaupt etwas zu bewirken - dein bisheriges Testpreset hat vermutlich keine oder kaum prunbare Widgets.
- **Ungenutzte Link-Busse gar nicht erst anlegen ✅ umgesetzt (achte Runde):** `AudioOutputLink`s eigener `audioConnectedPortMask` ist zum Zeitpunkt der Link-Kopplung im Builder bereits durch die allgemeine Verbindungsverarbeitung (deutlich früher im selben `build()`) korrekt gefüllt - die Bus-Kopplungsschleife prüft das jetzt vor jedem `addEdge()` und lässt einen nicht gespeisten Bus komplett aus (keine Kante, kein `audioConnectedPortMask`-Bit auf `AudioInputLink`, keine Pufferallokation - `requiresInputBlockBuffers()` deckt Link-Typen ohnehin nicht ab). `markDependency()` bleibt bewusst unbedingt bestehen, um die bestehende Render-Reihenfolgen-Garantie zwischen beiden Link-Widgets nicht anzutasten. Komponiert sauber mit dem bereits vorhandenen render-seitigen Silent-Skip ([LinkNodes.h](../main/audio/engine/nodes/LinkNodes.h), erste P2-Runde) und mit Teilgraph-Pruning oben. [GraphBuilder.h](../main/audio/engine/GraphBuilder.h). Build grün, Hardwaretest offen (braucht ein Preset, das nicht alle 8 Busse benutzt).
- **Weiterhin nicht umgesetzt:** ein eigener `Ramp`-Signaltyp (Plan selbst: "später").

**Damit ist P2 (beide Tabellenzeilen aus Abschnitt 3) vollständig umgesetzt**, mit Ausnahme des vom Plan selbst auf "später" vertagten `Ramp`-Typs.

**P3 (Filter/LUT):**

- Umgesetzt: Filter/CurtisFilter erkennen jetzt, wenn die audio-rate Cutoff-/Resonance-CV sich seit dem letzten Stride-Tick nicht geändert hat (z. B. ADSR auf Sustain-Plateau, pausierte Modulation) und überspringen die Koeffizientenberechnung dann komplett, statt bei jedem Stride-Tick bedingungslos neu zu rechnen. [FilterNode.h](../main/audio/engine/nodes/FilterNode.h), [CurtisFilterNode.h](../main/audio/engine/nodes/CurtisFilterNode.h).
- Umgesetzt: Pow2-/VCA-LUT auf je 257 Einträge reduziert (6.136 Byte weniger internes RAM), wie im Plan als erster sinnvoller Versuch vorgeschlagen. [SynthConfig.h](../main/audio/engine/SynthConfig.h). Numerisch klar unter hörbar (Abschnitt 4), aber wie dort gefordert noch kein Hardware-A/B.
- Sinus-LUT bewusst unverändert (Plan: erst nach getrennter Filter-/Hammondprüfung).

**P4 (SoundFont/Effekte):**

- Umgesetzt: SoundFontPlayer nutzt jetzt TinySoundFonts `TSF_STEREO_UNWEAVED`-Ausgabemodus (planar: L- und R-Kanal als zwei zusammenhängende Hälften eines Puffers) statt `TSF_STEREO_INTERLEAVED` - genau die im Plan vorgeschlagene "native getrennte Ausgabe". Interleaved-zu-Planar-Kopie entfällt dadurch vollständig; der verbleibende Kopier-Durchlauf trägt die Gain-Multiplikation direkt mit, statt eines zweiten separaten Durchlaufs. [SoundFontPlayer.h](../main/audio/engine/SoundFontPlayer.h), [NodeRender.h](../main/audio/engine/NodeRender.h).
- Geprüft und zurückgestellt: "Voice-/Filterzustände bevorzugt intern [RAM]". TinySoundFonts Allokations-Hook ([tiny_soundfont.cpp](../components/tiny_soundfont/tiny_soundfont.cpp)) routet unterschiedslos ausnahmslos alles (Samplebank UND Voice-/Kanalzustand) über einen PSRAM-zuerst-Allocator - eine Trennung nach Zweck bräuchte entweder separate Allokations-Tags in der als "upstream unverändert" gepflegten `tsf.h` (Policy-Bruch, viele Stellen) oder ein fragiles externes Pointer-Umhängen, das bei jedem internen `TSF_REALLOC` (z. B. `tsf_set_max_voices`) wieder auf PSRAM zurückfallen würde. Keine sichere kleine Lösung gefunden.
- **Nicht umgesetzt:** EspEq/EspAlc/EspChannelConvert/EspMixer/EspSonic/EspDrc/EspMbc-Vergleiche gegen native Float-Kernels - der Plan verlangt hierfür explizit Hardwaremessung ("Library-Kernel separat von Formatkonvertierung messen"), keine Quellcode-Änderung ohne das.
- **Nicht umgesetzt** (Abschnitt 7, "Ereignisse und Ausgabe"): typisierte Parameterbefehle vor der Audiogrenze statt cJSON im Audiotask (größere Änderung am LiveParamMsg-Protokoll/Websocket-Grenze), Pre-free-/Queue-Fehlerpfad über den Loader statt direkt im Renderloop (Eingriff in die sorgfältig sequenzierte Fade/Reload-Statemachine, Risiko ohne Hardware-Timing-Test), Signal-Sprungmessung bedarfsgesteuert + Logging außerhalb des Audiotasks (Kosten/Nutzen fraglich - schon jetzt geraten auf einmal pro 100 ms, Aufwand für einen neuen Debug-Flag-Mechanismus unverhältnismäßig zur eingesparten Handvoll Integer-Operationen pro Sample), Fade-Puffer im Idle-Zustand vermeiden (würde drei bereits dichte, sorgfältig austarierte PCM-Schleifen inkl. Mute-/Klick-Diagnose verdoppeln, für einen sehr kleinen Gewinn). `SoftLimiter`s `powf()` bewusst nicht angefasst - der Plan selbst verbietet eine Limiter-Änderung allein zur Benchmark-Verbesserung.

**P5 (IRAM/Compiler/Dual-Core):** **bewusst nicht begonnen.** Der Plan gated P5 selbst explizit auf "nur bei verbleibenden gemessenen Engpässen" - ohne jede Hardwaremessung aus dieser Sitzung wäre jeder Schritt hier (IRAM-Platzierung, Compiler-/ESP-DSP-Experimente, Kernaufteilung) reine Spekulation gegen die eigene Planvorgabe, zusätzlich mit echtem Risiko (IRAM-Überlauf, Linkfehler, Timing-Regressionen), das ohne Hardware nicht zu erkennen wäre.

**Fazit dieser Runde:** Die verbleibenden offenen P2/P4/P7-Punkte sind fast durchgehend nicht "vergessen", sondern an einer der drei vom Plan selbst gesetzten Schranken hängengeblieben: (a) ändert hörbares Verhalten ohne Entscheidungsgrundlage, (b) braucht Regressionstests über Topologien, die nur auf echter Hardware/mit Gehör möglich sind, oder (c) ist explizit auf gemessene Engpässe gegated. Nächster sinnvoller Schritt bleibt Abschnitt 8: auf Hardware flashen, damit diese Schranken tatsächlich fallen können.

## 1c. Erste Hardwaremessung (9. September 2026) und ein selbst eingebauter Bug

Erster echter Log von der Hardware (16 Stimmen, ein WaveGen, ein Preset ähnlich dem Screenshot-Patch):

```
P0 baseline: phases activity=1.79% render=34.36% output=0.25% | osc pitch const/audioRate=16/0 tune const/audioRate=0/0 silentSkip=0 gainConverged=16 | instances total/rendered/idleForced=36/36/0
Audio-Overrun: 3162 us > 2902 us Budget (preset=0 us, midi=0 Ev/3 us, liveParams=0/2 us, render=1047 us, postRender=2083 us, esConvert=23 us, wmConvert=0 us)
```

**Kritischer Bug in der eigenen P0-Instrumentierung gefunden und behoben:** Jeder der fünf mitgeschickten Log-Ausschnitte zeigt exakt dasselbe Muster - eine P0-Zeile, sofort gefolgt von einem Audio-Overrun, beide im ~2-Sekunden-Takt meines eigenen `ESP_LOGI`-Aufrufs. `postRender` (2083 µs) dominiert den Overrun bei weitem gegenüber `render` (1047 µs) - für ein 16-Stimmen/1-WaveGen-Preset ist das für den Bereich zwischen `Graph::renderBlock()` und dem PCM-Schreiben (u. a. mein eigener periodischer Log-Aufruf) unplausibel hoch. Ursache: der `ESP_LOGI`-Aufruf lief bisher direkt im Render-Task (`SynthEngine::taskLoop()`) - ein blockierender UART-Write dort kann genau der von Abschnitt 7 selbst beschriebene Fehlerfall sein ("Logging außerhalb des Audiotasks ausführen"), den ich für eine andere Diagnose (Signal-Sprungmessung) in Abschnitt 1b als nicht lohnend zurückgestellt hatte - für die eigene P0-Instrumentierung galt dieselbe Regel, nur habe ich sie beim Schreiben übersehen.

**Fix:** Der Render-Task publiziert weiterhin nur Atomics (unverändert, kein reales Risiko). Das eigentliche `ESP_LOGI` läuft jetzt in `Feature::Synth::log_p0_baseline_tick()` ([synth_manager.hpp](../main/features/synth/synth_manager.hpp)), aufgerufen alle 2 s aus `app_main`s eigenem Task ([main.cpp](../main/main.cpp)) - vollständig außerhalb des Echtzeitpfads. Gleichzeitig um eine Pro-Widget-Aufschlüsselung (dsp%/edge% je Widget, dieselben Daten wie die Browser-Anzeige) ergänzt, damit die nächste Messung direkt zeigt, welches Widget die unten offene Frage beantwortet.

**Offene Beobachtung, noch nicht erklärt:** `render=34,36 %` (~1047 µs von 2902 µs Budget) trotz `osc pitch const/audioRate=16/0` - alle 16 WaveGens liefen im billigen Konstant-Pitch-Pfad, `gainConverged=16` (Smoothing ebenfalls übersprungen). `RampScheduler::process()` (ADSR, falls die übrigen ~20 Instanzen davon einen Teil sind) ist im Sustain bereits O(1) pro Sample, keine erwartbare Ursache. Ohne die jetzt ergänzte Pro-Widget-Aufschlüsselung lässt sich nicht sagen, ob das an vielen leichten Instanzen mit Grundkosten pro Node (Argument für P2s Grundidee, nur eben mit Hardwaredaten statt Vermutung), an einem einzelnen teureren Widget in diesem Preset, oder an Graph::renderBlock's eigenem Overhead (Puffer-Init, Edge-Summing) liegt. Nächster Schritt: denselben Test mit dem jetzt erweiterten Log wiederholen.

**Zweiter Log, mit Pro-Widget-Aufschlüsselung:** Kein Audio-Overrun mehr - der Logging-Fix wirkt. `widget-3` (16 Instanzen, passt zur WaveGen-Anzahl) dominiert klar: dsp≈21-22 %, edge≈4 % - zusammen ~25 der ~34-36 % Render-Last, trotz aller aktiven Fastpaths (Konstant-Pitch, konvergiertes Gain). Die übrigen fünf Widgets liegen zusammen bei unter 6 %.

**Disassembly bestätigt eine konkrete, messbare Ursache - kein Ratespiel mehr:** `nodes::oscillator::render()` ist mit `riscv32-esp-elf-objdump` sauber als ein einziger, vollständig inline-optimierter Block sichtbar (keine übersehenen Funktionsaufrufe) - aber die Funktion lag in `.flash.text`, nicht in IRAM. Ein `grep -r IRAM_ATTR main/` zeigt: **nirgends in der gesamten Audio-Engine wird IRAM_ATTR verwendet** - der komplette Hot Path läuft über den Flash-Cache. Genau das hatte `Graph::allocate()`s eigener Kommentar schon als Verdacht notiert ("shared L2-cache pressure from the display's own PSRAM-backed framebuffer traffic"), nur nie an dieser Stelle verfolgt.

Damit ist die vom Plan selbst gesetzte Schranke für P5 ("nur bei verbleibenden gemessenen Engpässen") jetzt tatsächlich erfüllt - echte Hardwaremessung plus Disassembly, nicht Spekulation. **Umgesetzt:** `IRAM_ATTR` auf `Graph::renderBlock()`, `processNodeBlock()`, `nodes::oscillator::render()` sowie die vier LUT-Lookup-Funktionen und `PhaseOsc::process()`/`processWithIncrement()`/`currentSample()`/`advance()` in [DspPrimitives.h](../main/audio/engine/DspPrimitives.h). `idf.py size` vor/nachher: DIRAM-Nutzung 212.578 → 223.068 Byte (37,1 % → 39,0 %), 349.300 Byte weiterhin frei - reichlich Reserve, kein Überlauf. Per Objdump verifiziert: alle drei benannten Funktionen liegen jetzt tatsächlich in `.iram0.text` statt `.flash.text`.

**Noch nicht verifiziert:** ob das den Render-Anteil auf der Hardware wirklich senkt. Das ist jetzt ein sauberer A/B-Test mit genau demselben P0-Log - gleicher Patch (16 Stimmen, ein WaveGen) nochmal flashen und die `widget-3`-Zeile vergleichen.

**A/B-Ergebnis (dritter Log, gleicher Patch, 16 Stimmen):** Sauberer Vergleich, da der Log diesmal auch den Einschwingvorgang zeigt (7/11/10/16 aktive Stimmen) - `widget-3`s dsp-Anteil skaliert dabei exakt linear mit der Stimmenzahl (≈1,33 %/Stimme bei 7, 11, 10 und 16 Stimmen), kein Block-Fixkosten-Artefakt.

- **`edge` (Kabel-Summierung):** ≈4,2 % → ≈2,15 % bei 16 Stimmen - **etwa halbiert.** Realer, klar messbarer Gewinn durch IRAM_ATTR auf `Graph::renderBlock()`.
- **`dsp` (Oszillator-Render selbst):** ≈21,3 % → ≈21,3 % bei 16 Stimmen - **praktisch unverändert**, trotz `IRAM_ATTR` auf `nodes::oscillator::render()` und den LUT-Lookups.
- **Gesamt `render`-Phase:** ≈34-36 % → ≈32-34 % - kleiner, aber realer Gewinn, der fast vollständig aus der Edge-Summierung stammt.

**Einordnung:** Die schlichte Kabel-Summierschleife (`dstBuf[i] += srcBuf[i];`, keine Branches, kein FPU-Sonderfall) profitiert sichtbar von IRAM - passt zu einem instruktions-fetch-limitierten Muster. Der Oszillator-Kern selbst (Sinus-LUT-Zugriff, Interpolation, mehrere Float-Multiplikationen, Float-Vergleiche für die Phasen-Wrap-Prüfung) bleibt exakt gleich teuer unabhängig vom Code-Ort - das deutet auf einen **rechenzeit-limitierten, nicht fetch-limitierten** Engpass hin: die eigentliche Gleitkomma-Arbeit pro Sample, nicht der Instruktionsabruf. ~1,33 %/Stimme entspricht bei 2902 µs Budget etwa 38,6 µs/Block/Stimme bzw. ~300 ns/Sample für eine einzelne Stimme im vollständig optimierten Konstant-Pitch-/konvergiertem-Gain-Pfad - mehr, als eine reine Handvoll Float-Operationen naiv vermuten ließe.

**Konsequenz für die in Abschnitt 1a zurückgestellten P1-Punkte 3 und 7:** Genau die dort geforderte Voraussetzung ("Disassembly prüfen und auf dem P4 messen") ist jetzt erfüllt, und die Messung zeigt einen rechenzeit-limitierten Kern - exakt das Szenario, in dem separate Wellenform-Schleifen (Punkt 3, weniger Register-/Branch-Druck pro Sample) und ein Integer-Phasenakkumulator (Punkt 7, weniger Float-Operationen pro Sample) tatsächlich greifen könnten.

**Punkt 3 ✅ umgesetzt** (Punkt 7 auf Nutzerwunsch weiterhin zurückgestellt). Statt sechs Schleifen von Hand zu duplizieren (Instruktionscache-Risiko, Wartungsrisiko): ein Template `PhaseOsc::processWithIncrementT<WaveType>()` ([DspPrimitives.h](../main/audio/engine/DspPrimitives.h)) plus `renderFastLoop<WaveType>()` ([OscillatorNode.h](../main/audio/engine/nodes/OscillatorNode.h)) - eine Quelle im Code, der Compiler erzeugt die sechs Spezialisierungen. Die Wellenform-Auswahl (der `switch`) passiert dadurch genau einmal pro Block statt einmal pro Sample - für die WaveGen-eigene Schleife ist das die strengstmögliche Form von "seltener prüfen" (Blockgranularität, 128 Samples, statt der vorgeschlagenen 256 - ein Stride wie beim Filter ergibt hier keinen Sinn, weil jedes Sample trotzdem erzeugt werden muss, nur die Dispatch-Entscheidung lässt sich seltener treffen). Nur der bereits als "häufigster Fall" bestätigte Fast-Path (konstantes Pitch/Tune) ist betroffen; der seltenere audio-rate-Pfad (echtes FM/Vibrato) behält den bisherigen generischen `process()` mit Laufzeit-`switch`.

Build grün, `idf.py size`: DIRAM 224.390 Byte (39,2 %, vorher 39,0 %) - **nur ~1,3 KB zusätzlich**, deutlich weniger als befürchtet, weil der Compiler `renderFastLoop`/`processWithIncrementT` komplett in `render()` hineingezogen hat (keine eigenständigen Symbole mehr, siehe Objdump) statt sechsmal echten Code zu erzeugen und zusätzlich Aufrufoverhead zu addieren.

**A/B-Ergebnis, vierter Log (gleicher Patch, 16 Stimmen, wieder mit Einschwingvorgang 2/9/16 Stimmen sichtbar):** `widget-3`s dsp-Anteil skaliert weiterhin sauber linear - jetzt **≈1,04 %/Stimme statt ≈1,33 %/Stimme** (9 Stimmen: 9,36 % → 1,04 %/Stimme; 16 Stimmen: 16,6-16,7 % → 1,04 %/Stimme). Das sind **rund 22 % weniger Rechenzeit pro Stimme allein durch Punkt 3** - der Compiler hatte den `switch(wave)` vorher tatsächlich *nicht* aus der Schleife herausgezogen, trotz Inlining; die manuelle Spezialisierung war kein Nullergebnis. `edge` bleibt bei den ≈2,1 % aus dem IRAM-Schritt (davon unabhängig, wie erwartet). Gesamte `render`-Phase bei 16 Stimmen jetzt ≈27-29 % statt ursprünglich ≈34-36 % - IRAM (Abschnitt 1c oben) und Punkt 3 zusammen haben den Render-Anteil um etwa ein Fünftel gesenkt, komplett auf echter Hardware verifiziert.

## 1d. Abschnitt 5 durchgearbeitet (neunte Runde, 10. September 2026)

Auf Nutzeranfrage "schau dir jetzt noch Abschnitt 5 an und setz das um" systematisch jede Tabellenzeile geprüft. Wie immer: Quellcode-Umsetzung, kein Hardwaretest, `idf.py build` nach jedem Zwischenschritt grün. Status direkt in der Tabelle unten markiert; Details hier.

**Umgesetzt:**

- **Hammond** (Hoch bei Orgelpatches) - überraschend bereits sehr weit vorbereitet (Stride, Integer-Phase, aktive Drawbars im statischen Pfad). Zwei echte Lücken geschlossen: konstantes Pitch/Tune wird jetzt genauso erkannt wie bei Oscillator (schließt exakt die in Abschnitt 2 benannte Lücke: "Hammond besitzt sie ebenfalls nicht"); der modulierte Pfad iteriert beim Aufsummieren jetzt nur noch über `activeDrawbar[]` statt aller neun Register (die Phase aller neun läuft weiterhin unbedingt weiter, für Kontinuität bei Reaktivierung). [HammondNode.h](../main/audio/engine/nodes/HammondNode.h).
- **LFO** (Mittel) - One-Shot-Endwert wird jetzt einmal berechnet und der Rest des Blocks direkt gefüllt statt pro Sample identisch neu berechnet zu werden (gleiches Transition-Muster wie WavePlayer). Globale Kopie (`primary`-Mirror) nutzt jetzt `memcpy` statt manueller Sample-Schleife. **Echte Pufferansicht (Alias statt Kopie) bewusst NICHT umgesetzt** - siehe Befund unten. Dreieck/Sägezahn-als-Rampe zwischen Knicken bewusst übersprungen (Wrap-Erkennung nötig, mehrstufige Fallunterscheidung, LFOs sind pro Preset wenige und die reine Formel ist schon billig - Aufwand/Risiko lohnt sich hier nicht). [LfoNode.h](../main/audio/engine/nodes/LfoNode.h).
- **ADSR** (Mittel) - Sustain/Idle wird jetzt als echte Konstante erkannt (`RampScheduler::isHolding()`, neu in [DspPrimitives.h](../main/audio/engine/DspPrimitives.h)) und der Block direkt gefüllt statt 128× `process()` aufzurufen, das ohnehin nur denselben Wert zurückgegeben hätte. `velocity*kMaxVolts` vor die Schleife gezogen. "Timingkonfiguration nicht erneut setzen" bewusst übersprungen - `configure()` schreibt nur 5 Felder, kein Zustand, der Vergleich davor wäre teurer als der Schreibvorgang selbst. [AdsrNode.h](../main/audio/engine/nodes/AdsrNode.h).
- **Gain/VCA** (Mittel) - Smoothing-Konvergenz-Stop ergänzt (gleiches Muster wie Oscillator/Hammond). "Gemeinsame Gain-Folge bei Global" geprüft und zurückgestellt - siehe Filter-Befund unten, derselbe Trap gilt hier genauso. [GainNode.h](../main/audio/engine/nodes/GainNode.h).
- **Mixer** (Mittel) - Eingänge mit Gain=0 werden jetzt einmal pro Block aus der Iteration ausgeschlossen statt pro Sample gelesen+multipliziert (gleiche Summenreihenfolge, bit-identisch für den Normalfall). [MixerNode.h](../main/audio/engine/nodes/MixerNode.h).
- **Adder/Multiplier/Divide/Abs** (Mittel) - Divide cacht jetzt den Kehrwert eines konstanten Nenners bei variierendem Zähler (Multiplikation statt Division pro Sample) - dieselbe "1/x einmal, dann multiplizieren"-Technik, die diese Codebase an anderer Stelle (Biquad::setCoeffs) schon bewusst ohne `-ffast-math` einsetzt, keine Neuerung im Grundsatz. [ArithmeticNodes.h](../main/audio/engine/nodes/ArithmeticNodes.h).
- **Switch** (Niedrig-Mittel) - gewählter Port wird jetzt einmal pro Block bestimmt statt pro Sample neu geprüft. Direktes Durchreichen (Pufferansicht) bewusst NICHT umgesetzt - siehe Befund unten. [SwitchNode.h](../main/audio/engine/nodes/SwitchNode.h).
- **Delay** (Mittel) - Sinus/Cosinus des Mix wird jetzt nur bei tatsächlicher Wertänderung neu berechnet (gecacht), nicht mehr bei jedem Block unbedingt. [DelayNode.h](../main/audio/engine/nodes/DelayNode.h).
- **Reverb/ReverbLight** (Mittel-Hoch) - genau die vom Plan benannte Lücke gefunden und behoben: RoomSize/Damping markierten "dirty" schon bei bloßem Verbunden-Sein der CV, nicht bei tatsächlicher Wertänderung (der direkt danebenstehende WetLevel/DryLevel-Code machte es schon richtig). Zusätzlich comb-Gain-Neuberechnung (8× `powf()`) und Damping-Cutoff-Neuberechnung entkoppelt, wie vom Plan gefordert - eine Änderung an nur einem der beiden Werte zahlt jetzt nicht mehr für die jeweils andere Ableitung mit. [ReverbNode.h](../main/audio/engine/nodes/ReverbNode.h).
- **AudioOut/UsbOut/Es8311Out/Wm8960Out** (Mittel) - Mute-Rampen-Konvergenz-Stop ergänzt (gleiches Muster wie überall sonst); der Limiter selbst bewusst nicht angefasst, exakt wie der Plan es für sich selbst verbietet ("Keine Änderung des Limiters allein zur Verbesserung eines Benchmarks"). [OutputNodes.h](../main/audio/engine/nodes/OutputNodes.h).
- **AudioOutputLink/AudioInputLink** (Mittel) - Kopieren und Gain-Multiplikation jetzt in einem Durchgang statt zwei getrennten Schleifen (memcpy+separate Multiply-Schleife vorher). [LinkNodes.h](../main/audio/engine/nodes/LinkNodes.h).

**Zwei echte Befunde beim Durchdenken gefunden, die einen naiven Fix verhindert haben:**

1. **"Gemeinsame Pufferansicht" bei globalen/gemeinsamen Signalen (LFO-Mirror, Switch-Durchreichen, Filter-Koeffizientenfolge) ist NICHT sicher, wenn per-Instanz-Idle-Handling mit im Spiel ist.** `Graph::renderBlock()`s "erster stiller Block nach Aktivität"-`memset` arbeitet auf der EIGENEN `outL` jeder Instanz. Würde eine Instanz per Zeiger-Alias direkt auf eine ANDERE Instanz zeigen (statt eine Kopie zu halten), würde dieser `memset` bei einer gerade still gewordenen Stimme die geteilte Quelle für alle anderen mit zerstören. Betrifft LFOs globalen Modus und Switch gleichermaßen - beide bekamen stattdessen nur die sicheren Teiloptimierungen (memcpy statt Sample-Schleife bzw. Port-Auswahl vor die Schleife gezogen).
2. **Filter/CurtisFilter "gemeinsame Koeffizientenfolge bei globalem Modus" wurde bewusst NICHT umgesetzt, nachdem ein potenzieller Hörfehler beim Durchdenken auffiel:** `Graph::renderBlock()` rendert Stimme 0 (primary) einer Widget-Instanzgruppe **vollständig fertig** (alle Samples), bevor Stimme 1 überhaupt zu rendern beginnt - kein Sample-Interleaving zwischen Stimmen. Eine naive "kopiere primary's Koeffizienten am selben Stride-Tick" würde daher am Blockende landen: andere Stimmen würden bei echter Audio-Rate-Modulation sofort auf den FINALEN Koeffizientenzustand springen statt densselben Sweep wie primary innerhalb des Blocks mitzuschwingen - ein echter, hörbarer Fehler (Stimmen laufen bei Modulation auseinander). Eine korrekte Umsetzung bräuchte einen dedizierten Puffer, der primary's komplette Koeffizientenfolge über den Block aufzeichnet und von anderen Stimmen an denselben Indizes wiedergegeben wird - neue, nicht triviale Infrastruktur. Zurückgestellt.

**Bewusst übersprungen, mit Begründung:**

- **Clock** (niedrigste Priorität der Tabelle) - aktuelle Pro-Sample-Kosten (2 Floatvergleiche, kein LUT) sind schon minimal; eine Segment-basierte Neufassung trägt echtes Timing-Risiko für eine hörbar-rhythmische Funktion, die von hier aus nicht gegenzuhören ist.
- **Const** - Renderfunktion war schon leer (bestätigt), nichts zu tun; profitiert zusätzlich automatisch vom Teilgraph-Pruning (Abschnitt 1b), wenn nur per ControlRoute genutzt.
- **Knob/Button/UpDownButton** - bräuchten dieselbe größere "Skalar-Ausgang statt Puffer-Fill"-Infrastruktur wie P2s Kernstück; profitieren ebenfalls automatisch vom Teilgraph-Pruning, wenn nur per ControlRoute genutzt. Nicht separat umgesetzt.
- **MIDI-In/MIDI-Split** - "Kein eigener DSP-Kernel" bestätigt (Render-Dispatch ist bereits ein No-Op), nichts zu tun.
- **VuMeter/Oscilloscope/Spectrum** - "keine eigene FFT-Berechnung" bestätigt; profitieren vom Teilgraph-Pruning, sofern nicht per Monitor-Tap angebunden (siehe Abschnitt 1b - dort bewusst beide Enden der Tap-Kante als gebraucht markiert, gerade weil die externe Anzeige-Implementierung außerhalb dieser Untersuchung lag).
- **EspEq/EspAlc/EspChannelConvert/EspMixer/EspSonic/EspDrc/EspMbc** - verlangen laut Plan explizit Hardware-A/B-Vergleiche gegen native Float-Alternativen, keine Quellcode-Änderung ohne das. EspSonics "Korrektheit vor Performance" nicht separat untersucht (Zeitbudget).
- **Unknown** - "Kein DSP" bestätigt, nichts zu tun.

## 1e. Ramp-Signaltyp umgesetzt (zehnte Runde, 10. September 2026)

Auf Nutzerwunsch "implementiere mal den Ramp-Typ" (der in Abschnitt 3/P2 sowie Zeile 91/207/242-243 auf "später" vertagte dritte Signalzustand) umgesetzt - mit vorheriger Rückfrage an den Nutzer, da eine Recherche vor der Umsetzung einen wichtigen, einschränkenden Befund ergab.

**Befund vor der Umsetzung:** Die zwei naheliegendsten, teuersten Fälle - MIDI-Glide-Pitch in den Oscillator, und ADSR-Hüllkurve in Oscillator-/Gain-Amplitude - profitieren **nicht** von einer Rampen-Propagierung, weil die CV→Ergebnis-Abbildung dort absichtlich nichtlinear ist (`cv::hzFromPitchCv()`: `440*2^x`; `cv::vcaGain()`: LUT-basierte Exponentialkurve). Eine lineare CV-Rampe ergibt dort keine lineare Hz-/Gain-Rampe; diese Verbraucher müssen so oder so jedes Sample einzeln neu berechnen, unabhängig von einer Tag-Markierung. Der reale Nutzen liegt bei reinen linearen CV-Rechenketten zwischen einer Rampen-Quelle und einem nichtlinearen Endverbraucher.

**Umgesetzt (bewusst single-hop, nicht graphweite Propagierung):** Neues drittes Signalzustands-Feld auf `NodeInstance` (`rampPortMask` + `RampDescriptor{port,start,inc}[2]` für die Eingangsseite, `outIsRamp`/`outRamp` für die Ausgangsseite) - klar getrennt vom bestehenden, unveränderten `controlRampPortMask` (ein transientes "nicht als konstant behandeln"-Sicherheitsflag ohne eigenen Wert, weiterhin nur für den MIDI-Glide-Pfad zuständig). `RampScheduler::wholeBlockRamp()` (neu, `DspPrimitives.h`) exponiert `(start, inc)` des aktuellen ADSR-Segments nur dann, wenn es den kompletten Block abdeckt (kein Segmentwechsel mitten im Block) - sonst wird wie bisher per `process()`-Schleife materialisiert. `AdsrNode.h` publiziert dieses Tag zusätzlich zur bestehenden, unveränderten Sample-für-Sample-Füllung von `outL` (rein additiv, kein Verhaltensunterschied für `outL` selbst).

Propagierung nutzt bewusst keine neue GraphBuilder-Analyse, sondern hängt sich an die bereits vorhandene `viewPortMask`-Eignungsprüfung (genau eine unskalierte, nicht-Monitor-Tap-Kante, Quelle topologisch garantiert vor Ziel gerendert) - dieselbe Voraussetzung, die ein Rampen-Tag ebenfalls braucht, kostenlos mitgeerbt. `Graph::renderBlock()`s View-Zweig kopiert das Tag der Quelle auf das Ziel, sonst unverändert.

Konsumentenseitig bekamen Adder, Detune, Gain (nur im bereits konvergierten, blockkonstanten-Amount-Zweig) und Divide (nur im bereits vorhandenen konstanter-Nenner-Zweig) je einen neuen dritten Zweig zwischen dem bestehenden Constant-Fastpath und dem bestehenden Per-Sample-Fallback - alle vier sind tatsächlich linear in ihrem jeweils verbliebenen Eingang, daher ist eine getaggte Rampe dort algebraisch exakt fortsetzbar. Multiplier bewusst nicht erweitert (Produkt zweier Rampen ist quadratisch, kein Ramp mehr). Jeder nicht Ramp-fähige Verbraucher (Oscillator, Hammond, Filter, CurtisFilter, ValueMapper, Switch, Multiplexer, Divide mit variablem Nenner) bleibt komplett unangetastet - materialisiert wie zuvor über den unverändert immer gefüllten `outL`-Puffer.

Bewusste Scope-Grenze: Propagierung geht genau einen Hop weit (ADSR → ein direkter Konsument). Die Konsumenten selbst taggen ihren eigenen Output nicht erneut - eine zweite Verkettung (z. B. ADSR→Adder→Detune) fällt zurück auf den immer korrekten Bestandspfad. Echte graphweite Mehrfach-Hop-Propagierung bleibt weiterhin offen.

Geänderte Dateien: [NodeInstance.h](../main/audio/engine/NodeInstance.h), [SynthConfig.h](../main/audio/engine/SynthConfig.h) (Kommentar), [DspPrimitives.h](../main/audio/engine/DspPrimitives.h), [AdsrNode.h](../main/audio/engine/nodes/AdsrNode.h), [Graph.h](../main/audio/engine/Graph.h), [ArithmeticNodes.h](../main/audio/engine/nodes/ArithmeticNodes.h) (`renderAdder`/`renderDivide`), [DetuneNode.h](../main/audio/engine/nodes/DetuneNode.h), [GainNode.h](../main/audio/engine/nodes/GainNode.h). `idf.py build` grün, Binärgröße 0x163010→0x163360 Byte (+848 B, 54 % App-Partition weiterhin frei). RAM-Mehrkosten pro `NodeInstance`: ~40-48 Byte (neue Felder), nicht separat mit `idf.py size` verifiziert.

**Noch nicht auf Hardware getestet/gehört.** Braucht Presets, die gezielt ADSR→Adder, ADSR→Detune, ADSR→Gain(konvergiert) und ADSR→Divide(konstanter Nenner) während Attack/Decay/Release durchlaufen, plus ein Preset mit skalierter oder Mehrfachkanten-Rampenquelle, um zu bestätigen, dass der nicht getaggte Fallback-Pfad weiterhin identisch klingt. CPU-Einsparung durch diese Änderung ebenfalls nicht gemessen.

## 2. Warum die WaveGens hinter Frequency Dividern mehr kosten

Quellen: [OscillatorNode.h](../main/audio/engine/nodes/OscillatorNode.h), insbesondere `audioPortConstantForBlock` und `render`; [ArithmeticNodes.h](../main/audio/engine/nodes/ArithmeticNodes.h), `renderFrequencyDivider`; [GraphBuilder.h](../main/audio/engine/GraphBuilder.h), Verbindungserzeugung.

Direktes MIDI-Pitch wird als ControlRoute behandelt. Bei gehaltener Note berechnet der WaveGen seine Frequenz einmal pro Block. Ein Frequency Divider hat dagegen einen regulären Sample-Ausgang: Sein Pitch-Puffer wird als Audio-Verbindung am WaveGen registriert. Ohne Konstanterkennung löst das eine CV-zu-Hz-Umrechnung für jedes Sample aus, obwohl die Note unverändert bleibt.

Für acht tatsächlich gerenderte Stimmen sind das pro WaveGen 352.800 statt ungefähr 2.756 Frequenzumrechnungen pro Sekunde. Der Faktor 128 betrifft nur diese Umrechnung, nicht die gesamte Synthese. Der zusätzliche Aufwand wird überwiegend beim WaveGen gemessen; die geringe Last des Dividers widerspricht dem daher nicht.

**Im aktuellen Arbeitsbaum ist diese konkrete Ursache bereits teilweise behandelt.** `audioPortConstantForBlock()` prüft den Pitch-Puffer vollständig. Bei Gleichheit aller Samples bleibt nur eine Hz-Berechnung pro Block. Die Prüfung selbst benötigt weiterhin bis zu 127 Vergleiche pro Stimme und Block. Für Tune gibt es diese Erkennung noch nicht. Hammond besitzt sie ebenfalls nicht.

Die Mehrlast im Screenshot lässt sich deshalb nicht eindeutig einer fehlenden Optimierung zuordnen: Der Stand der laufenden Firmware ist unbekannt. Außerdem unterscheiden sich die WaveGens in Wellenform, Tonhöhe und Tune. Sinus benötigt Tabellenzugriffe und Interpolation, Rechteck nur einen Phasenvergleich. Weißes Rauschen überspringt die periodische Phasenberechnung vollständig. Niedrigere Tonhöhe allein bedeutet keine größere Samplezahl oder längere Syntheseschleife.

Der Screenshot zeigt zwei separate Divider, einmal /4 und einmal /2. Für die Diagnose entscheidend ist jeweils der Übergang vom MIDI-ControlRoute zum Sample-Pitch-Eingang.

**Kontrollierter Nachweis:** Acht gleiche Stimmen, identische Wellenform, Amplitude und Tune; zuerst MIDI direkt, danach Divider /1, anschließend /2 und /4 mit entsprechend tieferer direkter Referenznote. Pro Variante die Zahl blockweiser und sampleweiser Frequenzberechnungen zählen. Glide, Tune-Modulation und echte Audio-FM separat prüfen. Für den Vergleich denselben Firmwarestand und dieselbe effektive Instanzzahl verwenden.

## 3. Priorisierte Umsetzung

| Schritt | Konkrete Änderung | Erwarteter Nutzen | Aufwand / Abhängigkeit |
|---|---|---|---|
| P0 ✅ | Reproduzierbare native Baseline mit Phasen des Renderdurchlaufs, Instanzzahlen und Fast-Path-Zählern | Belastbare Zuordnung von CPU-Last und Spitzen | Klein bis mittel; zuerst — **Quellcode umgesetzt, siehe Abschnitt 1a; Benchmarkmodus mit festem MIDI-Ablauf offen; Hardwaremessung offen** |
| P1 ✅ | WaveGen pro Block spezialisieren; konstantes Tune erkennen; Phasenschritt und Verstärkung vorberechnen; Smoothing nach Konvergenz beenden | Hoher Kandidat im gezeigten Patch | Mittel; unabhängig vom großen Graphumbau — **umgesetzt und auf Hardware verifiziert: Wellenform-Spezialisierung (Punkt 3) senkt den Oszillator-Anteil um ≈22 %/Stimme, siehe Abschnitt 1c. Integer-Phasenakkumulator (Punkt 7) auf Nutzerwunsch zurückgestellt** |
| P1 ✅ | Chorus mit einem Ringpuffer; WavePlayer bei abgeschlossener Release stoppen; Mapping-/Fade-Konstanten cachen | Konkrete, örtlich begrenzte Einsparungen | Klein bis mittel; eigene Vergleichsfälle — **Quellcode umgesetzt, siehe Abschnitt 1a; Hardwaremessung offen** |
| P2 ✅⏳ | Signalzustand `Constant`/`Buffer` durch DSP-Verbindungen propagieren; später `Ramp` | Hoher Kandidat für Pitch-/CV-reiche Patches und viele Stimmen | Größer; saubere Port- und Summierungssemantik erforderlich — **lokale Variante (kein Umsortieren nötig) umgesetzt für Adder/Multiplier/Divide/Abs/FrequencyDivider/Detune/ValueMapper/Oscillator-Amplitude, siehe Abschnitt 1b. `Ramp`-Typ umgesetzt (Abschnitt 1e), aber bewusst nur single-hop (ADSR → ein direkter Konsument: Adder/Detune/Gain/Divide) statt echter graphweiter Mehrfach-Hop-Propagierung - Hardwaretest offen** |
| P2 ✅ | Einfache Verbindungen ohne Summierpuffer; ungenutzte Busse entfernen; nicht benötigte Teilgraphen aus Renderplan nehmen | Weniger Speicherzugriffe, RAM und Graph-Overhead | Größer; Ownership und Graphabhängigkeiten absichern — **vollständig umgesetzt: Pufferansicht (auf komplexem 40-Stimmen-Preset am Klang bestätigt), Teilgraph-Pruning und Builder-seitiges Link-Bus-Weglassen, siehe Abschnitt 1b. Nur der vom Plan selbst auf "später" vertagte `Ramp`-Typ bleibt offen** |
| P3 ✅ | Filter-Koeffizienten bei identischen CVs wiederverwenden; gemeinsame Modulation einmal vorberechnen; Block-Biquad vergleichen | Mittel bis hoch bei modulierten/mehrstimmigen Filtern | Mittel; Filterzustände bleiben pro Stimme — **Quellcode umgesetzt (Filter + CurtisFilter), siehe Abschnitt 1b; Hardwaremessung offen** |
| P3 ✅ | Pow2-/VCA-LUT zunächst auf je 257 Einträge reduzieren | 6.136 Byte weniger RAM; CPU-Effekt offen | Klein; numerische Prüfung und Hardware-A/B — **Quellcode umgesetzt, siehe Abschnitt 1b; Hardware-A/B offen** |
| P4 ⚠️ | SoundFont-Datenlayout, Effektkonvertierungen, Render-Metadaten und Ereignisverarbeitung optimieren | Presetabhängig; bessere Spitzenlast möglich | Mittel bis groß — **SoundFont-Ausgabelayout (planar) umgesetzt; Voice-State-RAM/Effektvergleiche/Ereignisverarbeitung zurückgestellt (vendored Library bzw. Hardwaremessung nötig), siehe Abschnitt 1b** |
| P5 ✅⏳ | Gezielte IRAM-/Compiler-/ESP-DSP-Versuche, erst danach optional Aufteilung auf zwei Kerne | Nur bei verbleibenden gemessenen Engpässen | Hoch; keine pauschalen Beschleunigungsversprechen — **IRAM-Platzierung des Hot Path umgesetzt, nachdem Hardwaremessung + Disassembly (Abschnitt 1c) den Engpass belegten; Wirkung auf der Hardware noch nicht verifiziert. Compiler-/ESP-DSP-Experimente und Kernaufteilung weiterhin nicht begonnen** |

Die Schritte sollen getrennt gemessen und übernommen werden. Eine globale Prozentersparnis vor der Umsetzung wäre spekulativ. Als Größenordnung: Würden ausschließlich die WaveGens des Screenshots 30 % schneller, entspräche das etwa 14,1 Prozentpunkten weniger Blocklast. Das ist ein Rechenbeispiel, keine Prognose.

### P1: WaveGen-Schleife vereinfachen

`PhaseOsc::process()` prüft bei jedem Sample Noise/PinkNoise, die Wellenform, Phasenbereich und Sample-Rate. Auch bei konstanter Frequenz berechnet `advance()` den Phasenzuwachs wiederholt. Der WaveGen ruft bei nicht angeschlossenem Gain fortlaufend den One-Pole-Smoother auf, selbst wenn sich der Gain nicht mehr ändert.

Vorgeschlagene Struktur:

1. Wellenform und Kombination aus konstanten/modulierten Ports einmal pro Block auswählen.
2. Für konstantes Pitch/Tune `phaseIncrement = hz / sampleRate` einmal vorberechnen und bei unveränderten Parametern weiterverwenden.
3. Separate innere Schleifen für Sinus, Rechteck, Dreieck, Sägezahn sowie die beiden Rauscharten. Keine Wellenformauswahl innerhalb der Sample-Schleife.
4. Gain-, Amplituden- und Portzeiger vor der Schleife auflösen. Bei konstanten Faktoren gemeinsame Verstärkung vorab bilden.
5. Smoothing nur bis zur definierten Konvergenz ausführen und anschließend den internen Zustand explizit auf den Zielwert setzen. So startet die nächste Änderung nicht von einem alten Restwert.
6. Den bestehenden Pitch-Konstanzpfad kurzfristig auf Tune erweitern; langfristig die Puffersuche durch Graphmetadaten ersetzen.
7. Integer-Phasenakkumulator als separate Variante messen. Die obersten Bits liefern den Tabellenindex, die unteren Bits die Interpolationsposition; der Überlauf übernimmt das Wrapping. Hammond zeigt bereits eine verwandte Implementierung.

Alternativ kann die Floatphase bleiben. Bei garantiertem Phasenzuwachs im passenden Bereich genügt für den normalen Überlauf eine Subtraktion von 1. Für große/negative Frequenzen oder Phasenoffsets muss ein allgemeiner Pfad erhalten bleiben. Aktuell ist der kombinierte Pitch-/Tune-Bereich nicht zuverlässig unter Nyquist begrenzt; pauschales Ersetzen von `floorf()` wäre daher falsch.

Compiler können Teile der invarianten Arbeit bereits herausziehen. Vor Übernahme die tatsächlich erzeugten Schleifen im Disassembly prüfen und auf dem P4 messen. Spezialisierung darf den Instruktionscache nicht durch zu viele Varianten verschlechtern.

### P2: Konstante Signale und Pufferarbeit

Der aktuelle `audioConnectedPortMask` beschreibt die Verkabelung, nicht die zeitliche Veränderung des Signals. Ein Kabel von einem Divider, einem Knob oder einem ADSR im Sustain-Zustand kann vollständig konstante Daten transportieren.

Den vorhandenen Skalarmechanismus deshalb auf Node-Ausgänge und reguläre DSP-Verbindungen erweitern:

- Zunächst je Ausgang nur `Constant(value)` oder `Buffer(pointer)` führen. Bei mehreren Eingängen bleibt ein Ergebnis nur dann konstant, wenn alle beteiligten Quellen konstant sind.
- Frequency Divider, Detune, Adder, Multiplier, Divide und Abs propagieren diese Information. Clamps, Division durch null und die heutige Additionsreihenfolge erhalten.
- Rampen später als eigenen Typ ergänzen. Nicht jede Operation erhält eine lineare Rampe: Division, Clamp-Grenzen und diskrete Auswahl können einen Sample-Puffer erfordern.
- MIDI-Pitch ohne laufenden Glide benötigt keinen pro Block gefüllten Sample-Puffer. Für Glide nur im aktiven Zeitraum materialisieren oder eine Rampe transportieren.
- Bei nur einer unskalierten Quelle und einem ausschließlich lesenden Ziel auf den Quellpuffer verweisen. Das entfernt Löschen und anschließendes `+=` des Zielpuffers. Mehrfachquellen verwenden weiterhin einen Akkumulator.
- Für Mehrfachquellen kann bereits die erste Quelle den Akkumulator initialisieren; danach erst addieren. Das spart den gesonderten Clear-Durchlauf, wenn alle Sonderfälle wie inaktive Stimmen definiert sind.
- Nur verwendete Link-Busse anlegen und transportieren. Aktuell erzeugt der Builder alle acht Verbindungen zwischen beiden Link-Nodes, und beide Nodes bearbeiten alle acht Busse.
- Nur vom Ausgang oder einem aktiven Monitor benötigte Teilgraphen rendern. Heute werden grundsätzlich alle Widgets in die Renderreihenfolge aufgenommen; explizit übersprungen wird beispielsweise ein unverbundener LFO.

Ein Mono-Puffer umfasst 512 Byte pro Instanz, bei acht Stimmen 4 KiB und bei 32 Stimmen 16 KiB. Entfernte Puffer schaffen deshalb zusätzlich Raum für wirklich häufig benutzte Zustände im internen RAM.

Vor dem Umbau Ownership von Pufferansichten und tatsächlich allokiertem Speicher trennen: `Graph::destroy()` darf geliehene Zeiger nicht freigeben. Rückkopplungen, globale Fan-outs, Voice-Summen, Live-Schalter und Monitor-Taps gehören ausdrücklich zu den Regressionstests.

Nicht ausgewählte Multiplexer-Zweige dürfen nicht automatisch eingefroren werden: Phasen, Hüllkurven, Clock und Effekttails können beim späteren Umschalten hörbar sein. Sichere erste Stufe ist das Entfernen wirklich nicht erreichbarer Teilgraphen; dynamische Abschaltung benötigt definierte Zustandsfortschreibung.

## 4. Lookup-Tabellen: Größe, Fehler und Grenzen

Quellen: [SynthConfig.h](../main/audio/engine/SynthConfig.h), [DspPrimitives.h](../main/audio/engine/DspPrimitives.h), [HammondNode.h](../main/audio/engine/nodes/HammondNode.h).

Aktuell belegen die vier Tabellen zusammen 26.624 Byte, also 26 KiB. Die vorhandene Linker-Map zeigt die Tabellen als `.bss`-Symbole. Sie werden beim Start im RAM gefüllt; es sind keine pro Stimme duplizierten Tabellen.

| Tabelle | Aktuell | Erster sinnvoller Versuch | RAM-Ersparnis | Bewertung |
|---|---:|---:|---:|---|
| Sinus | 4.096 Floatwerte, 16 KiB | Zunächst unverändert | 0 | Gemeinsame Nutzung durch Oszillator, Hammond und Filter beachten |
| Pow2 | 1.024 Floatwerte, 4 KiB | 257 Werte, 256 Intervalle | 3.068 Byte | Sehr kleiner Pitchfehler; gute erste Verkleinerung |
| VCA exponential | 1.024 Floatwerte, 4 KiB | 257 Werte, 256 Intervalle | 3.068 Byte | Kleine absolute Verstärkungsabweichung |
| tanh | 512 Floatwerte, 2 KiB | Zunächst unverändert; später 257 | Optional 1.020 Byte | In nichtlinearer Rückkopplung prüfen |

Bei linearer Sinusinterpolation ist die theoretische absolute Fehlerobergrenze `π²/(2*N²)`, bezogen auf Amplitude 1, ohne Float-Rundungsfehler:

| Einträge pro Periode | Fehlerobergrenze | Fehlerpegel relativ zu Amplitude 1 |
|---:|---:|---:|
| 4.096 | 2,94e-7 | −130,6 dB |
| 2.048 | 1,18e-6 | −118,6 dB |
| 1.024 | 4,71e-6 | −106,5 dB |
| 512 | 1,88e-5 | −94,5 dB |

Das sind Punktfehlergrenzen, keine gemessenen THD+N- oder Hörbarkeitsergebnisse. Für einen isolierten interpolierten Sinusoszillator erscheinen 1.024 oder 2.048 Werte daher aussichtsreich. Ein kleinerer LUT bedeutet aber dieselbe Zahl von Tabellenzugriffen und Interpolationsoperationen; schneller wird er nur bei günstigeren Speicherzugriffen oder einem vereinfachten Lookup.

**Hammond ist eine wichtige Ausnahme:** Dort erfolgt derzeit direkter Tabellenzugriff ohne Interpolation; `static_assert(kSineLutSize == 4096)` und `kSineLutIndexShift = 20` fixieren die Größe. Die obige Fehlerabschätzung gilt dafür nicht. Ein einfaches Verkleinern bricht zunächst den Build und erhöht nach Anpassung die Phasenquantisierung. Die Integerphase und ein eigener interpolierter Lookup müssen zusammen bewertet werden.

**Auch Filter sind eine wichtige Ausnahme:** Der RBJ-Biquad bildet `1 - cos(w0)`. Bei tiefen Frequenzen liegt der Kosinus extrem nahe an 1; dort kann ein kleiner absoluter LUT-Fehler einen großen relativen Koeffizientenfehler verursachen. Eine Host-Rechnung mit der aktuellen linearen Tabellengeometrie bei 20 Hz/44.100 Hz ergibt für `1-cos(w0)` gegenüber dem analytischen Wert ungefähr +3,54 % bei 4.096 und +115,33 % bei 1.024 Einträgen. Diese Zahlen sind keine direkt gemessene Grenzfrequenzabweichung und enthalten keine Float32-Rundung. Sie zeigen, warum ein kleiner Fehlerpegel beim Sinus allein die Filterqualität nicht garantiert.

Vor einer gemeinsamen Sinusverkleinerung die Koeffizientenbildung numerisch stabilisieren, etwa `1-cos(w0) = 2*sin(w0/2)^2` für die passende Stelle, und den vollständigen Frequenzgang einschließlich Q und Stabilität prüfen. Alternativ Filterberechnung und Oszillator-Lookup trennen. Eine zusätzliche kleine Tabelle bei beibehaltener großer Tabelle spart jedoch keinen Gesamt-RAM.

Für 257 Pow2-Werte liegt der maximale Interpolations-Pitchfehler näherungsweise bei **0,00159 Cent**; selbst 129 Werte ergeben nur ungefähr **0,00635 Cent**. Für 257 VCA-Werte ist die theoretische absolute Fehlergrenze etwa **1,12e-5** bezogen auf maximalen Gain 1. Nahe Stille ist das nicht gleichbedeutend mit demselben relativen Pegelfehler. Eine tanh-Tabelle mit 257 Werten hätte auf [0,4] eine theoretische Interpolationsfehlergrenze von etwa **2,35e-5**; die vorhandene Sättigung außerhalb ±4 verursacht eine davon unabhängige Abweichung.

Die Zahlen wurden mit Python aus den Interpolationsformeln berechnet. Für die eigentliche Freigabe müssen zusätzlich die exakten Float32-Implementierungen gegen Referenz und aktuellen Stand geprüft werden.

Weitere LUT-Versuche nachrangig:

- Ein zusätzlicher Endpunkt beim Sinus vermeidet den Sonderfall des zweiten Index am Tabellenende.
- Pow2 mit 256 Intervallen vereinfacht die Skalierung; `modff`/`ldexpf` im Disassembly prüfen. Ein spezialisierter Exponentenpfad braucht definierte Grenzen und einen korrekten Fallback.
- Die Pitchformel lässt sich mathematisch von CV über MIDI und zurück auf `440 * 2^(volts - 5,75)` verkürzen. Gültigkeit für den verwendeten CV-Anker und Rundungsabweichungen prüfen.
- Viertelwellentabellen sparen RAM, benötigen aber zusätzliche Spiegelungs-/Vorzeichenlogik. Integer-Tabellen brauchen Konvertierung. Beides nur nach Messung als CPU-Optimierung behandeln.
- Interpolation nicht pauschal abschalten. Bandbegrenzung von Rechteck/Sägezahn ist eine separate Qualitätsfrage; ihre derzeit vorhandenen Aliasing-Anteile werden durch kleinere LUTs nicht behoben.

## 5. Prüfung aller nativen Node-Gruppen

Die Priorität in dieser Tabelle bezeichnet den Nutzen für die gesamte Engine. Nicht jede Änderung wirkt im Screenshot.

| Node / Gruppe | Konkreter Befund und Maßnahme | Priorität |
|---|---|---|
| WaveGen / Oscillator, alle sechs Wellenformen | Blockweise spezialisierte Schleifen, konstantes Pitch/Tune, vorberechneter Phasenschritt, konvergiertes Smoothing stoppen; Noise überspringt Phase bereits | Sehr hoch ✅ |
| Hammond | Konstante Pitch-/Tune-Puffer erkennen; neun Phasenschritte nur bei Frequenz-/Ratioänderung bilden; im modulierten Pfad Vorberechnung und aktive Drawbars stärker nutzen; stille Drawbar-Phasen weiterführen | Hoch bei Orgelpatches ✅ |
| LFO | Sinus nutzt bereits Blockinterpolation; globale Kopien durch gemeinsame Pufferansicht ersetzen; Dreieck/Sägezahn zwischen Knicken als Rampen behandeln, Kanten/Sync erhalten; One-Shot-Endwert direkt halten | Mittel ✅⚠️ (Pufferansicht/Rampen bewusst zurückgestellt, s. Abschnitt 1d) |
| Clock | Pulswechsel über Samplezähler bzw. Grenzpositionen vorberechnen, konstante Abschnitte ausgeben; gebrochene Perioden mit Restakkumulator erhalten, sonst Tempo-Drift | Niedrig bis mittel ⏳ (übersprungen, s. Abschnitt 1d) |
| Detune | Konstante Ein-/Ausgänge als Skalare; geglätteten Betrag und beide Offsets einmal vorbereiten; laufende Glide-Rampen erhalten | Mittel ✅ |
| Frequency Divider | Oktavabstand bereits gecacht; konstanten Ausgang deklarieren; Fast Path muss auch laufende Control-Rampen berücksichtigen | Sehr hoch für nachfolgende Generatoren ✅ |
| ADSR | Sustain und Idle als konstante Ausgänge; Rampen abschnittsweise rendern; Velocity*10 einmal vorberechnen; Timingkonfiguration bei unverändertem CV nicht erneut setzen | Mittel ✅ |
| Filter LP/HP/BP | Unveränderte CV-Werte trotz Audio-Kabel erkennen; nur geänderte Cutoff-/Q-Abbildung berechnen; bei gemeinsamem Modulationssignal Koeffizientenfolge einmal pro Widget bilden | Hoch bei Modulation ✅⚠️ (Koeffizientenfolge-Teilung zurückgestellt, echter Hörfehler-Trap gefunden, s. Abschnitt 1d) |
| CurtisFilter LP24/LP12/BP12/HP12 | `% stride` durch Zähler ersetzen, Modus außerhalb Sample-Schleife wählen; Koeffizienten cachen; drei Sättigungen pro Sample sind wesentlicher DSP-Aufwand, Reduktion verändert Klang | Hoch bei Curtis-Patches ✅ |
| Gain/VCA linear/exponential | Kurve und Modulationsart einmal auswählen; Smoothing nach Konvergenz stoppen; konstante Gains vorberechnen; bei Global gemeinsame Gain-Folge nutzen, Audio bleibt pro Stimme | Mittel ✅⚠️ (Gain-Folge-Teilung zurückgestellt, derselbe Trap wie Filter) |
| Mixer | Aktive Eingänge und Gains einmal vorbereiten; Null-Gain-Eingänge nicht lesen; zusammenhängende Add-/Multiply-Schleifen statt wiederholter Portauswahl prüfen | Mittel ✅ |
| Adder / Multiplier / Divide / Abs | Skalar-/Rampeneigenschaften weitergeben; konstante Nenner beim Divide durch gecachten Kehrwert behandeln; bisherige Nullsemantik erhalten | Mittel ✅ |
| ValueMapper | Steigung/Offset oder inversen Eingangsbereich bei Paramänderung cachen; im Samplepfad steht aktuell eine Division durch denselben Bereich | Mittel ✅ |
| Switch | Gewählten Eingangszeiger einmal wählen; bei passender Ownership direkt weiterreichen | Niedrig bis mittel ✅⚠️ (Durchreichen bewusst zurückgestellt, s. Abschnitt 1d) |
| Multiplexer | Ohne Fade Pufferansicht; während Fade Kehrwert der Fade-Länge cachen statt pro Sample dividieren; nach Fade Rest des Blocks direkt kopieren | Mittel ✅ |
| Const | Renderfunktion ist bereits leer und Wert wird per ControlRoute verteilt; unbenutzten Output-Puffer und Renderdispatch einsparen | Niedrig ✅ (bereits leer, profitiert von Teilgraph-Pruning) |
| Knob / Button / UpDownButton | Aktuell werden konstante Output-Blöcke gefüllt; durch Skalar-Ausgänge ersetzen; Mapping bleibt ereignisgesteuert | Niedrig einzeln, mittel als Graphverbesserung ⏳ (braucht P2-Infrastruktur, s. Abschnitt 1d) |
| MIDI-In / MIDI-Split | Kein eigener DSP-Kernel. Routen nach Quelle, Kanal/CC und Ziel vorbereiten; VoiceAllocator nicht als primären Dauerton-Hotspot behandeln | Niedrig für gehaltene Noten ✅ (bestätigt bereits No-Op) |
| WavePlayer | Nach Gate-off und vollständig abgeklungenem 20-ms-Fade `playing=false` setzen; derzeit laufen Sample-Lesezugriffe bis Dateiende weiter. Restblock beim Ende zusammenhängend nullen. Pitchratio cachen; langer Float-Positionsakkumulator separat auf Präzision prüfen | Hoch für lange Samples ✅ |
| SoundFontPlayer / TinySoundFont | Voice-/Filterzustände bevorzugt intern, Samplebank weiter PSRAM; derzeit führt der Allocation-Hook grundsätzlich zuerst nach PSRAM. Interleaved-zu-Planar-Kopie und nachfolgenden Gain-Durchlauf zusammenlegen; native getrennte Ausgabe prüfen | Hoch bei SF2-Patches ✅⚠️ (Voice-State-RAM zurückgestellt, vendored-Lib-Problem) |
| Chorus | Beide DelayLines bekommen dasselbe Mono-Sample und haben dieselbe Länge: ein gemeinsamer Ring mit zwei interpolierten Lesetaps erhält das Verfahren und spart einen Schreibstrom sowie etwa 17,6 KiB pro Instanz. LFO-Schritt, Offset/Depth in Samples vorberechnen | Hoch, klar begrenzt ✅ |
| Delay | Sinus/Cosinus des Mix nur bei Änderung berechnen; Zeit und Feedback vorbereiten; Ringzugriff in zusammenhängenden Abschnitten prüfen; auch bei Mix=0 Tail-/Wiederaktivierungssemantik erhalten | Mittel ✅ |
| Reverb / ReverbLight | RoomSize/Damping werden bei angeschlossenen CVs jeden Block neu abgeleitet, auch bei gleichem Wert. Änderungen erkennen und beide Ableitungen entkoppeln. Speicherzugriffe der Ringe messen; Tails beim Abschalten beachten | Mittel bis hoch ✅ |
| EspEq | Pro Node zwei Float/S32-Umwandlungen; einzelne Standardfilter gegen native Float-Variante vergleichen, zusammenhängende ESP-Effektketten möglichst im selben Format halten | Mittel ⏳ (braucht Hardware-A/B) |
| EspAlc | Statische Kanalverstärkung im Floatgraph gegen Library plus Konvertierungen vergleichen; die konkrete Library-Gain-/Clipping-Semantik erhalten | Mittel ⏳ (braucht Hardware-A/B) |
| EspChannelConvert | Gewichtete Kanalsumme direkt in Float prüfen; aktuelle Konvertierungen und Kopie des Monokanals verursachen Zusatzdurchläufe | Mittel ⏳ (braucht Hardware-A/B) |
| EspMixer | Fade-Verfahren im Floatgraph vergleichen oder Konvertierungen zwischen benachbarten ESP-Nodes vermeiden; nicht auf einfache statische Summe reduzieren | Mittel ⏳ (braucht Hardware-A/B) |
| EspSonic | Variable Ein-/Ausgabemengen benötigen korrekte Pufferung. Zunächst `in_num`/`out_num`, Verbrauch, Restdaten und FIFO-Verhalten prüfen; erst dann Interleave-/Konvertierungsschleifen optimieren | Korrektheit vor Performance ⏳ (nicht untersucht) |
| EspDrc / EspMbc | Library-Kernel separat von Formatkonvertierung messen; gemeinsame S32-Ketten und internen Scratch testen; Algorithmus/Lookahead nicht stillschweigend ändern | Mittel, presetabhängig ⏳ (braucht Hardware-A/B) |
| AudioOutputLink / AudioInputLink | Nur belegte der acht Mono-Busse bearbeiten; unveränderten Gain 1 durchreichen; Kopieren und Multiplikation zusammenfassen; Voice-Summe nur an vorgesehener Grenze | Mittel ✅ |
| AudioOut / UsbOut / Es8311Out / Wm8960Out | Gleicher nativer Output-Kernel: konstante Kanaltrims vorberechnen, konvergierte Mute-Rampe stoppen, Limiter prüfen; aktive Senken bei Kopieren/PCM-Konvertierung berücksichtigen | Mittel ✅ |
| VuMeter / Oscilloscope / Spectrum | Im nativen `processNodeBlock()` keine eigene FFT-/Messberechnung; Verkabelung erzeugt dennoch Puffer- und Summierarbeit. Native Taps nur bei Bedarf transportieren. Externe Anzeigeimplementierung wurde nicht untersucht | Niedrig bis mittel ✅ (profitiert von Teilgraph-Pruning) |
| Unknown | Kein DSP; unbekannte/fehlgeschlagene Nodes nicht als nutzlose Renderarbeit führen | Niedrig ✅ (bestätigt bereits No-Op) |

Quellen für die Detailtabelle: [nodes](../main/audio/engine/nodes), [NodeRender.h](../main/audio/engine/NodeRender.h), [SoundFontPlayer.h](../main/audio/engine/SoundFontPlayer.h), [TinySoundFont](../components/tiny_soundfont/include/tsf.h), [ESP-Effektadapter](../main/audio/engine/EspAudioEffectsAdapter.h), [zusätzliche ESP-Effekte](../main/audio/engine/EspAdditionalEffects.h).

### Filter: gemeinsam vorberechnen ist nicht gemeinsam filtern

Der Global-Schalter von Filter und Gain teilt im aktuellen Code hauptsächlich die Modulationsquelle. Er reduziert nicht automatisch die Anzahl der Audio-Instanzen. Identische Koeffizienten kann man gemeinsam vorberechnen, während die Filterzustände pro Stimme getrennt bleiben.

Ein einzelner Filter hinter der Stimmensumme wäre nur für lineare Filter mit identischen zeitabhängigen Koeffizienten und kompatiblen Zustands-/Resetregeln mathematisch entsprechend. Unterschiedliche Hüllkurven pro Stimme, individuelle Resonanzen und insbesondere Curtis-Sättigung schließen eine automatische Zusammenlegung aus. Effekte hinter dem Link können als bewusste Patchgestaltung viel CPU sparen, verändern aber gegebenenfalls den Klang.

Für langsame Modulation kann ein größerer Koeffizientenabstand mit geglättetem Verlauf geprüft werden. Bei echter Audio-FM weiterhin volle Auflösung anbieten. Koeffizienten nicht beliebig interpolieren, ohne Stabilität und Resonanzverhalten zu prüfen.

## 6. Korrektheitsbefunde, die Optimierungen beeinflussen

Diese Punkte sind im Quellcode sichtbar, wurden aber nicht auf Hardware reproduziert. Sie sollten als getrennte Änderungen behandelt werden, damit Klangänderungen nicht irrtümlich einer CPU-Optimierung zugeschrieben werden.

1. **Glide durch CV-Rechennodes** ✅ **behoben:** Graph setzt `controlRampPortMask`, aber unter anderem Frequency Divider, Detune, ValueMapper und Arithmetic-Fast-Paths prüfen nur `audioConnectedPortMask`. Ein direktes MIDI-Glide kann daher am ersten Rechennode auf Sample 0 des Blocks reduziert werden. Die neue Konstanzlogik muss beide Signalarten berücksichtigen. Im Screenshot ist Glide aus; dieser Punkt erklärt dort keine Zusatzlast. *Umgesetzt: alle betroffenen Block-Konstant-Fastpaths ([ArithmeticNodes.h](../main/audio/engine/nodes/ArithmeticNodes.h), [DetuneNode.h](../main/audio/engine/nodes/DetuneNode.h), [ValueMapperNode.h](../main/audio/engine/nodes/ValueMapperNode.h)) prüfen jetzt zusätzlich `controlRampPortMask`, Build grün, Hardwaremessung offen.*
2. **Curtis LP24** ✅ **behoben:** `processStage(s, 2, stage2)` liefert den Ausgang der dritten Stufe zurück, dieser wird verworfen. Die vierte Stufe erhält stattdessen `s.stage[2]`, also den aktualisierten Integratorzustand. Gegen das beabsichtigte TPT-Modell prüfen, bevor Frequenzgänge als Referenz für Optimierungen eingefroren werden. *Entscheidung (Nutzer): fixen. [CurtisFilterNode.h](../main/audio/engine/nodes/CurtisFilterNode.h) reicht jetzt Stage 3s tatsächlichen Output an Stage 4 weiter (korrekte 4-Pol-TPT-Kaskade). Ändert hörbar Frequenzgang/Resonanzcharakter von LP24 gegenüber dem bisherigen Verhalten - bestehende Presets in diesem Modus klingen danach anders. Build grün, Hörvergleich auf Hardware offen.*
3. **ESP-Effekt-Pegel** ✅ **behoben:** Die Engine führt Audio nominell in ±5 V, `floatToS32()` sättigt aber bereits bei ±1 und `s32ToFloat()` liefert normalisierte Werte zurück. In den untersuchten Adaptern fehlt eine explizite Umrechnung zwischen beiden Bereichen. Das kann Clipping und andere Pegel verursachen. Vor Kernel-/Formatvergleichen normalisierte und CV-Audiodomäne sauber abgleichen. *Umgesetzt: [EspAudioEffectsAdapter.h](../main/audio/engine/EspAudioEffectsAdapter.h)s `floatToS32()`/`s32ToFloat()` normalisieren jetzt mit `cv::kAudioPeakVolts`, exakt wie OutputNodes.h es an der Hardware-Ausgabe bereits tut. Eindeutiger Fix (kein Klang-Ermessensspielraum wie bei Punkt 2) - vorher clippte jedes normal-lautstarke Audiosignal durch EspEq/EspAlc/EspChannelConvert/EspMixer/EspSonic/EspDrc/EspMbc hart bei einem Fünftel seines Pegels. Build grün, Hardwaremessung offen.*
4. **Stimmenruhe ist graphweit nach Slotnummer** ⏳ **nicht umgesetzt:** `voiceStillSounding[v]` wird über alle verbundenen ADSRs gebildet, ohne eigene Domäne pro MIDI-In oder Signalzweig. Eine lange Modulations-ADSR kann damit andere Zweige desselben Slots weiterlaufen lassen. Ein Sampleplayer oder Effekt benötigt umgekehrt eigene Regeln für sein Ende. Aktivität nach Abhängigkeiten und Tails modellieren, nicht allein anhand irgendeiner ADSR oder eines momentanen Nullsamples. *Bewusst zurückgestellt: das ist ein Modellierungsproblem (Abhängigkeits-/Tail-Graph statt Slot-weitem ADSR-ODER), kein lokaler Fix - Risiko, Presets mit langen Modulations-ADSRs oder Sample-/Effekt-Tails falsch abzuschneiden, ohne Hörtests über viele Topologien nicht verantwortbar von hier aus.*

## 7. Engine, Echtzeit und Speicher

### Profiling korrekt auswerten

In [Graph.h](../main/audio/engine/Graph.h) liegt der Node-Timer um die Renderaufrufe aller Instanzen eines Widgets. Eingangspuffer vorbereiten und ausgehende Verbindungen summieren liegen außerhalb. Pro Block wird nur ein Widget vermessen und anschließend geglättet. Die Widgetanzeigen sind deshalb weder eine vollständige Kostenaufteilung noch exakt gleichzeitige Messungen. Das erklärt auch, weshalb ein Monitor 0,0 % zeigen kann und trotzdem Transportarbeit verursacht.

Zusätzlich je Messfenster getrennt erfassen:

- MIDI-/Parameterverarbeitung, Aktivitätsermittlung, Pufferinitialisierung.
- Node-DSP insgesamt, Kabel-Summierung, Outputkopien und PCM-Konvertierung.
- Anzahl aktiver, tatsächlich gerenderter und trotz Ruhe gerenderter Instanzen.
- Konstante/modulierte Pitch- und Tune-Blöcke, Hz-Berechnungen, Filter-Koeffizientenupdates.
- Mittlere Renderzeit, P95/P99, Maximum, Deadline-Überschreitungen und DMA-Unterläufe soweit verfügbar.

**Umsetzungsstand (siehe Abschnitt 1a):** Die ersten drei Punkte sind implementiert - Node-DSP/Kabel-Summierung sind jetzt für das jeweils profilierte Widget getrennt gemessen, ebenso Aktivitätsermittlung/Pufferinitialisierung als eigene Blockphase; dazu Instanzzähler (aktiv/gerendert/trotz Ruhe gerendert) und Fast-Path-Zähler für WaveGen-Pitch/-Tune. MIDI-/Parameterverarbeitung waren bereits vorher separat gemessen (`taskLoop()`s `midiUs`/`liveParamUs`), ebenso Mittelwert/P95/P99/Maximum/Deadline-Überschreitungen (`TimingSnapshot`, `renderOverrunCount()`) - die Annahme, diese fehlten noch, war ungenau. Offen: Filter-Koeffizientenupdates als eigener Zähler, DMA-Unterläufe, und die PCM-Konvertierung ist bereits gemessen (`esConvertUs`/`wmConvertUs`), aber nicht in dieser Liste verlinkt.

`esp_timer_get_time()` misst verstrichene Zeit und enthält mögliche Unterbrechungen. Ein Cycle-Counter hilft bei kurzen isolierten Kernels, ist ohne Beachtung von Interrupts ebenfalls keine automatisch exklusive CPU-Zeit. Beide Messarten mit leerem Messrahmen und wiederholten Durchläufen kalibrieren. Espressif empfiehlt solche gezielten Messungen und beschreibt Cache-/IRAM-Optimierungen: [Speed Optimization für ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-guides/performance/speed.html).

### Datenlayout und Speicher

`NodeInstance` enthält weiterhin Zustände vieler nicht verwendeter Node-Typen. Renderdaten wie Phase, Zeiger und Koeffizienten von Presetdaten, Mappinglisten und IDs trennen. Typbezogene Pools oder kleine Runtime-Header können internen RAM und Cachewege sparen. Nicht blind sämtliche Daten nach PSRAM verschieben: Der Code dokumentiert bereits negative Erfahrungen mit einem solchen Versuch.

`Graph` hält große Arrays für maximal mögliche Kanten und einen VoiceAllocator für jedes Widget, obwohl nur MIDI-In einen benötigt. Insbesondere dessen Mono-Notenliste wird für viele unbenutzte Slots bezahlt. Tatsächliche Kapazitäten allokieren und nur relevante Allocator-Instanzen anlegen. Das spart primär RAM; eine CPU-Wirkung muss separat gemessen werden.

ESP-Effekt-Scratch wird bevorzugt in PSRAM angelegt. Trotz eines API-Aufrufs pro Block werden darin viele Samples gelesen und geschrieben. Der Kommentar, dies sei allein wegen der blockweisen API kein Render-Budget-Risiko, ist kein Messnachweis. Kleine gemeinsam genutzte Scratch-Puffer im internen RAM sind einen Vergleich wert. Teilen ist nur erlaubt, wenn die Library nach Rückkehr keine Pufferreferenzen benötigt und keine parallelen Renderaufrufe stattfinden.

### Ereignisse und Ausgabe

In [SynthEngine.h](../main/audio/engine/SynthEngine.h) werden Live-Parameter noch im Audiotask mit cJSON geparst. MIDI- und Parameterqueues werden bis zur Leere abgearbeitet. Typisierte, bereits aufgelöste Parameterbefehle vor der Audiogrenze erzeugen; redundante Werte pro Parameter zusammenfassen. MIDI-Reihenfolge, Note-off und Sustain dürfen dabei nicht verloren gehen. Für Ereignisfluten ein begrenztes Arbeitsbudget mit definierter Weiterbearbeitung entwickeln.

Normale Graphfreigabe liegt bereits beim Loader. Der Pre-free-Pfad und der Queue-Fehlerpfad geben jedoch noch direkt im Renderloop Speicher frei. Auch diese Sonderfälle über eine vorab existierende stille Graphinstanz und den Loader abwickeln, sofern das Speicherprotokoll dies erlaubt.

Pro Sample laufen im PCM-Pfad zusätzlich Signal-Sprungmessungen für ES8311/WM8960. Sie können bei normalen Rechteckwellen anschlagen und Warnlogs erzeugen. Diese Diagnose bedarfsgesteuert machen und Logging außerhalb des Audiotasks ausführen. Den globalen Fade-Puffer im stabilen Zustand vermeiden; konstante Faktoren bei PCM-Konvertierung zusammenfassen. Notwendige stille DMA-Ausgaben dürfen dabei nicht entfallen.

Der `SoftLimiter` ruft oberhalb der Schwelle pro Sample `powf()` auf. Eine begrenzte Näherung oder Lookup-Funktion ist ein Kandidat, falls der Limiter im jeweiligen Patch tatsächlich aktiv ist. Außerdem erreicht die geglättete Verstärkung nach einer Begrenzung unter Float-Rundung möglicherweise nie wieder exakt 1; der Fast Path prüft aber `== 1.0f`. Definierte Rückkehr zur Einheit einbauen und Attack/Release sowie Overshoots vergleichen. Keine Änderung des Limiters allein zur Verbesserung eines Benchmarks.

### Compiler, DSP-Library, zwei Kerne

Performance-Optimierung ist bereits aktiviert. Kein pauschales `-ffast-math`: Im Graphen können umsortierte Summen, NaN-Behandlung und nichtlineare Rückkopplungen hörbare oder funktionale Unterschiede verursachen. Einzelne reine DSP-Kernels mit lokalen Optionen, FMA-Nutzung oder IRAM-Platzierung vergleichen und die Ausgabe prüfen.

Die offizielle ESP-DSP-Benchmarktabelle enthält P4-spezifische Ergebnisse. Beispielsweise ist der dortige Float-Biquad schneller als die ANSI-RISC-V-Referenz; das ist kein Beleg für denselben Vorteil gegenüber diesem bereits optimierten Biquad. Ein einzelner Float-Dot-Product zeigt zudem deutlich weniger Vorteil als die Integer-Variante. Daher passende Blockkernels messen, keine allgemeine SIMD-Beschleunigung für beliebigen Floatcode annehmen. Quelle: [ESP-DSP Benchmarks](https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html).

Eine größere L2-Konfiguration ist nur ein gesonderter Speicher-/Timingversuch. Auf dem P4 hängt der verfügbare RAM von der Cachegröße ab; mehr Cache kann weniger Platz für interne Audiozustände bedeuten. Quelle: [Espressif: Minimizing RAM Usage](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/performance/ram-usage.html).

Die Engine rendert derzeit auf Core 1, der Loader auf Core 0. Als späte Option unabhängige Stimmenpakete auf beide Kerne verteilen und erst an der globalen Grenze summieren. Dafür müssten Core-0-Reserve, Synchronisierung, gemeinsamer Cache/PSRAM, globale Modulation und statische Scratch-Puffer berücksichtigt werden. Das ist deutlich invasiver als die vorstehenden Optimierungen und liefert keine garantierte Verdopplung.

Größere Blöcke senken nur den blockbezogenen Overhead, nicht die Samplearbeit. Sie erhöhen zudem Steuer-/Audiolatenz. Eine niedrigere Samplerate verändert Bandbreite, Aliasing und sämtliche angebundenen Audiotakte. Beides ist für den vorliegenden Optimierungswunsch nachrangig.

## 8. Mess- und Abnahmeplan

Einen nativen C++-Benchmarkmodus mit festem MIDI-Ereignisablauf verwenden; kein Browser und keine JS-Engine als Referenz. Den Screenshot-Patch als Preset reproduzieren und Wellenformen, Tune, Hüllkurven, Verbindungen sowie effektive Stimmenzahl festhalten.

| Prüfgruppe | Varianten | Abnahmekriterium |
|---|---|---|
| WaveGen | 1/8/16/32 Stimmen, alle sechs Wellenformen, direktes Pitch, Divider /1 /2 /4, konstantes Tune-Kabel | Fast-Path-Zähler plausibel; geringere Zeit bei gleichem Signalverhalten |
| Modulation | Glide, LFO auf Pitch/Tune, Audio-FM, Wellenformwechsel, CV-Rampen über Blockgrenzen | Keine auf Blockrate eingefrorene echte Modulation, keine zusätzlichen Sprünge |
| Filter | 20 Hz bis oberer Cutoff, LP/HP/BP und Curtis-Modi, Q-Sweep, CV konstant und dynamisch, Stride 1/4/8/16 | Frequenzgang, Resonanz, Stabilität und Zustandsübergänge erhalten bzw. genehmigte Qualitätsvariante |
| Stimmenende | Release, Sustain-Pedal, Retrigger, Voice-Stealing, mehrere MIDI-Ins, lange Sampledatei, Reverb-/Delaytail | Kein Abschneiden hörbarer Tails, kein unnötiges Weiterrechnen nach sicherem Ende |
| Graph | Eine/mehrere Quellen, globale Quelle, Voice-Summe, alle Link-Busse, Monitorwechsel, Switch/Multiplexer-Fade | Identische Routingsemantik, keine ungültigen Pufferansichten |
| Effekte/SF2 | Einzelne Nodes und Ketten, lineare/komprimierende Pegel, SoundFont mit Modulation, Sonic ungleich Tempo 1 | Pegel, FIFO-Verhalten, Klang und Last separat bewerten |
| Dauerbetrieb | Screenshot-Patch und dichter Mischpatch, gehaltene Akkorde plus Noten-/CC-Wechsel, normale Systemlast | Keine neuen Unterläufe; verbesserte mittlere/P99-Zeit und ausreichender RAM |

Zunächst mindestens 30 Sekunden pro stabiler A/B-Variante, nach Auswahl längerer Lauf über mehrere Minuten. Warm- und Kaltcache sowie normale Hintergrundaktivität berücksichtigen. Der automatische Stimmen-Watchdog darf einen langsameren Kandidaten nicht durch reduzierte Stimmenzahl scheinbar schneller machen; jede Änderung der effektiven Stimmenzahl macht den Vergleich ungültig.

Qualität auf zwei Ebenen prüfen: numerische Differenz zum aktuellen Float32-Code und zur mathematischen Referenz, anschließend Spektrum/Frequenzgang sowie Hörvergleich mit identischen MIDI-Ereignissen. Bei Änderungen an Phase oder Rundung ist Bitidentität nicht immer sinnvoll; erlaubte Frequenz-, Pegel- und Artefaktabweichungen vorab festlegen. Für Pow2 wäre beispielsweise deutlich unter 0,01 Cent ein leicht prüfbares numerisches Ziel, aber kein Ersatz für Filter- oder FM-Tests.

Als vorläufiges Echtzeitziel für den vorgesehenen Patch: P99 höchstens 80 % des Blockbudgets, also ungefähr 2.322 µs, und keine Deadline-Überschreitungen im festgelegten Dauerlauf. Das ist ein vorgeschlagenes Reservenziel, keine bereits erreichte Eigenschaft und keine Garantie gegen beliebige Systemstörungen.

Nach jedem Umsetzungsschritt genau den zugehörigen Vergleich wiederholen, Ergebnis mit Firmwarekennung dokumentieren und nur bei nachgewiesenem Nutzen übernehmen. Zuerst P0/P1, danach die durchgängige Konstantenweitergabe. Die Sinustabelle wird erst nach getrennter Filter-/Hammondprüfung verkleinert.
