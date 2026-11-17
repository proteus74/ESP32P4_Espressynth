# Projektvideo „Build your own synthesizer" – Arbeitsstand

Notizen zum Video `Espressynth-Build-Your-Own-Synthesizer.mp4` in diesem Ordner: was festgelegt
ist, wie es gebaut wird und wo man weitermacht.

Stand: 17.11.2026, neunter Schnitt, ca. 8:50 min, 1920×1080, 30 fps.

## Festgelegt

**Sprache und Ton**

- Alles auf Englisch: Folien, Sprecher, Einblendungen.
- Aufhänger steht im Vordergrund: „Build your own synthesizer". „Moog" kommt im Titel nicht vor
  (Markenname); „70s analog" / „80s FM" als Umschreibung ist in Ordnung.
- Sprecher: Stimme `chris`, Modell `elevenlabs/eleven-v4` über OpenRouter.
- Hintergrundmusik im 70er/80er-Analogstil, wird von `music.js` erzeugt (keine fremde Musik).

**Aussagen, die drin sein müssen**

- Das MIDI-Keyboard ist das Frontpanel: alles wird über dessen Knöpfe, Fader und Pads gesteuert.
  Der Browser ist nur für das einmalige Patchen da.
- Ein Display ist nicht nötig, es macht die Bedienung nur bequemer.
- ESP32-C6 für WLAN/Bluetooth gibt es nur „on boards that have one".
- Vier Boards laufen ohne Konfiguration; ein eigenes Board richtet man über Kconfig ein.
- Man braucht ein ESP32-P4-Board und ein **USB-MIDI-Keyboard** (nicht „USB-Kabel").
- Flashen über die englische Seite `jolabi.de/en/projects/espressynth/flash`, mit markierter
  Klickstelle.
- Keine Folie „Months of work" mehr (auch kein „three months", „vibe-coded", „one-person project").
- Die Hardware kostet etwa 20 Dollar (auf „What is Espressynth?" und „Ready to build yours?").
- MIDI-Learn ist ein Kernstück und wird live gezeigt: CC-Slider anlegen, „Learn", Knopf am
  Keyboard drehen, Slider auf den Filter-Cutoff verkabeln, Knopf steuert den Filter.
- Globale MIDI-Steuerung wird gezeigt: Preset vor/zurück, Master-Tempo, Master-Lautstärke.
- Bluetooth-MIDI-Keyboards haben eine eigene Folie.
- Ein Schema zeigt die Verbindungen: USB- und Bluetooth-Keyboard → Board → Lautsprecher,
  Browser ↔ Board über WLAN oder Netzwerkkabel.
- Die Patch-Folie („A playable patch in one minute") zeigt einen kompakten Patch, bei dem man
  den Kabeln folgen kann: `video-src/patch.png`, ein Bild aus Demo A.
- In der MIDI-Learn-Demo ist ein kleines MIDI-Keyboard eingeblendet (eine Oktave, vier Knöpfe):
  Tasten leuchten mit den gespielten Tönen, der erste Knopf dreht sich mit dem Controller.
  Gezeichnet in `build5.py` (`Demo.keyboard`), Töne und Knopfweg stehen dort unter `'keyboard'`.
- In allen Demos stehen die Kabel auf **Orthogonal** (setzt `demo-init-real.js` vor dem Laden
  der Seite). Die Kabel-Demo wechselt Normal → Sag Mode → Direct → zurück auf Orthogonal.
- Layer: in der MIDI-Learn-Demo wird am CC-Slider „Add layer" geklickt (Reiter „Layer 2"),
  dazu ein Stichpunkt auf der MIDI-Folie („same knobs and buttons, different jobs").
- „Show Oscilloscopes" (View-Menü) wird am Ende von Demo C eingeschaltet.
- Die Display-Folie zeigt vier echte Bilder: Regler-Karten einer Seite, Hüllkurve des ADSR beim
  Drehen, Pages-Auswahl, Preset-Liste. Dafür baut `demo-e.js` (aus `make_demo_scripts5.py`)
  einen Patch mit vier gelernten Slidern (CC 70–73: Cutoff, Resonance, Attack, Release) und
  einem CC Button in den unbenannten Slot; `board_cc.py` dreht die Regler, `board_cli.py`
  macht die Bilder. Am Display muss dazu „Pages" → „Sound Generation" getippt werden
  (`lvgl tap 700 409`, dann `lvgl tap 250 223`).
- Kabel-Darstellung ist umstellbar (Normal, Sag Mode, Orthogonal, Direct) und wird gezeigt.
- Der Ton im Video kommt aus der JavaScript-Engine im Browser und kann knacksen; auf dem ESP32
  ist er glasklar. Steht auf der Folie „Let's build one", wird gesprochen und ist in jeder Demo
  als rotes Schild unten rechts zu sehen.

**Was bewusst fehlt**

- „Flashing from source".
- „about 11 ms" Latenz und „up to 16 voices".
- „44.1 kHz, rendered in blocks of 128 samples" (Folie „Under the hood").
- Das MDA Piano: nicht in Text, Sprecher, Widget-Wand oder Node-Picker-Bild. Für das
  Picker-Bild (`video-src/add-node.png`) wurde der Eintrag nur im Screenshot ausgeblendet.

**Bild**

- Logo `Logo.svg` dauerhaft oben rechts, zweifarbig: „Jolabi" weiß, „synth" rot (`#e5383b`).
- Demos werden im Webinterface des echten ESP32 aufgenommen, damit CPU, PSRAM und System-RAM
  echte Werte zeigen. Die Aufnahme sitzt leicht verkleinert unter einer Kopfzeile, damit das
  Logo nichts vom Editor verdeckt.
- Filter in der Bau-Demo nur kurz (Cutoff drehen), danach andere Wellenform (SuperSaw).
- FM8 mit „E.PIANO 1", Reverb und Delay auf der FX-Seite.
- Neben den Feature-Folien laufen rechts alle 69 Widgets durch – mit Rand, damit die Ports
  links und rechts sichtbar sind.
- „See what you hear" ist eine Live-Aufnahme (Oszilloskop, Spektrum, VU-Meter bewegen sich).
- „A display is optional" zeigt echte Screenshots vom Display: erst die offene Preset-Liste,
  dann die Hauptseite.

## Aufbau (achter Schnitt; Zeiten ab der zweiten Zeile jetzt etwa 10 s früher, Zeile 0:10 entfällt)

| Start | Folie |
|------:|-------|
| 0:00 | Titel: Build your own synthesizer |
| 0:10 | Months of work. |
| 0:15 | One chip. One browser. Your own synth. |
| 0:27 | What is Espressynth? (mit „about 20 dollars") |
| 0:53 | How it all connects (Schema) |
| 1:10 | A playable patch in one minute |
| 1:21 | Let's build one. (Hinweis Browser-Engine) |
| 1:37 | **Demo A:** Synth bauen – Oszillator, Filter, ADSR, Sinus, Sägezahn, Cutoff, SuperSaw |
| 2:13 | And that is just the start. |
| 2:18 | **Demo B:** FM8 (Editor, E.PIANO 1, FX-Seite), Mini Lead, TR-808 mit E-Piano |
| 3:01 | A Eurorack in your browser (Widgets: Control, Mixing/Output, Misc, External Hardware) |
| 3:19 | Add Node: right-click the canvas |
| 3:30 | From fat 70s analog bass to glassy 80s FM. |
| 3:41 | Sound sources (Widgets: Generator ohne MDA Piano, Midi) |
| 3:56 | Shape it, move it, polish it (Widgets: Filter, Effects, Modulator) |
| 4:13 | **Demo C:** Oszilloskop, Spektrum, VU-Meter live |
| 4:32 | Sequences and rhythm (Widgets: Sequencer, Math) |
| 4:45 | Your MIDI keyboard is the front panel |
| 5:03 | **Demo D, Teil 1:** MIDI-Learn – CC-Slider, Learn, Knopf drehen, auf Cutoff verkabeln, spielen |
| 5:24 | **Demo D, Teil 2:** Kabel-Darstellung umstellen |
| 5:33 | **Demo D, Teil 3:** Global MIDI Controls |
| 5:45 | No cable? Bluetooth MIDI |
| 6:05 | A display is optional (Display-Screenshots) |
| 6:18 | Connected to the real world |
| 6:34 | Presets, samples and files (File Explorer) |
| 6:44 | Under the hood |
| 7:07 | Four boards, ready to go |
| 7:27 | Ready to build yours? (mit „about 20 dollars") |
| 7:37 | Flash it from your browser |
| 7:48 | Select your board, click Flash (markierte Flash-Seite) |
| 7:56 | Your first five minutes |
| 8:14 | What will you build? |
| 8:28 | Schlusstitel |

Die Länge jeder Folie folgt dem Sprechertext; die Zeiten verschieben sich, sobald Text
geändert wird.

## Quellen: `video-src/`

Alles, was das Video erzeugt, liegt in `documentation/video-src/`: 348 MB Aufnahmen, rund
15 MB Skripte und Bilder, dazu nach einem Bau etwa 200 MB Zwischenergebnisse (WAV-Dateien,
gerenderte Folien). Der Ordner ist von Git ausgeschlossen: die `.gitignore` im Repo-Stamm nimmt `documentation/video-src/`,
`documentation/voice-samples/` und die MP4 aus. Diese Notizdatei und `Logo.svg` sind nicht
ausgeschlossen.

| Datei | Zweck |
|-------|-------|
| `slides5.js` | Die Folien: Text, Bilder, Sprechertext (`say`), Widget-Wände (`wall`), Seitenbilder (`side`), Demos (`demo`) |
| `slides5.html` | Layout der Folien; wird von Chrome ohne Fenster zu PNGs gerendert |
| `tts5.py` | Sprecher über OpenRouter; ein Clip pro Folie, nach Text-Hash in `tts/` zwischengespeichert |
| `music.js` | Erzeugt die Hintergrundmusik passend zur Gesamtlänge |
| `build5.py` | Schneidet alles: Zeitplan aus dem Sprechertext, Tonmischung, Bilder, Logo, Spektrum-Balken, ffmpeg |
| `demo5-a/`, `-b/`, `-c/`, `-d/` | Aufnahmen der vier Demos: Einzelbilder mit Zeitstempeln, `audio.wav`, `timeline.json`, `sync.json` |
| `make_demo_scripts5.py` | Erzeugt die vier Playwright-Skripte für die Demos (Abläufe stehen darin) |
| `demo-init-real.js` | Wird in die Editor-Seite eingeschleust: Töne einspeisen, ans Gerät spiegeln, Ton mitschneiden, sichtbarer Mauszeiger |
| `sync5.py` | Wandelt den Demo-Ton um und gleicht Ton und Bild ab |
| `board_backup.py` | Liest alle Presets und Einstellungen vom Board und merkt sich Prüfsummen |
| `board_session.py` | Board für eine Aufnahme vorbereiten (`unnamed`) und danach zurückstellen (`restore`) |
| `board_cli.py` | Konsolenbefehle über COM-Port ohne Reset; `shot <datei.png>` macht ein Bild vom Display |
| `gallery5/`, `gallery.js` | Die 69 Widget-Bilder und das Skript, das sie aufnimmt |
| `demo_server.py`, `demo-init.js`, `board/`, `starter_preset.json` | Editor ohne Gerät auf Port 8766, für Probeläufe und die Widget-Bilder |
| `starter_real.json` | Leerer Start-Patch für Demo A und C auf dem echten Board |
| `logo.png`, `make_logo.py`, `logo/` | Ecken-Logo und wie es aus `Logo.svg` entsteht |
| `display-*.png`, `flash-annotated.png`, `add-node.png` | Display-Screenshots, markierte Flash-Seite, Node-Picker ohne MDA Piano |

Voraussetzungen auf diesem Rechner: Python mit numpy, Pillow und pyserial, Node.js, ffmpeg,
Chrome unter `C:\Program Files\Google\Chrome\Application\chrome.exe`, Schriften Bahnschrift
(Windows) sowie Quicksand und Bauhaus 93 (nur für das Logo).

## Weitermachen

Alle Befehle im Ordner `documentation/video-src/` ausführen.

**Nur Text oder Reihenfolge ändern**

1. `slides5.js` bearbeiten. `say` so schreiben, wie es gesprochen werden soll (z. B.
   „E S P thirty-two P four", „F M eight", „U S B").
2. `python tts5.py ../openrouterapi.txt` – spricht nur geänderte Texte neu.
3. Kontrolle einzelner Bilder: `python build5.py check --stills 14,132,296` (Zeiten in Sekunden).
4. Ganzes Video: `python build5.py out/espressynth.mp4` (etwa 15 Minuten).
5. Ergebnis nach `documentation/Espressynth-Build-Your-Own-Synthesizer.mp4` kopieren.

Kosten Sprecher: der komplette Text (ca. 5.200 Zeichen) lag bei etwa 40 Cent, einzelne
geänderte Folien bei wenigen Cent. Der Schlüssel liegt in `documentation/openrouterapi.txt`
(steht in `.gitignore`).

**Demos neu aufnehmen**

Braucht das Board im Netz (zuletzt `192.168.1.135`, `COM38`) und Claude Code mit dem
Playwright-Plugin; die Skripte laufen über dessen `browser_run_code_unsafe` und müssen unter
`.playwright-mcp/vid/` im Repo liegen.

1. Sichern: `python board_backup.py 192.168.1.135 board-backup/before`
2. Skripte erzeugen: `python make_demo_scripts5.py <repo>/.playwright-mcp/vid real 192.168.1.135`,
   `demo-init-real.js` daneben kopieren.
3. Demo A: `python board_session.py 192.168.1.135 unnamed starter_real.json`, dann `demo-a.js` ausführen.
4. Demo C: wieder `unnamed`, dann `demo-c.js`.
5. `python board_session.py 192.168.1.135 restore board-backup/before`
   Demo D: wieder `unnamed`, dann `demo-d.js`. Die eine Aufnahme wird in `build5.py` in drei
   Teile geschnitten (`d1`, `d2`, `d3`), jeder mit eigenem Sprechertext.
6. Demo B: `demo-b.js` (lädt FM8, dann Moog und TR808_Drum_Machine).
7. Wieder `restore`, dann `board_backup.py … board-backup/after` und Prüfsummen vergleichen.
8. Aufnahmen aus `.playwright-mcp/vid/demo-*/` nach `demo5-*/` kopieren, `python sync5.py demo5-a demo5-b demo5-c`.

Was dabei mit dem Board passiert:

- Benannte Presets werden nicht verändert: Speicheranfragen des Browsers fängt das Skript ab.
- Nur der unbenannte Slot `presets/current.json` wird überschrieben und von `restore` zurückgespielt.
- Autosave bleibt an. Beim TR-808 legt das Gerät deshalb `TR808_Drum_Machine.json.live` an;
  `restore` löscht die Datei wieder.
- Die Töne gehen parallel ans Gerät, es spielt also hörbar mit (bei Demo D stand die
  Lautstärke auf 32 %).
- Demo D schickt Controller 74 ans Gerät. Controller 18 und 19 sind global belegt (Tempo,
  Lautstärke) und dürfen in Demos nicht verwendet werden.
- Im Dialog „Global MIDI Controls" wird nur gezeigt, nichts angeklickt: ein Klick auf „Learn"
  oder „Clear" würde die Geräte-Einstellung ändern.
- Für den Ton im Browser bekommt jedes Preset beim Laden einen „JS Audio-Out" dazugesetzt –
  nur im Browser, nicht auf dem Gerät.

**Display-Screenshot**

Seit diesem Stand kann die Firmware das (nur RGB-Display, also CrowPanel 5"):

- `lvgl screenshot /littlefs/screen.raw` – aktuelles Bild als rohes RGB565, 800×480
- `lvgl tap <x> <y>` – Klick auf das Widget an dieser Stelle

`python board_cli.py COM38 192.168.1.135 "lvgl tap 120 20" "wait 3" "shot display-presets.png" "lvgl tap 80 72"`
öffnet die Preset-Liste, holt das Bild als PNG ab, löscht die Rohdatei und schließt die Liste.

**Widget-Bilder neu aufnehmen** (nach Änderungen an Widgets)

`python demo_server.py <repo>/data 8766` starten, `gallery.js` und `demo-init.js` nach
`.playwright-mcp/vid/` kopieren, `gallery.js` über Playwright ausführen, Bilder nach `gallery5/`.

**Logo ändern**

`logo/big.html` mit Chrome als `logo/big.png` rendern (schwarz auf weiß), dann
`python make_logo.py logo/big.png logo.png 52`. Farben stehen in `make_logo.py`, Position und
Hinterlegung in `build5.py` (`LOGO_POS`).

## Offene Punkte

- Beim Bauen wurden nur Einzelbilder und Pegel geprüft, nicht der Ton nach Gehör.
- Die Browser-Engine knackst in den Aufnahmen weiterhin, der Demo-Ton ist bis an die Grenze
  ausgesteuert. Besser wäre echter Ton vom ESP32 (z. B. über USB-Audio aufgenommen).
- Lautstärken: Sprecher ca. −19,5 dB, Demos −13 bis −17 dB; TR-808 und Demo C sind am lautesten.
- SuperSaw-Akkorde in Demo A sind leiser als der Rest.
- Die Hauptseite auf dem Display ist beim FM8-Preset leer, weil es keine Bedienelemente auf
  einer Seite hat. Ein Preset mit Knöpfen sähe besser aus.
- Auf dem Display-Screenshot ist die IP-Adresse des Boards lesbar.
- Die Aufnahmen laufen mit etwa 11 Bildern pro Sekunde; Mausbewegungen wirken dadurch etwas ruckelig.
- Firmware-Fehler, beim Aufnehmen gefunden: wechselt das Gerät auf den unbenannten Slot, zeigt
  das Display oben weiter den alten Preset-Namen und die Regler-Karten erscheinen erst nach
  „Pages" → Seite antippen. In den Display-Bildern ist der falsche Name deshalb übermalt.
- Das eingeblendete Keyboard gibt es bisher nur in Demo D, Teil 1; in den anderen Demos leuchten
  keine Tasten mit.
- Das Schema ist schlicht (Kästen und Pfeile); Symbole für Keyboard, Board und Laptop wären schöner.
- „about 20 dollars" ist eine Angabe des Autors und nirgends im Projekt belegt.
- `documentation/voice-samples/` enthält 21 Stimmproben von der Stimmenauswahl und kann weg.
