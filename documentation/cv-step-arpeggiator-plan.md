# Plan: CV Step-Arpeggiator Node

## Ziel

Ein neuer monophoner Node kombiniert einen 8/16-Step-CV-Sequencer mit dem Verhalten eines einfachen Arpeggiators. Externe analoge Clock und Run/Reset-Gate steuern die Wiedergabe. Der Node liefert eine Tonhoehen-CV und einen Gate-Puls pro neuem Step.

## Funktionsumfang

### Anschluesse

Eingaenge:

- `clock`: externes analoges Clock-Signal; steigende Flanke bei der bestehenden Gate-Schwelle loest einen Step aus.
- `gate`: Run/Reset-Signal. Eine steigende Flanke setzt den Sequencer auf Step 1. Nur bei Gate High werden Clock-Impulse verarbeitet. Bei Gate Low bleibt die zuletzt ausgegebene CV erhalten und es werden keine neuen Steps oder Gate-Pulse erzeugt.
- `pitch`: additive Basis-Tonhoehe nach 1 V/Oktave.
- `direction`: CV-gesteuerte Laufrichtung.
- `step1` bis `step16`: je ein analoger Wert fuer die Tonhoehe eines Steps.

Ausgaenge:

- `cv`: gehaltene Tonhoehen-CV des aktuellen Steps.
- `gateOut`: Gate-Puls mit 10 V bei jedem neuen Step.

### Step-Werte und CV-Prioritaet

Jeder der 16 Steps besitzt einen kleinen lokalen Drehregler sowie einen eigenen analogen Eingang.

- Ohne Kabel am Step-Port verwendet der Node den lokalen Knob-Wert.
- Mit Kabel am Step-Port uebernimmt der Node ausschliesslich die anliegende Eingangsspannung; der Knob-Wert wird ignoriert.
- Die Tonhoehen-CV ist `aktiver Step-Wert + pitch`.
- Die Step-Werte sind die spielbaren Arpeggio-Tonhoehen; es gibt keine MIDI-Notenliste oder zusaetzliche Arpeggio-Notenquelle.

### Steps und Laufrichtung

Ein Switch bestimmt, ob 8 oder 16 Steps aktiv sind. Die Werte aller 16 Steps bleiben beim Umschalten gespeichert; Steps 9 bis 16 werden im 8-Step-Modus nicht abgespielt.

`direction` wird im Bereich 0 bis 10 V in vier feste Bereiche eingeteilt:

| CV-Bereich | Modus | Verhalten |
| --- | --- | --- |
| 0 bis <2.5 V | Forward | Step 1 bis letzter aktiver Step, dann Wrap zu Step 1. |
| 2.5 bis <5 V | Backward | Letzter aktiver Step bis Step 1, dann Wrap zum letzten Step. |
| 5 bis <7.5 V | Ping-Pong | Vorwaerts und rueckwaerts ohne doppelte Wiedergabe der Endpunkte. |
| 7.5 bis 10 V | Random | Pro Clock-Flanke zufaelliger aktiver Step; unmittelbare Wiederholungen sind erlaubt. |

Bei einer Aenderung von Step-Anzahl oder Richtung wird der aktuelle Index sicher auf den aktiven Bereich normalisiert.

### Clock, Run/Reset und Gate-Ausgabe

- Clock- und Run/Reset-Signale verwenden die bestehende Gate-Konvention von 0 bis 10 V mit 5 V Schwelle.
- Die steigende Flanke von `gate` startet auf Step 1 und beendet gegebenenfalls einen noch laufenden Ausgangspuls.
- Eine steigende Flanke von `clock` waehrend Gate High gibt den aktuellen Step aus, startet `gateOut` und waehlt anschliessend den folgenden Step.
- Clock-Flanken waehrend Gate Low werden ignoriert und nicht nachgeholt.
- Die Pulslange von `gateOut` ist in Millisekunden einstellbar, analog zum vorhandenen Clock-Node.

## Implementierung

1. Sequencer-Typ in `main/audio/engine/NodeTypes.h` und den bestehenden Factory-, Typ-Mapping- und JSON-Serialisierungsstellen registrieren.
2. Port-IDs und -Anzahlen in `main/audio/engine/PortTables.h` nach lokaler Konvention ergaenzen.
3. Neue Node-Implementierung in `main/audio/engine/nodes/` anlegen. Der Laufzeit-State umfasst aktuellen Step, Ping-Pong-Richtung, vorherige Clock- und Gate-Zustaende, Gate-Pulszaehler und PRNG-State fuer Random.
4. Die vorhandene `portSample()`-Logik aus `main/audio/engine/NodeInstance.h` fuer jeden Step nutzen: verkabelter Input-Port hat Vorrang vor dem lokalen Control-/Knob-Wert.
5. Gate-Flankenerkennung nach den Vorbildern in `WavePlayerNode.h`, `DxEnvelopeNode.h` und `AdsrNode.h` implementieren.
6. Samplegenaues Gate-Puls-Timing am Muster von `ClockNode.h` ausrichten.
7. Parameter-Dispatch und Live-Updates fuer Step-Anzahl, Pulsdauer und lokale Step-Werte in `main/audio/engine/ParamDispatch.h` und der bestehenden Graph-Update-Kette ergaenzen.
8. Web-Editor in `data/static/js/main.js` integrieren: Node-Kategorie, Audio-Port-Definitionen, Inspector-Parameter und spezialisierter Node-Renderer.
9. Renderer: 16 kleine runde Drehregler dicht nebeneinander an der Unterkante. Direkt unter jedem Knob stehen formatierte Spannung und die entsprechende Note. Der jeweilige Step-Eingang ist ebenfalls unten beim zugehoerigen Knob. Clock, Gate, Pitch und Direction bleiben klar getrennt; `cv` und `gateOut` liegen an der vorhandenen Ausgangsseite. Im 8-Step-Modus werden die letzten acht UI-Schritte deaktiviert oder ausgeblendet, ohne ihre Werte zu verlieren.
10. Note/CV-Anzeige aus der bestehenden Pitch-CV-Konvention ableiten: 1 V/Oktave, C4 bei 5 V. Die Anzeige rundet auf die naechstliegende chromatische Note und zeigt beispielsweise `5.00 V` und `C4`. Sie zeigt den Step-Wert, nicht den globalen Pitch-Offset.

## Tests und Abnahme

- Lokaler Knob wird ohne Kabel genutzt; ein belegter Step-Port ueberschreibt ihn vollstaendig.
- Gate-Rising-Edge setzt auf Step 1; Gate Low haelt CV und blockiert Clock-getriebene Steps.
- Clock-Rising-Edges, Gate-Pulsdauer und Pitch-Addition funktionieren samplegenau nach bestehender Node-Konvention.
- Forward, Backward, Ping-Pong, Random, 8-Step- und 16-Step-Grenzen testen; Random darf Wiederholungen erzeugen.
- Im Graph-Editor alle unteren Step-Knobs, Spannungs-/Notentexte und Step-Ports auf Desktop und schmaler Ansicht pruefen.
- ESP-IDF-Build nach dem Laden von `C:\esp\v6.1-rc1\esp-idf\export.ps1` mit `idf.py build` ausfuehren.

## Nicht Bestandteil des ersten Umfangs

Keine Quantisierung oder Skalenauswahl, keine per-Step-Gates, Ratcheting, Swing, Pattern-Speicher, Transport-Start/Stop oder MIDI-Notenlisten-Arpeggiator.
