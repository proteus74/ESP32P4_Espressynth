# Implementierungsplan für virtuelle Audio-Links

Stand: 10. September 2026. Dieser Plan beschreibt zwei neue, frei platzierbare Widgets für unsichtbare Audio-Verbindungen innerhalb eines Presets. Es wurden noch keine Änderungen an Firmware oder WebUI umgesetzt.

## 1. Ziel

Zwei neue Widgets ergänzen den Audio-Graph:

- **Virtueller Ausgang:** besitzt einen sichtbaren Mono-Audioeingang und sendet dieses Signal über einen benannten Link weiter.
- **Virtueller Eingang:** besitzt einen sichtbaren Mono-Audioausgang und gibt das Signal eines ausgewählten virtuellen Ausgangs aus.

Damit lassen sich längere oder seitenübergreifende Kabel durch eine logische Verbindung ersetzen. Die vorhandenen acht festen Busse `audio-output-link` und `audio-input-link` bleiben unverändert, damit bestehende Presets kompatibel bleiben.

## 2. Empfohlene Semantik

- Beide neuen Widget-Typen sind mehrfach platzierbar und auf den Seiten "Sound Generation" sowie "FX & Output" verfügbar.
- Ein virtueller Ausgang definiert einen Link. Sein Name muss innerhalb des Presets eindeutig sein.
- Ein virtueller Eingang wählt in seinen Eigenschaften einen vorhandenen virtuellen Ausgang aus.
- Intern erfolgt die Zuordnung über eine unveränderliche `linkId`, nicht über den sichtbaren Namen. Umbenennen unterbricht dadurch keine Verbindung.
- Ein virtueller Ausgang darf mehrere virtuelle Eingänge speisen. Das entspricht einem Audio-Fan-out ohne zusätzliche Pegeländerung.
- Ein virtueller Eingang kann jeweils genau einen virtuellen Ausgang auswählen.
- Nicht verbundene oder nicht mehr auflösbare virtuelle Eingänge liefern Stille.
- Die Verbindung ist latenzfrei auf Blockebene und wird beim Graphaufbau wie eine normale gerichtete Audio-Kante behandelt.
- Die erste Ausbaustufe ist mono. Stereo wird bei Bedarf mit zwei getrennten, benannten Links aufgebaut.

## 3. Begriffe und Typnamen

Vorgeschlagene Preset-Typen:

- `virtual-output`: sichtbarer Eingang `in`, kein sichtbarer Ausgang
- `virtual-input`: kein sichtbarer Eingang, sichtbarer Ausgang `out`

Die Bezeichnungen folgen der Sicht des Patch-Kabels: Der virtuelle Ausgang nimmt das lokale Signal entgegen, der virtuelle Eingang stellt es an einer anderen Stelle wieder bereit.

## 4. Datenmodell

Parameter eines virtuellen Ausgangs:

```json
{
  "linkId": "link-a7f32",
  "linkName": "Drums"
}
```

Parameter eines virtuellen Eingangs:

```json
{
  "sourceLinkId": "link-a7f32"
}
```

Regeln:

- `linkId` wird beim Erstellen einmal generiert und danach stabil gespeichert.
- IDs müssen kurz genug für die statischen Firmware-Strukturen sein und innerhalb eines Presets eindeutig bleiben.
- `linkName` wird getrimmt, darf nicht leer sein und wird ohne Beachtung der Groß-/Kleinschreibung auf Eindeutigkeit geprüft.
- `sourceLinkId` darf leer sein. Dieser Zustand bedeutet "Nicht verbunden".
- Beim Kopieren eines kompletten Sender-/Empfänger-Verbunds werden neue Link-IDs erzeugt und interne Referenzen gemeinsam angepasst.

## 5. WebUI

### 5.1 Widget-Definitionen und Palette

In [main.js](../data/static/js/main.js) werden beide Widget-Typen mit ihren Mono-Ports in `WIDGET_TYPES` und `AUDIO_PORTS` ergänzt. Beide Typen kommen in eine passende Palette-Kategorie und in `DUAL_PAGE_TYPES`.

Sie dürfen nicht in `LINK_WIDGET_TYPES` aufgenommen werden. Diese Liste erzwingt für die vorhandenen Seitenbusse Singleton-, Lösch- und Migrationsregeln, die für die neuen frei platzierbaren Links falsch wären.

### 5.2 Eigenschaften

Da die WebUI keinen allgemeinen Eigenschaften-Inspector besitzt, wird das bestehende Muster widget-spezifischer Einstellungsdialoge verwendet:

- Doppelklick auf einen virtuellen Ausgang öffnet ein Textfeld für den eindeutigen Link-Namen.
- Doppelklick auf einen virtuellen Eingang öffnet ein Dropdown mit allen vorhandenen virtuellen Ausgängen.
- Dropdown-Einträge zeigen den Namen; gespeichert wird die zugehörige `linkId`.
- Ein fehlendes Ziel bleibt als "Nicht verbunden" beziehungsweise "Fehlender Link: <ID>" sichtbar.
- Der technische Status oder gewählte Link-Name wird kompakt im Widget angezeigt.

Dialog-Markup kommt nach [index.html](../data/index.html), Styling nach [style.css](../data/static/css/style.css). Zustände, Öffnen/Speichern, Validierung, Undo/Redo und Preset-Parameter werden in [main.js](../data/static/js/main.js) umgesetzt.

### 5.3 Browser-Audio-Vorschau

[audio-engine.js](../data/static/js/audio-engine.js) erhält für beide Typen je einen Mono-`GainNode` als transparente Audio-Schnittstelle. Eine Link-Map ordnet `linkId` dem virtuellen Ausgang zu und verbindet ihn mit allen Empfängern, deren `sourceLinkId` passt.

Die Browser-Verkabelung muss nach folgenden Aktionen neu aufgebaut werden:

- Widget erstellen oder löschen
- Link-Zuordnung ändern
- Preset laden
- Undo/Redo
- Kopieren und Einfügen

Die bestehende `_wireLinkBridge()`-Logik für das feste achtkanalige Seiten-Link-Paar bleibt separat bestehen.

## 6. Firmware und GraphBuilder

### 6.1 Typen, Parameter und Ports

- [NodeTypes.h](../main/audio/engine/NodeTypes.h): `VirtualOutput` und `VirtualInput` ergänzen und die beiden JSON-Typnamen zuordnen.
- [PresetParser.h](../main/audio/engine/PresetParser.h): statische Felder für `linkId`, `linkName` und `sourceLinkId` ergänzen.
- [PresetFill.h](../main/audio/engine/PresetFill.h): Link-Parameter aus dem Preset lesen.
- [PortTables.h](../main/audio/engine/PortTables.h): Mono-Eingang des virtuellen Ausgangs und Mono-Ausgang des virtuellen Eingangs registrieren.
- Eine kleine neue Node-Implementierung oder eine klar getrennte Erweiterung von [LinkNodes.h](../main/audio/engine/nodes/LinkNodes.h) stellt transparentes Passthrough bereit.

Die sichtbaren Ports entsprechen nicht vollständig den internen Ports: Der virtuelle Ausgang benötigt intern einen Quellpuffer für die versteckte Kante, der virtuelle Eingang intern einen Zielport für diese Kante.

### 6.2 Link-Auflösung

Im [GraphBuilder.h](../main/audio/engine/GraphBuilder.h) wird nach der Verarbeitung normaler Preset-Verbindungen eine Tabelle aller virtuellen Ausgänge aufgebaut. Anschließend wird jeder virtuelle Eingang anhand seiner `sourceLinkId` aufgelöst.

Für jede gültige Zuordnung:

1. versteckte Audio-Kante vom virtuellen Ausgang zum virtuellen Eingang anlegen,
2. Eingangs-Portmaske des Empfängers aktualisieren,
3. Widget-Abhängigkeit für die topologische Render-Reihenfolge markieren,
4. dieselben Regeln für Voice-Instanzen verwenden wie bei normalen Audio-Kanten.

Nicht verwendete Links erhalten keine versteckte Kante und verursachen dadurch keine unnötige Eingangspuffer-Allokation.

Eine Änderung der Link-Zuordnung ist eine Topologieänderung. Sie wird nicht als einfacher Live-Parameter behandelt, sondern über den normalen Preset-/Graph-Neuaufbau aktiviert.

## 7. Validierung und Fehlerfälle

- Leerer oder doppelter Ausgangsname: Speichern im Dialog verhindern und verständliche Meldung zeigen.
- Doppelte `linkId`: beim Laden deterministisch nur die erste Definition verwenden, Warnung protokollieren und betroffene Empfänger nicht stillschweigend falsch verbinden.
- Unbekannte `sourceLinkId`: Empfänger bleibt stumm; Preset bleibt ladbar.
- Gelöschter Ausgang: zugehörige Empfänger behalten ihre Referenz und zeigen den fehlenden Link an, bis der Benutzer neu zuordnet.
- Versteckter Zyklus: beim Graphaufbau erkennen und die verursachende virtuelle Verbindung mit Warnung ablehnen. Ein impliziter, von der Oberfläche nicht sichtbarer Feedbackpfad darf nicht von zufälliger Render-Reihenfolge abhängen.
- Maximale Widget-/Kantenanzahl: versteckte Link-Kanten müssen bei `kMaxRuntimeEdges` berücksichtigt werden. Obergrenze ist die Anzahl virtueller Eingänge, nicht nur die Anzahl unterschiedlicher Ausgänge.
- Teilgraph-Pruning und Buffer-Views müssen die versteckten Kanten wie normale Audio-Kanten sehen.

## 8. Umsetzungsreihenfolge

1. Preset-Schema und neue Typen in WebUI und Firmware definieren.
2. Widgets, Zustände, Palette und Einstellungsdialoge implementieren.
3. Speichern/Laden, Undo/Redo sowie Copy/Paste einschließlich ID-Neuvergabe fertigstellen.
4. Browser-Audio-Link-Map implementieren und interaktiv testen.
5. Firmware-Parser, Ports und transparentes Node-Rendering ergänzen.
6. Versteckte Kanten und Abhängigkeiten im GraphBuilder auflösen.
7. Validierung, Warnungen und Zyklusbehandlung ergänzen.
8. Build, Browser-Test und Hardware-Hörtest durchführen.

## 9. Abnahmekriterien

- Ein Signal kann innerhalb derselben Seite über ein Link-Paar übertragen werden.
- Ein Signal kann zwischen beiden Seiten in beide Richtungen übertragen werden.
- Mehrere Empfänger können denselben Ausgang gleichzeitig hören.
- Umbenennen eines Ausgangs unterbricht bestehende Empfänger nicht.
- Speichern und erneutes Laden erhält Namen und Zuordnungen.
- Undo/Redo erhält eine konsistente Link-Topologie.
- Kopieren eines vollständigen Link-Verbunds kollidiert nicht mit dem Original.
- Löschen eines Ausgangs erzeugt an Empfängern Stille und einen sichtbaren Fehlerstatus.
- Alte Presets und die vorhandenen acht Seitenbusse funktionieren unverändert.
- Browser-Vorschau und ESP32-Ausgabe zeigen dasselbe Routingverhalten.
- Ein vollständiger Build mit `C:\esp\v6.1-rc1\esp-idf\export.ps1` und anschließend `idf.py build` läuft erfolgreich durch.

## 10. Bewusst nicht Teil der ersten Version

- Stereo-Link als einzelnes Widget
- Mehrere Quellen pro virtuellem Eingang oder automatisches Summieren
- Gain-, Mute- oder Panorama-Regler im Link
- Link-Zuordnung während laufender Firmware ohne Graph-Neuaufbau
- Preset-übergreifende oder netzwerkweite Links