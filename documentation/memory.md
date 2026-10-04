# Speicherbudget für die Audio-Engine

Stand: 2026-10-04. Statische Untersuchung von Commit `2798cc6` einschließlich der vorhandenen lokalen Änderungen. Referenzkonfiguration: aktuelles `sdkconfig`; Build-Metadaten nennen ESP-IDF `v6.1`, Ziel ESP32-P4. Es wurden keine Firmware- oder Konfigurationsänderungen vorgenommen und keine Hardwaremessungen durchgeführt. Die genannten Testwerte sind Vorschläge, keine freigegebenen Einstellungen.

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

- [ ] **M0:** Aktuelle Konfiguration/Abhängigkeiten sichern, frischen Referenzbuild erzeugen, Diagnosematrix A–D erfassen.
- [ ] **M1:** NimBLE-Allokationsmodus extern testen; tatsächliche P4-/PSRAM-Deltas und Latenzen dokumentieren.
- [ ] **M2:** MSYS-/ACL-/Event-Pools einzeln reduzieren, jeweils mit drei Geräten und WiFi-Last prüfen.
- [ ] **M3:** WebSocket-Maximallänge vor Allokation validieren; Payload-Speicherort gezielt festlegen.
- [ ] **M4:** Hosted-Queue-/RPC-Allokationen bestimmen, danach einzelne Limits testen; Staging bei 2 belassen.
- [ ] **M5:** BLE-Rollen/Services und Peer-Pools prüfen; nur tatsächlich unnötige Ressourcen entfernen.
- [ ] **M6:** Stackreserven aller dauerhaften Tasks messen; belegte Überdimensionierung reduzieren.
- [ ] **M7:** AP-Netif-Lebenszyklus optimieren, sofern Messung nennenswerten Nutzen zeigt.
- [ ] **M8:** Erst bei weiter bestehendem Engpass lwIP, BSS-Platzierung und malloc-Schwelle testen.
- [ ] **M9:** Erfolgreiche Einzeländerungen kombiniert testen, Defaults aktualisieren und Hardware-Dauertest durchführen.

Jede Maßnahme erhält ein Ergebnis in dieser Tabelle; nicht wirksame oder instabile Versuche werden verworfen:

| Maßnahme / Konfiguration | Intern frei vorher/nachher | Größter Block vorher/nachher | Minimum unter Last | PSRAM-Delta | Stimmen / Audio-p99 / Drops | Entscheidung |
| --- | --- | --- | --- | --- | --- | --- |
| Referenz WiFi + BLE | offen | offen | offen | offen | offen | messen |

**Freigabekriterium:** Bei identischem Preset und gleicher Stimmenzahl steigt der nutzbare interne Speicher bzw. sinkt der Lastpeak reproduzierbar, ohne Regression bei MIDI, Audio, UI, Bonding, Reconnect oder Flashzugriffen. Erst danach höhere Stimmenzahlen ausprobieren.
