# Speicherbudget für die Audio-Engine

Stand: 2026-10-04. Abschnitte 1–3 enthalten ursprüngliche statische Untersuchung von Commit `2798cc6`. Umsetzung auf Basis von `b8342ce`, ESP-IDF `v6.1`, ESP32-P4: siehe Abschnitt 5. Dort stehen tatsächlich verwendete Einstellungen, Hardwaremessungen und verbleibende Abnahmegrenzen; ursprüngliche Versuchswerte sind keine zusätzlichen Freigaben.

## Ziel und wichtigste Ergebnisse

Mehr **internen, zusammenhängenden P4-RAM** für Audiozustände und Blockpuffer verfügbar machen, während WiFi und BLE eingeschaltet bleiben. Freies PSRAM allein löst diesen Engpass nicht: `GraphBuilder` reduziert Stimmen, wenn deren zeitkritischer Zustand nicht in internen RAM passt.

Die aussichtsreichsten Kandidaten sind NimBLE-Allokationen im PSRAM, kleinere BLE-Pools und gezielt begrenzte Web-Allokationen. Danach folgen Hosted-Queues und gemessene Task-Stacks. Ein verbindliches Gesamteinsparpotenzial lässt sich ohne Heap-/Linker- und Laufzeitmessungen nicht angeben; Effekte überlappen und dürfen nicht einfach addiert werden.

| Speicherbereich | Bedeutung für dieses Projekt |
| --- | --- |
| P4 interner RAM | Audiozustände, Stacks, Heap, bestimmte DMA-/Transportpuffer; primäres Optimierungsziel |
| P4 PSRAM | Geeignet für weniger zeitkritische Daten; Verlagerung spart internen RAM, aber keinen Gesamtspeicher |
| ESP32-C6 RAM | WiFi-Funkstack und BLE-Controller; Einsparungen hier werden nicht zum P4-Audiobudget |
| Flash | Weniger eingebundener Code ist nicht automatisch weniger Laufzeit-RAM |

`network_init_task()` initialisiert ESP-Hosted gemeinsam für WiFi/BLE. Der NimBLE-Host läuft auf dem P4; `Feature::BLE::init()` bindet ihn an den C6-Controller. Deshalb nicht die üblichen lokalen ESP32-WiFi-RX/TX-Tipps ungeprüft auf den P4 übertragen.

## 1. Ausgangsmessung, bevor Parameter geändert werden

Vorhandene Diagnose wiederverwenden:

- [MemoryDiagnostics.cpp](../main/features/synth/MemoryDiagnostics.cpp), `snapshot()`, `captureReloadPhase()`, `setGraphStats()`.
- [synth_web_api.hpp](../main/features/synth/synth_web_api.hpp), `memory_diagnostics_handler()`, Route `/api/diagnostics/memory`.
- Bereits enthalten: freier/größter/minimaler interner Heap und PSRAM, Synth-Stackreserve, Instanz-/Blockpuffergrößen und PSRAM-Fallbacks.

Zusätzlich für einen Messbuild: Snapshots vor/nach Hosted-, BLE-, WiFi-, Web- und Synth-Initialisierung; internen DMA-fähigen Heap getrennt erfassen; High-Water-Marks der übrigen Tasks ergänzen. Flash-/NVS-Aufrufe und seltene Fehlerpfade gehören zur Stackmessung. ESP-IDF-Stackgrößen und die hier verwendeten High-Water-Marks in Bytes auswerten.

| Variante | Funkzustand | Zweck |
| --- | --- | --- |
| A | WiFi aus, BLE aus | Grundlast; Ethernet/Web/LVGL ansonsten unverändert lassen |
| B | WiFi an, BLE aus | Hosted + WiFi isolieren |
| C | WiFi aus, BLE an | Hosted + NimBLE isolieren |
| D | WiFi an, BLE an | Zielbetrieb; gemeinsame Hosted-Kosten nicht doppelt zählen |

Pro Variante gleiches Board, Preset, Stimmenzahl, Displayzustand und gleiche USB-Geräte verwenden. Boot, Leerlauf, BLE-Scan, Pairing, drei Verbindungen, MIDI-Bursts, Web-Upload/Download, AP-Fallback, Reconnect und Presetwechsel erfassen. Im Zustand ohne Webzugang dieselben Werte seriell ausgeben. Mindestens 100 Presetwechsel und 30 Minuten kombinierte Last als ersten Regressionstest vorsehen; das ersetzt keinen längeren Dauertest.

Neben freien Bytes immer `internalLargest`, `internalMinimum`, effektive Stimmenzahl, MIDI-Drops, Render-Overruns, DMA-Underruns und Render-p99 dokumentieren. Die Heap-Minima gelten über die bisherige Laufzeit: Vergleichsläufe jeweils nach Neustart beginnen. Mehr freie Bytes bei weniger Stimmen sind kein Gewinn.

Für jeden Kandidaten separaten Build erzeugen und `idf.py size`, `idf.py size-components` sowie bei Bedarf `idf.py size-files` vergleichen. Aktueller Workspace enthält kein App-Mapfile direkt unter `build`; vorhandene Build-Metadaten sind kein Nachweis für die RAM-Belegung des aktuellen Quellstands. Einstellungen in `sdkconfig.defaults` bzw. passenden Board-Defaults dauerhaft ablegen; bestehendes `sdkconfig` für Testbuilds ausdrücklich neu konfigurieren, da Defaults bestehende Werte nicht einfach überschreiben.

## 2. Priorisierte Maßnahmen

### P1 — NimBLE-Daten gezielt nach PSRAM verlagern

**Befund:** `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y`; PSRAM ist bereits aktiviert. `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y`, aber `CONFIG_BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC` ist aus. NimBLE-Hoststack: 4096 Bytes.

**Arbeit:** Zuerst ausschließlich `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` testen. Lokal vorhandenes IDF-Kconfig beschreibt diese Auswahl für dynamische Host-Allokationen. Tatsächliche Pool-, Peer- und HCI-Allokationspfade separat prüfen: Die Option verschiebt weder pauschal alle statischen Daten noch jeden Hosted-Puffer. Hoststack und DMA-Puffer nicht ohne Nachweis mitverlagern; BLE speichert Bonding-Daten in NVS.

**Potenzial:** Anteil der bisher intern angelegten dynamischen NimBLE-Daten; Bytes müssen gemessen werden. Kein fester Gewinn von beispielsweise „50 KB“ belegt. Prüfen, ob ausgelagerte Daten dieselben Pools enthalten, die unter P1 unten verkleinert werden sollen.

**Abnahme:** Pairing/Bonding über Neustart, HID und MIDI gleichzeitig, maximale Verbindungszahl, Reconnect und WLAN-Last funktionieren ohne zusätzliche MIDI-Drops, Cachefehler oder schlechtere Audio-p99. PSRAM-Allokationsfehler müssen kontrolliert behandelt werden.

### P1 — BLE-Pools an tatsächlichen Bedarf anpassen

Aktuelle Werte aus `sdkconfig`:

| Parameter | Ist | Separater Versuch |
| --- | ---: | --- |
| `BT_NIMBLE_MAX_CONNECTIONS` | 3 | 3 beibehalten; 1/2 nur als ausdrücklich eingeschränktes Geräteprofil |
| `BT_NIMBLE_MSYS_1_BLOCK_COUNT` / `BLOCK_SIZE` | 12 / 256 | zunächst beibehalten |
| `BT_NIMBLE_MSYS_2_BLOCK_COUNT` / `BLOCK_SIZE` | 24 / 320 | Count 16, danach ggf. 12 |
| `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` / `ACL_SIZE` | 24 / 255 | Count 16; Größe zunächst beibehalten |
| `BT_NIMBLE_TRANSPORT_EVT_COUNT` | 30 | zunächst 20 |
| `BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT` | 8 | zunächst beibehalten; Scan-Bursts testen |
| `BT_NIMBLE_ATT_PREFERRED_MTU` | 256 | erst später 128 testen |
| `BT_NIMBLE_ATT_MAX_PREP_ENTRIES` | 64 | Nutzung von Prepare Writes und zulässigen Kconfig-Bereich prüfen |
| `BT_NIMBLE_GATT_MAX_PROCS` | 4 | erst nach Messung paralleler Discovery/Subscriptions verkleinern |

Alle Namen in dieser Tabelle tragen in `sdkconfig` den Präfix `CONFIG_`. Zulässige Werte/Abhängigkeiten vor jedem Versuch im verwendeten IDF bestätigen.

Die beiden MSYS-Pools ergeben rechnerisch `12*256 + 24*320 = 10.752` Bytes nominelle Blockkapazität. `MSYS_2` von 24 auf 12 spart darin **3.840 Bytes**, ohne Verwaltungs-/Alignmenteffekte. ACL 24 auf 16 entspricht **2.040 Bytes** nomineller Payload-Kapazität; Events 30 auf 20 entsprechen bei Größe 70 **700 Bytes**. Diese Zahlen sind keine gemessenen internen Heapgewinne und keine unabhängige Gesamtsumme: Transportimplementierung, Speicherort und Poolbeziehungen zuerst bestätigen.

MTU-Verkleinerung spart nicht automatisch Pool-RAM, solange Blockgrößen gleich bleiben. Sie kann größere BLE-MIDI-Nachrichten und HID-Discovery beeinflussen. Poolzahl, MTU und Blockgröße deshalb nicht gleichzeitig ändern.

`CONFIG_BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC=y` ist ein **nachgelagerter Versuch**: Laut lokalem IDF-Kconfig werden Blöcke dann während der Nutzung alloziert/freigegeben. Das kann Leerlaufverbrauch senken, erhöht aber Allokationsaktivität und mögliche Fragmentierung; der Lastpeak bleibt entscheidend. Zunächst feste, ausreichend dimensionierte Pools bevorzugen.

### P1 — WebSocket-Speicherspitzen begrenzen

**Fundstelle:** [synth_web_api.hpp](../main/features/synth/synth_web_api.hpp), `websocket_handler()`, insbesondere `malloc(frame.len + 1)` um Zeile 1633.

Die vom Client angegebene Länge wird vor einer Obergrenze alloziert. Danach entsteht ein cJSON-Baum. Die Prüfung gegen `sizeof(LiveParamMsg::json)` erfolgt erst beim Weiterreichen an die Engine und schützt diesen ersten Peak nicht; lokal behandelte Nachrichtentypen haben andere Anforderungen.

**Arbeit:** Protokollweit zulässige Maximallänge aus allen verwendeten Nachrichten bestimmen, vor Addition/Allokation prüfen und übergroße Frames kontrolliert ablehnen bzw. Verbindung schließen. Danach Payload explizit in PSRAM anlegen, sofern deren Verwendung keine Cache-aus-Zugriffe erfordert. Parser-Allokationen separat betrachten. Keine pro Request umgeschalteten globalen cJSON-Hooks: Andere Tasks können gleichzeitig parsen.

**Potenzial:** Lastabhängiger Heap-Peak und Fragmentierung; kein dauerhafter fixer Gewinn. Bei der aktuellen `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` bevorzugen kleine normale Allokationen internen Speicher.

**Abnahme:** Alle gültigen Web-MIDI-/Parameter-/Geräteeinstellungsnachrichten funktionieren; Grenzwert, Übergröße, ungültiges JSON und Verbindungsabbruch geben Speicher zuverlässig frei. Heap nach wiederholten Requests stabil; keine Audioaussetzer unter Request-Bursts.

### P2 — ESP-Hosted-Queues verkleinern, DMA-Staging beibehalten

**Befund:** `CONFIG_ESP_HOSTED_HOST_SDIO_TX_Q_SIZE=20`, `RX_Q_SIZE=20`, `RX_STAGING_SLOTS=2`; RPC maximal 16 ausstehende Anforderungen. Hosted-Stack-Konfigurationen stehen auf 4096 Bytes, externe Taskstacks sind für Hosted aus.

**Arbeit:** Im verwendeten Hosted-Transport tatsächliche Queueelemente, Payload-Eigentümer und Allokations-Caps bestimmen. TX/RX separat von 20 auf 12 testen. RPC-Pending-Limit erst nach Messung des gleichzeitigen Bedarfs von Scan, BLE, Status und OTA reduzieren. Keine pauschale Rechnung „40 Slots mal Paketgröße“: Queues können nur Referenzen halten; reservierter Speicher und gehaltene Payloads unterscheiden sich.

`RX_STAGING_SLOTS=2` ist laut lokalem `managed_components/espressif__esp_hosted/host/mcu/eh_host_mcu_transport/Kconfig.host.sdio` bereits das Minimum. Nicht auf 1 setzen. Shared SDIO muss BLE-MIDI auch während WiFi-Uploads bedienen können. Geringerer Durchsatz ist nur akzeptabel, wenn UI und Audioanforderungen weiter erfüllt bleiben.

**Potenzial:** Zu messen; Queueverwaltung plus gegebenenfalls geringere Zahl gleichzeitig gehaltener Pakete. DMA-/Staging-Speicher nicht blind in PSRAM verlagern. Änderungen möglichst über öffentliche Kconfig-Optionen, nicht durch unversionierte Edits unter `managed_components`.

### P2 — Nicht verwendete BLE-Rollen und Discovery-Ressourcen reduzieren

**Befund:** Central, Peripheral, Broadcaster, Observer sowie GATT-Client und -Server sind aktiviert. Das Projekt beschreibt seinen BLE-Betrieb in [Kconfig.projbuild](../main/Kconfig.projbuild) als Central für HID/MIDI. [ble_manager.hpp](../main/features/ble/ble_manager.hpp), `init()`, ruft `peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 32, 32)` auf.

**Arbeit:** Peripheral/Broadcaster und unnötige lokale GATT-Serverdienste nach Prüfung der Abhängigkeiten einzeln deaktivieren. Central, Observer, GATT-Client, Verschlüsselung, Bonding und NVS-Persistenz erhalten. Die zahlreichen aktivierten `BT_NIMBLE_*_SERVICE`-Optionen sind nur Kandidaten: Unreferenzierte Services können bereits vom Linker entfernt werden.

Für `peer_init` erst Definition, Allokationsstrategie und tatsächliche Discovery-Auslastung prüfen; die Zahlen 20/32/32 nicht unbelegt mit der Verbindungszahl multiplizieren. Generische HID-Geräte können deutlich mehr Services/Characteristics benötigen als reine MIDI-Geräte. Kleinere Grenzen müssen eine fehlgeschlagene Discovery sauber beenden und ihre Ressourcen freigeben.

**Potenzial:** Unbekannt; Flash- und RAM-Deltas getrennt ausweisen. Kein Abschalten von Sicherheit als Speicheroptimierung.

### P2 — Task-Stacks nach Messung reduzieren

| Fundstelle | Reservierung | Bewertung |
| --- | ---: | --- |
| `web_server.hpp`, `Feature::Web::begin()` | 10.240 B | Dateiexplorer/Upload benötigen laut Code mehr als den Standardstack |
| `display_lvgl.hpp`, `Feature::LVGL::init()` | 16.384 B | PNG-Decoding und VFS als Spitzenlast berücksichtigen |
| `synth_manager.hpp`, `preset_nav` | 4.096 B | Navigation und Dateisystempfade messen |
| `i2c_bus_manager.hpp`, Worker | je 4.096 B | Pro aktiviertem Bus; Fehler-/Loggingpfade messen |
| NimBLE-Host | 4.096 B | Pairing, Discovery, Notifications und NVS messen |
| `app_startup.hpp`, `network_init` | 12.288 B | Nur Boot-Peak: Task löscht sich bereits selbst |
| `app_startup.hpp`, `sd_card_init` | 6.144 B | Boot-Reservierung getrennt von dauerhaften Tasks bewerten |

Zunächst Reserven erfassen, danach Kandidaten einzeln in kleinen Schritten reduzieren und eine begründete Sicherheitsreserve behalten. Beispiel: HTTP 10.240 auf 8.192 Bytes würde **2.048 Bytes** dauerhaft sparen, ist aber ohne Lastmessung nicht freigegeben. Große lokale JSON-/Uploadpuffer als mögliche Ursache des Stackbedarfs prüfen; deren Auslagerung spart erst dann internen RAM, wenn auch der reservierte Stack kleiner wird.

HTTP-Stack bleibt intern: `web_server.hpp` setzt absichtlich `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` und dokumentiert den Flash-/Cachefehler mit PSRAM-Stack. Gleiches Risiko bei NVS-/Dateisystempfaden anderer Tasks prüfen. `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` verlagert gewöhnliche `xTaskCreate...`-Stacks nicht automatisch. [ESP-IDF v6.1: External RAM](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-guides/external-ram.html).

### P2 — AP-Fallback nur bei Bedarf anlegen

**Fundstellen:** [wifi_manager.hpp](../main/features/wifi/wifi_manager.hpp), `init()`, `start_captive_portal()`, `stop_captive_portal()`.

Das AP-Netif wird bei aktivierter Captive-Portal-Funktion schon beim Start angelegt, obwohl zunächst STA-Modus verwendet wird. Beim erfolgreichen Reconnect werden DNS und AP-Modus beendet, das Netif bleibt bestehen. DNS startet bereits bedarfsgesteuert; hierfür keinen weiteren Dauerverbrauch behaupten.

**Arbeit:** AP-Netif und gegebenenfalls Retry-Timer erst bei Fallback erzeugen, nach sauberem Ende wieder freigeben. Eventhandler-/DHCP-Lebenszyklus korrekt abmelden; wiederholtes Anlegen/Löschen darf keine Lecks oder Rennen erzeugen. Zuerst mit dauerhaftem Netif vergleichen: Wenn Gewinn klein und Fragmentierung höher ist, bestehende Lösung behalten.

**Potenzial:** Kleiner, noch ungemessener P4-Gewinn im normalen STA-Betrieb. AP-Funkressourcen auf dem C6 getrennt ausweisen. Captive Portal vollständig auszuschalten ist ein Funktionsprofil, keine transparente Optimierung.

### P3 — lwIP und globale malloc-Policy erst danach

`LWIP_MAX_SOCKETS=16`, `httpd.max_open_sockets=12`, TCP-Sendepuffer/Fenster jeweils 5760 Bytes, IPv6 an. Die Socketzahl wurde laut `web_server.hpp` bewusst wegen paralleler Browserrequests, WebSocket und Watchdogproblemen angehoben. Deshalb nicht zuerst verkleinern. TCP-Fenster gegebenenfalls separat auf 4320 Bytes testen; dies ist ein Versuchswert, keine garantierte Einsparung pro Verbindung. Ethernet ist ebenfalls aktiviert und nutzt lwIP mit.

`SPIRAM_MALLOC_ALWAYSINTERNAL=16384` kann später versuchsweise auf 4096 gesenkt werden, nachdem Allokationen in Audio-, Flash- und Treiberpfaden geprüft wurden. Explizite PSRAM-Allokationen für bekannte unkritische Daten sind zunächst gezielter. `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` ist kein belegter „verschwendeter 32-KiB-Puffer“; dessen Änderung erzeugt kein zusätzliches physisches RAM. DMA-/Stackreserven erhalten.

IPv6 nur in einem ausdrücklich IPv4-only vorgesehenen Profil deaktivieren. `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` bzw. gezielte `EXT_RAM_BSS_ATTR`-Platzierung können statische, nicht zeitkritische Daten verlagern; erst Linkermap prüfen, anschließend Cache-aus-Pfade testen. Keine pauschale Verlagerung des Audiozustands. Die IDF-Dokumentation erläutert malloc-Schwelle, BSS-Platzierung und Cachebeschränkungen: [External RAM, v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-guides/external-ram.html).

## 3. Bereits umgesetzt und weitere Grenzen

- `Graph::allocate()` ist laut [Graph.h](../main/audio/engine/Graph.h) bereits PSRAM-first. Dies nicht erneut als neue Einsparung zählen.
- Live-Parameter-, MIDI-Debug- und LVGL-Knob-Aktivitätsqueues nutzen bereits PSRAM (`synth_manager.hpp`, `SynthEngine::begin()`).
- `SynthEngine::loaderTaskLoop()` fordert beim Reload bereits Pre-Free des alten Graphen an. Eine angebliche generelle Doppelbelegung alter/neuer Graphen wäre hier keine neue Erkenntnis. Timeout-/Fehlerpfade separat untersuchen, falls die Diagnose dort Peaks zeigt.
- Display-Bildcache nutzt PSRAM; bestimmte MIPI-Varianten besitzen zusätzliche Vollbildpuffer. Deren Reduktion ist vorrangig PSRAM-/Bandbreitenoptimierung und boardabhängig. SPI-Puffer sind DMA-markiert; keine pauschale Gleichbehandlung.
- Zeitkritische Audiozustände, DSP-Tabellen und Blockpuffer nicht allein für eine höhere Anzeige „freier interner RAM“ ins PSRAM schieben. Mehr Cachekonkurrenz mit Display und Funk kann Stimmenzahl und Echtzeitfähigkeit verschlechtern.
- Browser-JavaScript-Verkleinerung spart hauptsächlich Browser-/Flashressourcen, nicht automatisch P4-Laufzeit-RAM.
- Ein optionaler späterer Funk-Aus-Modus benötigt echten Stop-/Deinit-Lebenszyklus. Disconnect allein ist keine garantierte Speicherfreigabe. Shared Hosted-Transport darf bei weiterlaufendem BLE oder WiFi nicht abgebaut werden; Ethernet/Webzugriff berücksichtigen. Für das Ziel „beide aktiviert“ zunächst nachrangig.

## 4. Abarbeitungsplan

- [x] **M0 Umsetzung:** Referenz sichern, frisch bauen/flashen, DMA-/Taskdiagnose ergänzen; A/C/D gemessen. B scheitert am vorhandenen Linkerproblem, siehe offene Abnahme.
- [x] **M1 Umsetzung:** Externe NimBLE-Allokationen testen und übernehmen; Vergleich mit Referenz durchgeführt.
- [x] **M2 Umsetzung:** MSYS2 16, ACL 16, Events 20 einzeln bauen/flashen; verfügbares MIDI-Gerät funktioniert. Drei Geräte und echte WiFi-Datenlast bleiben offen.
- [x] **M3:** WebSocket-Grenze 8 KiB, Payload/Broadcast in PSRAM; zusätzlicher Preset-Upload als 1-KiB-Stream. Grenz-/Abbruchtests bestanden.
- [x] **M4:** TX/RX jeweils 12 einzeln getestet; Allokationspfade geprüft. RPC 16 und DMA-Staging 2 bleiben erhalten.
- [x] **M5:** Peripheral/Broadcaster deaktiviert; Central/Observer/Client und Sicherheit erhalten. Gemeinsame Peer-Pools 20/32/32 nach Prüfung beibehalten.
- [x] **M6:** Dauerhafte Taskreserven gemessen; Synth, Loader, LVGL und HTTP reduziert. Nicht ausreichend belastete oder knapp dimensionierte Tasks bleiben erhalten.
- [x] **M7 Umsetzung:** AP-Netif bedarfsgesteuert anlegen und nach Reconnect freigeben. Boot-Messung erfolgt; tatsächlicher AP-/STA-Wechsel bleibt offen.
- [x] **M8 Bewertung:** Globale malloc-Policy, lwIP und BSS nach RAM-Gewinn unverändert lassen; kein belegter Nutzen gegenüber zusätzlichen Risiken.
- [x] **M9 Umsetzung:** Defaults aktualisiert, 100 identische Reloads, Vergleichstest, 30-Minuten-Lauf und kontrollierter BLE-Reconnect bestanden. Zusätzliche Hardware-Abnahme bleibt unten ausdrücklich offen.

Jede Maßnahme erhält ein Ergebnis in dieser Tabelle; nicht wirksame oder instabile Versuche werden verworfen:

| Maßnahme / Konfiguration | Intern frei vorher/nachher | Größter Block vorher/nachher | Minimum unter Last | PSRAM-Delta | Stimmen / Audio-p99 / Drops | Entscheidung |
| --- | --- | --- | --- | --- | --- | --- |
| Referenz / optimiert, identischer Kurzlasttest | 51.515 / 110.179 B nach Last | 31.744 / 31.744 B in diesem Vergleichsboot | 20 / 47.216 B | −21.064 B frei | 3 Stimmen; p99-Median 2.176 / 2.112 µs; 0 MIDI-Drops | RAM-Gewinn bestätigt; Audio-Basislast bleibt |

**Freigabekriterium:** Bei identischem Preset und gleicher Stimmenzahl steigt der nutzbare interne Speicher bzw. sinkt der Lastpeak reproduzierbar, ohne Regression bei MIDI, Audio, UI, Bonding, Reconnect oder Flashzugriffen. Erst danach höhere Stimmenzahlen ausprobieren.

## 5. Umsetzung und Hardwaremessungen

### Messbedingungen

ESP32-P4 an `COM15`, Web unter `http://192.168.1.121`. Ausschließlich `FatSuperSaw`, unveränderte Preset-Datei und drei Stimmen. Preset bleibt als zuletzt geladenes Startpreset erhalten. Alle erfolgreichen Stufen wurden einzeln mit `idf.py build` kompiliert, mit `idf.py -p COM15 app-flash` hochgeladen und anschließend über `/api/diagnostics/memory` gemessen. LittleFS, Webpartition und NVS wurden nicht neu geflasht. Einstellungen stehen dauerhaft in `sdkconfig.defaults`; vorhandenes `sdkconfig` wurde für Versuche ausdrücklich angepasst.

WiFi und BLE sind im finalen Zielprofil eingeschaltet. WLAN-SSID ist leer; Webverkehr läuft über Ethernet. Ein gespeichertes BLE-MIDI-Gerät verbindet sich nach Neustarts wieder. Neue Paarung, Verschlüsselungsnachweis und parallele HID-/Dreigerätenutzung wurden nicht vorgetäuscht. WiFi-Last auf dem C6 kann aus Ethernetverkehr nicht abgeleitet werden.

Momentwerte schwanken um mehrere KiB durch TCP-/RPC-Puffer und Verbindungsaufbau. Tabellenwerte sind konkrete Snapshots, keine isoliert addierbaren Einsparungen. Heap-Minimum gilt seit Boot; ursprünglicher Langzeitwert von 20 Bytes wurde auch im frischen Referenzvergleich unter Last wieder gemeldet. `heap_caps_get_minimum_free_size()` addiert Tiefstwerte einzelner Regionen, die zu unterschiedlichen Zeiten aufgetreten sein können: konservative Untergrenze, kein Beweis, dass insgesamt gleichzeitig nur 20 Bytes frei waren. Größter interner Block beträgt optimiert je nach Boot/Allokationsreihenfolge 31.744 oder 45.056 Bytes; Wachstum auf 44 KiB ist nicht bei jedem Boot garantiert.

### Einzelstufen

Alle Zahlen in Bytes. Zu jeder erfolgreichen Stufe liegen Build-/Flashlogs und JSON-Snapshots lokal unter `build/memory-audit/` (ignorierte Arbeitsartefakte).

| Stufe | Änderung | Intern frei | Größter Block | Bisheriges Minimum | PSRAM frei | Build / Upload |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 00 | Frische Ausgangsfirmware | 54.491 | 31.744 | 41.160 | 26.271.352 | bestanden |
| 01 | DMA-, MIDI-/Overrun- und Taskdiagnose ergänzen | 52.831 | 31.744 | 24.472 | 26.271.344 | bestanden |
| 02 | NimBLE `MEM_ALLOC_MODE_EXTERNAL` | 79.015 | 31.744 | 44.844 | 26.243.432 | bestanden |
| 03 | MSYS2-Blöcke 24 auf 16 | 83.059 | 31.744 | 60.816 | 26.245.996 | bestanden |
| 04 | ACL-RX-Blöcke 24 auf 16 | 83.039 | 31.744 | 60.816 | 26.248.428 | bestanden |
| 05 | Event-Blöcke 30 auf 20 | 83.075 | 31.744 | 59.276 | 26.249.312 | bestanden |
| 06 | WS-Payload/Broadcast in PSRAM, Grenze 8 KiB | 83.075 | 31.744 | 60.816 | 26.249.312 | bestanden |
| 07 | Hosted TX-Queue 20 auf 12 | 81.195 | 31.744 | 52.620 | 26.249.312 | bestanden |
| 08 | Hosted RX-Queue 20 auf 12 | 81.839 | 31.744 | 52.620 | 26.249.312 | bestanden |
| 09 | Preset speichern: 1-KiB-Stream statt gesamtem Body | 83.775 | 31.744 | 60.816 | 26.249.320 | bestanden |
| 10 | BLE-Peripheral deaktivieren | 76.783 | 31.744 | 43.512 | 26.250.264 | bestanden |
| 11 | BLE-Broadcaster deaktivieren | 84.335 | 31.744 | 60.832 | 26.250.276 | bestanden |
| 12 | Synth-Stack 32 auf 12 KiB | 102.355 | 45.056 | 74.152 | 26.250.276 | bestanden |
| 13 | LVGL-Stack 16 auf 12 KiB | 106.479 | 45.056 | 79.268 | 26.250.276 | bestanden |
| 14 | HTTP-Stack 10 auf 8 KiB | 108.863 | 31.744 | 69.552 | 26.250.276 | bestanden |
| 15 | Loader-Stack 8 auf 6 KiB | 110.551 | 45.056 | 75.216 | 26.250.276 | bestanden |
| 16 | AP-Netif erst im Fallback erzeugen | 113.651 | 45.056 | 85.428 | 26.250.284 | bestanden |
| 17 | Dauerhafte Defaults prüfen | 111.231 | 45.056 | 77.232 | 26.250.276 | bestanden |
| Final | Zielkonfiguration explizit neu konfigurieren | 110.815 | 45.056 | 79.268 | 26.250.276 | bestanden |

Größter dauerhafter Gewinn stammt aus NimBLE-Verlagerung und Stack-Reservierungen. Vier Stack-Kürzungen sparen zusammen exakt **28.672 Bytes Reservierung**. Der AP-Versuch zeigte etwa 3 KiB Unterschied im Boot-Snapshot; Schwankungen verhindern genaue isolierte Zuschreibung. Webänderungen begrenzen überwiegend Lastspitzen, statt einen zusätzlichen festen Leerlaufgewinn zu liefern. Kleinere BLE-Pools sparen nach PSRAM-Verlagerung teilweise PSRAM.

### Konfiguration und bewusst beibehaltene Grenzen

- BLE: Central, Observer, GATT-Client, drei Verbindungen, drei Bonds, MTU 256, Security und NVS-Persistenz bleiben aktiv. Peripheral/Broadcaster fallen weg. MSYS1 bleibt 12 × 256; MSYS2 16 × 320; ACL 16 × 255; Events 20 × 70; Discard-Events 8; vier GATT-Prozeduren. Prepare-Write-Grenze 64 unverändert; als Central kein belegter Gewinn durch zusätzliche Reduktion.
- `peer_init(3, 20, 32, 32)` reserviert gemeinsame Service-/Characteristic-/Descriptor-Pools. Keine unbelegte Multiplikation mit drei Peers. Grenzen bleiben für generisches HID erhalten. Runtime-Mempool-Allokation bleibt aus; feste Pools vermeiden zusätzliche Heapaktivität im MIDI-Pfad.
- Hosted: TX/RX je 12. Queueelemente enthalten Verwaltungsdaten/Referenzen; kein nachgewiesener Gewinn von 40 vollständigen SDIO-Paketen. SDIO-DMA-Staging bleibt beim Minimum 2; RPC-Limit 16 und Transport-Stacks 4 KiB bleiben erhalten. Keine unversionierten Änderungen in `managed_components`.
- HTTP-Stack bleibt intern. Payload und Broadcast dürfen in PSRAM liegen; Dateischreibpuffer bleibt wegen Flash-/Cachezugriffen intern. Keine wechselnden globalen cJSON-Hooks.
- Preset-POST schreibt in temporäre Datei und ersetzt Ziel erst nach vollständigem Empfang und erfolgreichem Schreiben/Schließen. Abbruch/Schreibfehler löschen temporäre Datei und erhalten ursprüngliches Preset. Gleichzeitig belegte Flashdateien benötigen Platz; bei Platzmangel wird Fehler gemeldet.
- AP-Netif nutzt nativen IDF-Lebenszyklus. STA-Netif bleibt erhalten; IDF behält dessen globale Handler beim Löschen des AP-Netifs. Retry-Timer bleibt mit kleiner fester Reservierung bestehen. DNS war bereits bedarfsgesteuert.
- Navigation bleibt bei 4 KiB: früherer 2-KiB-Absturz dokumentiert, Navigationstiefe mit fest vorgeschriebenem Preset nicht belastet. I2C-Worker bleiben je 4 KiB: vollständige CV-/Fehlerlast fehlt. NimBLE-, Hosted-, USB- und Main-Stacks bleiben erhalten; teilweise geringe Reserven beziehungsweise ungetestete Security-/OTA-/Treiberpfade.
- lwIP: 16 Sockets, HTTP 12, TCP-Fenster/Sendepuffer 5.760 Bytes, IPv6 erhalten. malloc-Schwelle 16 KiB und interne Reserve 32 KiB bleiben erhalten. Keine pauschale BSS-/DSP-Verlagerung nach PSRAM. Linkermap zeigt statischen DIRAM-Verbrauch von 276.924 Bytes, einschließlich 156.494 Bytes Text; gemeldeter Linker-Rest ist kein freier Laufzeitheap.

### Stackreserven nach Last

| Task | Reservierung vorher / nachher | Kleinste gemessene Reserve nach Änderung |
| --- | ---: | ---: |
| Synth | 32.768 / 12.288 B | 9.292 B |
| Preset-Loader | 8.192 / 6.144 B | 2.212 B im Dauertest; 3.188 B nach isolierten FatSuperSaw-Reloads |
| LVGL | 16.384 / 12.288 B | 7.560 B inklusive Splash-PNG |
| HTTP | 10.240 / 8.192 B | 4.716 B inklusive Uploads |

Diese Reserven beziehen sich auf vorgegebenes Preset und vorhandene Hardware. Andere DSP-Graphs, USB-Audio-Mixing und vollständige Dateiexplorer-/HID-/CV-Last benötigen zusätzliche Abnahme; Stackgrößen nicht aus diesem Einzelprofil weiter reduzieren.

### Funkmatrix nach Optimierung

Ethernet, Display, USB und `FatSuperSaw` bleiben gleich. Matrix misst P4-RAM, nicht C6-RAM.

| Variante | Intern frei | Größter Block | Boot-Minimum | Ergebnis |
| --- | ---: | ---: | ---: | --- |
| A: WiFi/BLE aus | 179.991 B | 49.152 B | 122.152 B | gemessen mit vorübergehend bedingt registrierter NimBLE-Hilfskomponente |
| B: WiFi an, BLE aus | — | — | — | Linkerfehler, keine Laufzeitmessung |
| C: WiFi aus, BLE an | 116.595 B | 45.056 B | 85.968 B | Build, Flash und MIDI-Reconnect bestanden |
| D: WiFi/BLE an | 111.595 B | 45.056 B | 72.916 B | wiederhergestellter Zielbuild bestanden |

Vollständig abgeschaltetes Bluetooth baut mit ursprünglicher unconditional IDF-Beispielkomponente nicht: `host/ble_hs.h` fehlt. Bedingte lokale Registrierung erlaubte Messung A, wurde anschließend verworfen. WiFi-only scheitert auch mit ursprünglich registrierter Komponente und lediglich deaktiviertem App-BLE an `--enable-non-contiguous-regions discards section .bss.g_wifi_osi_funcs` / `unresolvable R_RISCV_HI20 relocation`. Keine unbelegte Zahl für B. Ein zukünftiges Funk-Aus-Profil benötigt eigene Build-/Linker-Abnahme.

Beim Wiederherstellen einer Konfigurationskopie wird `idf.py reconfigure` ausdrücklich ausgeführt: `Copy-Item` kann alten Änderungszeitpunkt erhalten und Ninja sonst veraltete generierte Konfiguration weiterverwenden. Finale generierte Header bestätigen WiFi und App-Bluetooth eingeschaltet.

### Belastung und Abnahme

**100 Reloads:** ausschließlich `POST /api/preset/select?name=FatSuperSaw`, Generation 3 bis 103, alle erfolgreich. Intern frei vorher/nachher 111.259 / 110.195 Bytes; PSRAM frei identisch 26.250.276 Bytes; größter interner Block 45.056 Bytes; Lebenszeit-Minimum 38.908 Bytes. Drei Stimmen, 25 Graph-Instanzen, 57 interne Blockpuffer mit zusammen 29.184 Bytes, keine Output-PSRAM-Fallbacks und keine MIDI-Drops. Einzelne Netzwerkpuffer verursachen kleine Momentdifferenzen; kein fortlaufender Graph-/PSRAM-Verlust belegt.

**Web-Grenzen:** ungültiges JSON verwirft Nachricht ohne Absturz; 8.192-Byte-Textframe bleibt verbunden, 8.193-Byte-Frame schließt Session. Abgebrochener HTTP-Save erhält Originaldatei bytegenau. Um 96 KiB Leerraum erweitertes, semantisch identisches FatSuperSaw lässt sich speichern; anschließend Originalbytes wiederhergestellt. Tests nach HTTP-Stack-Kürzung erneut bestanden.

**Direkter Referenzvergleich:** gespeicherte Referenz enthält zusätzliche Diagnose, aber keine Speicheroptimierungen. Referenz und Finalbinary jeweils frisch auf App-Partition geflasht. Identisch: 300 Dreiklangzyklen mit 70 ms Note-on/30 ms Pause, 20 Presetdownloads parallel, anschließend fünf Reloads und zwei Sekunden Nachlauf.

| Messwert | Referenz | Optimiert |
| --- | ---: | ---: |
| Intern frei vor / nach Test | 52.123 / 51.515 B | 110.771 / 110.179 B |
| Minimum seit Boot | 20 B | 47.216 B |
| Größter Block nach Test | 31.744 B | 31.744 B |
| PSRAM frei nach Test | 26.271.340 B | 26.250.276 B |
| Stimmen / Graph-Instanzen | 3 / 25 | 3 / 25 |
| MIDI-Drops | 0 | 0 |
| Neue Render-Overruns im Test | 271 | 276 |
| Median der gemeldeten Render-p99-Werte | 2.176 µs | 2.112 µs |
| Mittelwert der gemeldeten Render-p99-Werte | 2.377 µs | 2.395 µs |
| Größter gemeldeter p99-Wert | 5.888 µs | 6.656 µs |

**Bewertung:** Rund 57,3 KiB zusätzlicher interner Heap im vergleichbaren Lastzustand; etwa 20,6 KiB weniger freies PSRAM. Audio-p99 im Mittel nahezu gleich; Einzelmessung liefert keinen statistischen Nachweis identischer Extremwerte. Audio-Overruns und DMA-Underruns existieren bereits in Referenz. Nutzer hat Weblade-Underruns ausdrücklich akzeptiert; diese gelten nicht als Abnahmefehler. Ungeplante Neustarts, MIDI-Drops, Stimmenreduktion oder PSRAM-Fallbacks bleiben Abnahmefehler.

**30-Minuten-Test:** zweiter Lauf erfolgreich über 1.801,6 Sekunden. 3.527 Dreiklangzyklen, 873 Presetdownloads, 29 angeforderte FatSuperSaw-Reloads und sechs BLE-Scans. Keine ungeplanten Neustarts, keine MIDI-Drops, durchgehend interne Graph-Instanzen/Blockpuffer und keine Output-PSRAM-Fallbacks. Erster Lauf wurde nach 395 Sekunden durch vom Nutzer bestätigten manuellen Reset unterbrochen, nicht als Firmwareabsturz gewertet. MIDI wird über bestehenden Web-MIDI-Pfad injiziert; tatsächlicher BLE-MIDI-Burst von drei Geräten wird damit nicht ersetzt.

| Dauertest-Messwert | Ergebnis |
| --- | ---: |
| Intern frei am Anfang / Ende | 106.803 / 112.443 B |
| Bereich der Minuten-Snapshots | 104.539–112.867 B |
| Konservative Heap-Untergrenze seit Boot | 46.440 B |
| Größter interner Block am Ende | 45.056 B |
| PSRAM frei am Anfang / Ende | 26.250.284 / 22.318.120 B |
| MIDI-Drops | 0 |
| Median gemeldeter Render-p99-Werte | 2.112 µs |
| Neue Render-Overruns während Lauf | 5.214; einschließlich akzeptierter Web-/Reload-Last |

Messabweichung: Generation 1 bis 31 enthält einen zusätzlichen Reload außerhalb der 29 angeforderten Reloads. Generation 2 meldete kurz fünf konfigurierte statt drei Stimmen; maximal drei aktive Stimmen gemessen. Danach wieder drei konfigurierte Stimmen. PSRAM zeigte einmalige zusätzliche Belegung von etwa 3,75 MiB, anschließend Plateau mit temporären Reload-Schwankungen; kein fortlaufender interner Heapverlust. Ursache dieser zusätzlichen Laufzeitaktivität nicht belegt. Dauertest deshalb als Stabilität unter kombinierter Aktivität lesen; strikt gleicher Drei-Stimmen-Vergleich ist oben separat dokumentiert. Gespeicherte FatSuperSaw-Datei am Ende semantisch identisch mit ursprünglicher Sicherung.

**BLE-Reconnect:** gespeichertes, verbundenes MIDI-Gerät kontrolliert getrennt, Disconnect bestätigt, bestehendes Autoconnect erneut aktiviert. MIDI-Verbindung nach 1.046 ms wiederhergestellt; ursprüngliche Auto-Einstellung erhalten, kein Bond gelöscht. Danach 110.871 Bytes intern frei, größter Block 45.056 Bytes, MIDI-Drops weiterhin 0 und identischer interner Graph.

**Buildgröße:** `idf.py size` und `idf.py size-components` ausgeführt. Referenzbinary mit Diagnose 2.277.824 Bytes, optimiert 2.258.608 Bytes, Differenz −19.216 Bytes Flash. Finaler statischer DIRAM-Verbrauch 276.924 Bytes. Flashersparnis nicht als zusätzlichen Heapgewinn zählen. Fehlgeschlagene Varianten wurden nicht hochgeladen; bestätigte Zielkonfiguration ist wieder aktiv.

### Verbleibende Hardware-Abnahme

- [x] 30-Minuten-Lauf abschließen, Abweichungen und Endwerte eintragen; BLE-Reconnect prüfen.
- [ ] Drei BLE-Geräte, HID und MIDI parallel, neue Paarung/Bond-Key-Persistenz und echte BLE-MIDI-Bursts unter WLAN-Upload testen. Hardware fehlt; drei Verbindungen bleiben konfiguriert.
- [ ] WLAN-SSID konfigurieren, reale WiFi-Datenlast und wiederholte STA/AP-Fallback-/Reconnect-Zyklen prüfen. Keine Zugangsdaten vorhanden; Ethernet ist kein Ersatz für diesen Test.
- [ ] WiFi-only-Laufzeitmessung ergänzen; Linkerproblem im Nachtrag unten behoben.
- [ ] Andere große DSP-Presets, USB-Audio-Mixing, volle CV-/Dateiexplorer-Last und längeren Dauertest zusätzlich abnehmen.

**Abschließender Geräteabgleich:** Finale Kompilierung erfolgreich; SHA-256 des Build-Binaries stimmt mit hochgeladener Firmware überein. Webdiagnose: 113.043 Bytes intern frei (110,4 KiB), größter Block 45.056 Bytes (44 KiB), DMA-fähig intern frei 67.247 Bytes; FatSuperSaw aktiv, BLE bereit und ein gespeichertes Auto-Gerät verbunden, MIDI-Drops 0. Momentwert schwankt mit Netzwerkaktivität; vergleichbarer Gewinn bleibt 57,3 KiB. Einzelne Funk-/Hardware-Abnahmen bleiben oben ausdrücklich offen.

### Nachtrag: WiFi-only-Buildfehler

Nach dem Audit war lokal `CONFIG_APP_ENABLE_WIFI=y`, `CONFIG_APP_ENABLE_BLUETOOTH=n` und SDK-Bluetooth weiterhin eingeschaltet. Dieses Profil reproduzierte den oben dokumentierten Linkerfehler. Ursache: IDF 6.1 definiert auf P4 ohne native WiFi-Hardware `g_wifi_osi_funcs` als schwaches Objekt eines leeren Structs, also mit Größe null. Binutils verwirft dessen Section beim Linken mit `--enable-non-contiguous-regions`; verbliebene Referenzen verursachen anschließend RISC-V-Relocation-Fehler. Kein nachgewiesener RAM-Überlauf.

`main/wifi_remote_compat.c` ersetzt ausschließlich bei Remote-WiFi ohne native WiFi-Hardware den unbenutzten Platzhalter durch ein starkes Ein-Byte-Objekt. Hosted überträgt die Konfigurationswerte per RPC, keine OSI-Funktionstabelle. Native WiFi-Konfigurationen behalten ihre echte Tabelle. Die globale RAM-Aufteilung und SDK-Dateien bleiben unverändert. Die Datei wird über `main/CMakeLists.txt` registriert. WiFi-only-Build und Gegenprüfung mit WiFi + BLE erfolgreich. Anschließend ursprüngliche WiFi-only-Konfiguration wiederhergestellt und erneut gebaut; Laufzeitmessung der Matrix B weiterhin offen.

Der vorhandene Buildcache verwendet `C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe`. Eine Shell mit anderer IDF-Python-Umgebung meldet separat einen Cache-Konflikt. Zur Prüfung wurde dieselbe Python-Umgebung wie im vorhandenen Cache verwendet.
