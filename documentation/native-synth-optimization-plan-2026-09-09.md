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
| P0 | Reproduzierbare native Baseline mit Phasen des Renderdurchlaufs, Instanzzahlen und Fast-Path-Zählern | Belastbare Zuordnung von CPU-Last und Spitzen | Klein bis mittel; zuerst |
| P1 | WaveGen pro Block spezialisieren; konstantes Tune erkennen; Phasenschritt und Verstärkung vorberechnen; Smoothing nach Konvergenz beenden | Hoher Kandidat im gezeigten Patch | Mittel; unabhängig vom großen Graphumbau |
| P1 | Chorus mit einem Ringpuffer; WavePlayer bei abgeschlossener Release stoppen; Mapping-/Fade-Konstanten cachen | Konkrete, örtlich begrenzte Einsparungen | Klein bis mittel; eigene Vergleichsfälle |
| P2 | Signalzustand `Constant`/`Buffer` durch DSP-Verbindungen propagieren; später `Ramp` | Hoher Kandidat für Pitch-/CV-reiche Patches und viele Stimmen | Größer; saubere Port- und Summierungssemantik erforderlich |
| P2 | Einfache Verbindungen ohne Summierpuffer; ungenutzte Busse entfernen; nicht benötigte Teilgraphen aus Renderplan nehmen | Weniger Speicherzugriffe, RAM und Graph-Overhead | Größer; Ownership und Graphabhängigkeiten absichern |
| P3 | Filter-Koeffizienten bei identischen CVs wiederverwenden; gemeinsame Modulation einmal vorberechnen; Block-Biquad vergleichen | Mittel bis hoch bei modulierten/mehrstimmigen Filtern | Mittel; Filterzustände bleiben pro Stimme |
| P3 | Pow2-/VCA-LUT zunächst auf je 257 Einträge reduzieren | 6.136 Byte weniger RAM; CPU-Effekt offen | Klein; numerische Prüfung und Hardware-A/B |
| P4 | SoundFont-Datenlayout, Effektkonvertierungen, Render-Metadaten und Ereignisverarbeitung optimieren | Presetabhängig; bessere Spitzenlast möglich | Mittel bis groß |
| P5 | Gezielte IRAM-/Compiler-/ESP-DSP-Versuche, erst danach optional Aufteilung auf zwei Kerne | Nur bei verbleibenden gemessenen Engpässen | Hoch; keine pauschalen Beschleunigungsversprechen |

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
| WaveGen / Oscillator, alle sechs Wellenformen | Blockweise spezialisierte Schleifen, konstantes Pitch/Tune, vorberechneter Phasenschritt, konvergiertes Smoothing stoppen; Noise überspringt Phase bereits | Sehr hoch |
| Hammond | Konstante Pitch-/Tune-Puffer erkennen; neun Phasenschritte nur bei Frequenz-/Ratioänderung bilden; im modulierten Pfad Vorberechnung und aktive Drawbars stärker nutzen; stille Drawbar-Phasen weiterführen | Hoch bei Orgelpatches |
| LFO | Sinus nutzt bereits Blockinterpolation; globale Kopien durch gemeinsame Pufferansicht ersetzen; Dreieck/Sägezahn zwischen Knicken als Rampen behandeln, Kanten/Sync erhalten; One-Shot-Endwert direkt halten | Mittel |
| Clock | Pulswechsel über Samplezähler bzw. Grenzpositionen vorberechnen, konstante Abschnitte ausgeben; gebrochene Perioden mit Restakkumulator erhalten, sonst Tempo-Drift | Niedrig bis mittel |
| Detune | Konstante Ein-/Ausgänge als Skalare; geglätteten Betrag und beide Offsets einmal vorbereiten; laufende Glide-Rampen erhalten | Mittel |
| Frequency Divider | Oktavabstand bereits gecacht; konstanten Ausgang deklarieren; Fast Path muss auch laufende Control-Rampen berücksichtigen | Sehr hoch für nachfolgende Generatoren |
| ADSR | Sustain und Idle als konstante Ausgänge; Rampen abschnittsweise rendern; Velocity*10 einmal vorberechnen; Timingkonfiguration bei unverändertem CV nicht erneut setzen | Mittel |
| Filter LP/HP/BP | Unveränderte CV-Werte trotz Audio-Kabel erkennen; nur geänderte Cutoff-/Q-Abbildung berechnen; bei gemeinsamem Modulationssignal Koeffizientenfolge einmal pro Widget bilden | Hoch bei Modulation |
| CurtisFilter LP24/LP12/BP12/HP12 | `% stride` durch Zähler ersetzen, Modus außerhalb Sample-Schleife wählen; Koeffizienten cachen; drei Sättigungen pro Sample sind wesentlicher DSP-Aufwand, Reduktion verändert Klang | Hoch bei Curtis-Patches |
| Gain/VCA linear/exponential | Kurve und Modulationsart einmal auswählen; Smoothing nach Konvergenz stoppen; konstante Gains vorberechnen; bei Global gemeinsame Gain-Folge nutzen, Audio bleibt pro Stimme | Mittel |
| Mixer | Aktive Eingänge und Gains einmal vorbereiten; Null-Gain-Eingänge nicht lesen; zusammenhängende Add-/Multiply-Schleifen statt wiederholter Portauswahl prüfen | Mittel |
| Adder / Multiplier / Divide / Abs | Skalar-/Rampeneigenschaften weitergeben; konstante Nenner beim Divide durch gecachten Kehrwert behandeln; bisherige Nullsemantik erhalten | Mittel |
| ValueMapper | Steigung/Offset oder inversen Eingangsbereich bei Paramänderung cachen; im Samplepfad steht aktuell eine Division durch denselben Bereich | Mittel |
| Switch | Gewählten Eingangszeiger einmal wählen; bei passender Ownership direkt weiterreichen | Niedrig bis mittel |
| Multiplexer | Ohne Fade Pufferansicht; während Fade Kehrwert der Fade-Länge cachen statt pro Sample dividieren; nach Fade Rest des Blocks direkt kopieren | Mittel |
| Const | Renderfunktion ist bereits leer und Wert wird per ControlRoute verteilt; unbenutzten Output-Puffer und Renderdispatch einsparen | Niedrig |
| Knob / Button / UpDownButton | Aktuell werden konstante Output-Blöcke gefüllt; durch Skalar-Ausgänge ersetzen; Mapping bleibt ereignisgesteuert | Niedrig einzeln, mittel als Graphverbesserung |
| MIDI-In / MIDI-Split | Kein eigener DSP-Kernel. Routen nach Quelle, Kanal/CC und Ziel vorbereiten; VoiceAllocator nicht als primären Dauerton-Hotspot behandeln | Niedrig für gehaltene Noten |
| WavePlayer | Nach Gate-off und vollständig abgeklungenem 20-ms-Fade `playing=false` setzen; derzeit laufen Sample-Lesezugriffe bis Dateiende weiter. Restblock beim Ende zusammenhängend nullen. Pitchratio cachen; langer Float-Positionsakkumulator separat auf Präzision prüfen | Hoch für lange Samples |
| SoundFontPlayer / TinySoundFont | Voice-/Filterzustände bevorzugt intern, Samplebank weiter PSRAM; derzeit führt der Allocation-Hook grundsätzlich zuerst nach PSRAM. Interleaved-zu-Planar-Kopie und nachfolgenden Gain-Durchlauf zusammenlegen; native getrennte Ausgabe prüfen | Hoch bei SF2-Patches |
| Chorus | Beide DelayLines bekommen dasselbe Mono-Sample und haben dieselbe Länge: ein gemeinsamer Ring mit zwei interpolierten Lesetaps erhält das Verfahren und spart einen Schreibstrom sowie etwa 17,6 KiB pro Instanz. LFO-Schritt, Offset/Depth in Samples vorberechnen | Hoch, klar begrenzt |
| Delay | Sinus/Cosinus des Mix nur bei Änderung berechnen; Zeit und Feedback vorbereiten; Ringzugriff in zusammenhängenden Abschnitten prüfen; auch bei Mix=0 Tail-/Wiederaktivierungssemantik erhalten | Mittel |
| Reverb / ReverbLight | RoomSize/Damping werden bei angeschlossenen CVs jeden Block neu abgeleitet, auch bei gleichem Wert. Änderungen erkennen und beide Ableitungen entkoppeln. Speicherzugriffe der Ringe messen; Tails beim Abschalten beachten | Mittel bis hoch |
| EspEq | Pro Node zwei Float/S32-Umwandlungen; einzelne Standardfilter gegen native Float-Variante vergleichen, zusammenhängende ESP-Effektketten möglichst im selben Format halten | Mittel |
| EspAlc | Statische Kanalverstärkung im Floatgraph gegen Library plus Konvertierungen vergleichen; die konkrete Library-Gain-/Clipping-Semantik erhalten | Mittel |
| EspChannelConvert | Gewichtete Kanalsumme direkt in Float prüfen; aktuelle Konvertierungen und Kopie des Monokanals verursachen Zusatzdurchläufe | Mittel |
| EspMixer | Fade-Verfahren im Floatgraph vergleichen oder Konvertierungen zwischen benachbarten ESP-Nodes vermeiden; nicht auf einfache statische Summe reduzieren | Mittel |
| EspSonic | Variable Ein-/Ausgabemengen benötigen korrekte Pufferung. Zunächst `in_num`/`out_num`, Verbrauch, Restdaten und FIFO-Verhalten prüfen; erst dann Interleave-/Konvertierungsschleifen optimieren | Korrektheit vor Performance |
| EspDrc / EspMbc | Library-Kernel separat von Formatkonvertierung messen; gemeinsame S32-Ketten und internen Scratch testen; Algorithmus/Lookahead nicht stillschweigend ändern | Mittel, presetabhängig |
| AudioOutputLink / AudioInputLink | Nur belegte der acht Mono-Busse bearbeiten; unveränderten Gain 1 durchreichen; Kopieren und Multiplikation zusammenfassen; Voice-Summe nur an vorgesehener Grenze | Mittel |
| AudioOut / UsbOut / Es8311Out / Wm8960Out | Gleicher nativer Output-Kernel: konstante Kanaltrims vorberechnen, konvergierte Mute-Rampe stoppen, Limiter prüfen; aktive Senken bei Kopieren/PCM-Konvertierung berücksichtigen | Mittel |
| VuMeter / Oscilloscope / Spectrum | Im nativen `processNodeBlock()` keine eigene FFT-/Messberechnung; Verkabelung erzeugt dennoch Puffer- und Summierarbeit. Native Taps nur bei Bedarf transportieren. Externe Anzeigeimplementierung wurde nicht untersucht | Niedrig bis mittel |
| Unknown | Kein DSP; unbekannte/fehlgeschlagene Nodes nicht als nutzlose Renderarbeit führen | Niedrig |

Quellen für die Detailtabelle: [nodes](../main/audio/engine/nodes), [NodeRender.h](../main/audio/engine/NodeRender.h), [SoundFontPlayer.h](../main/audio/engine/SoundFontPlayer.h), [TinySoundFont](../components/tiny_soundfont/include/tsf.h), [ESP-Effektadapter](../main/audio/engine/EspAudioEffectsAdapter.h), [zusätzliche ESP-Effekte](../main/audio/engine/EspAdditionalEffects.h).

### Filter: gemeinsam vorberechnen ist nicht gemeinsam filtern

Der Global-Schalter von Filter und Gain teilt im aktuellen Code hauptsächlich die Modulationsquelle. Er reduziert nicht automatisch die Anzahl der Audio-Instanzen. Identische Koeffizienten kann man gemeinsam vorberechnen, während die Filterzustände pro Stimme getrennt bleiben.

Ein einzelner Filter hinter der Stimmensumme wäre nur für lineare Filter mit identischen zeitabhängigen Koeffizienten und kompatiblen Zustands-/Resetregeln mathematisch entsprechend. Unterschiedliche Hüllkurven pro Stimme, individuelle Resonanzen und insbesondere Curtis-Sättigung schließen eine automatische Zusammenlegung aus. Effekte hinter dem Link können als bewusste Patchgestaltung viel CPU sparen, verändern aber gegebenenfalls den Klang.

Für langsame Modulation kann ein größerer Koeffizientenabstand mit geglättetem Verlauf geprüft werden. Bei echter Audio-FM weiterhin volle Auflösung anbieten. Koeffizienten nicht beliebig interpolieren, ohne Stabilität und Resonanzverhalten zu prüfen.

## 6. Korrektheitsbefunde, die Optimierungen beeinflussen

Diese Punkte sind im Quellcode sichtbar, wurden aber nicht auf Hardware reproduziert. Sie sollten als getrennte Änderungen behandelt werden, damit Klangänderungen nicht irrtümlich einer CPU-Optimierung zugeschrieben werden.

1. **Glide durch CV-Rechennodes:** Graph setzt `controlRampPortMask`, aber unter anderem Frequency Divider, Detune, ValueMapper und Arithmetic-Fast-Paths prüfen nur `audioConnectedPortMask`. Ein direktes MIDI-Glide kann daher am ersten Rechennode auf Sample 0 des Blocks reduziert werden. Die neue Konstanzlogik muss beide Signalarten berücksichtigen. Im Screenshot ist Glide aus; dieser Punkt erklärt dort keine Zusatzlast.
2. **Curtis LP24:** `processStage(s, 2, stage2)` liefert den Ausgang der dritten Stufe zurück, dieser wird verworfen. Die vierte Stufe erhält stattdessen `s.stage[2]`, also den aktualisierten Integratorzustand. Gegen das beabsichtigte TPT-Modell prüfen, bevor Frequenzgänge als Referenz für Optimierungen eingefroren werden.
3. **ESP-Effekt-Pegel:** Die Engine führt Audio nominell in ±5 V, `floatToS32()` sättigt aber bereits bei ±1 und `s32ToFloat()` liefert normalisierte Werte zurück. In den untersuchten Adaptern fehlt eine explizite Umrechnung zwischen beiden Bereichen. Das kann Clipping und andere Pegel verursachen. Vor Kernel-/Formatvergleichen normalisierte und CV-Audiodomäne sauber abgleichen.
4. **Stimmenruhe ist graphweit nach Slotnummer:** `voiceStillSounding[v]` wird über alle verbundenen ADSRs gebildet, ohne eigene Domäne pro MIDI-In oder Signalzweig. Eine lange Modulations-ADSR kann damit andere Zweige desselben Slots weiterlaufen lassen. Ein Sampleplayer oder Effekt benötigt umgekehrt eigene Regeln für sein Ende. Aktivität nach Abhängigkeiten und Tails modellieren, nicht allein anhand irgendeiner ADSR oder eines momentanen Nullsamples.

## 7. Engine, Echtzeit und Speicher

### Profiling korrekt auswerten

In [Graph.h](../main/audio/engine/Graph.h) liegt der Node-Timer um die Renderaufrufe aller Instanzen eines Widgets. Eingangspuffer vorbereiten und ausgehende Verbindungen summieren liegen außerhalb. Pro Block wird nur ein Widget vermessen und anschließend geglättet. Die Widgetanzeigen sind deshalb weder eine vollständige Kostenaufteilung noch exakt gleichzeitige Messungen. Das erklärt auch, weshalb ein Monitor 0,0 % zeigen kann und trotzdem Transportarbeit verursacht.

Zusätzlich je Messfenster getrennt erfassen:

- MIDI-/Parameterverarbeitung, Aktivitätsermittlung, Pufferinitialisierung.
- Node-DSP insgesamt, Kabel-Summierung, Outputkopien und PCM-Konvertierung.
- Anzahl aktiver, tatsächlich gerenderter und trotz Ruhe gerenderter Instanzen.
- Konstante/modulierte Pitch- und Tune-Blöcke, Hz-Berechnungen, Filter-Koeffizientenupdates.
- Mittlere Renderzeit, P95/P99, Maximum, Deadline-Überschreitungen und DMA-Unterläufe soweit verfügbar.

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
