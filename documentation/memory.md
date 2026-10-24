# Memory Budget for the Audio Engine

Status: 2026-10-04. Sections 1–3 contain the original static analysis of commit `2798cc6`. Implementation based on `b8342ce`, ESP-IDF `v6.1`, ESP32-P4: see section 5. It contains the settings actually used, hardware measurements and remaining acceptance limits; the original trial values are not additional approvals.

## Goal and main results

Make **more internal, contiguous P4 RAM** available for audio state and block buffers while WiFi and BLE stay enabled. Free PSRAM alone does not solve this bottleneck: `GraphBuilder` reduces voices when their time-critical state does not fit into internal RAM.

The most promising candidates are NimBLE allocations in PSRAM, smaller BLE pools and deliberately limited web allocations. Hosted queues and measured task stacks follow after that. A binding total savings potential cannot be stated without heap/linker and runtime measurements; effects overlap and must not simply be added together.

| Memory area | Meaning for this project |
| --- | --- |
| P4 internal RAM | Audio state, stacks, heap, certain DMA/transport buffers; primary optimization target |
| P4 PSRAM | Suitable for less time-critical data; moving data saves internal RAM, but not total memory |
| ESP32-C6 RAM | WiFi radio stack and BLE controller; savings here do not count toward the P4 audio budget |
| Flash | Less linked code does not automatically mean less runtime RAM |

`network_init_task()` initializes ESP-Hosted jointly for WiFi/BLE. The NimBLE host runs on the P4; `Feature::BLE::init()` binds it to the C6 controller. Therefore, do not transfer the usual local ESP32 WiFi RX/TX tips to the P4 without verification.

## 1. Baseline measurement before changing parameters

Reuse the existing diagnostics:

- [MemoryDiagnostics.cpp](../main/features/synth/MemoryDiagnostics.cpp), `snapshot()`, `captureReloadPhase()`, `setGraphStats()`.
- [synth_web_api.hpp](../main/features/synth/synth_web_api.hpp), `memory_diagnostics_handler()`, route `/api/diagnostics/memory`.
- Already included: free/largest/minimum internal heap and PSRAM, synth stack reserve, instance/block buffer sizes and PSRAM fallbacks.

Additionally for a measurement build: snapshots before/after hosted, BLE, WiFi, web and synth initialization; capture internal DMA-capable heap separately; add high-water marks for the remaining tasks. Flash/NVS calls and rare error paths belong to the stack measurement. Evaluate ESP-IDF stack sizes and the high-water marks used here in bytes.

| Variant | Radio state | Purpose |
| --- | --- | --- |
| A | WiFi off, BLE off | Base load; leave Ethernet/web/LVGL otherwise unchanged |
| B | WiFi on, BLE off | Isolate hosted + WiFi |
| C | WiFi off, BLE on | Isolate hosted + NimBLE |
| D | WiFi on, BLE on | Target operation; do not count shared hosted costs twice |

For each variant use the same board, preset, voice count, display state and USB devices. Record boot, idle, BLE scan, pairing, three connections, MIDI bursts, web upload/download, AP fallback, reconnect and preset change. In the state without web access, output the same values over serial. Plan at least 100 preset changes and 30 minutes of combined load as the first regression test; this does not replace a longer endurance test.

Always document `internalLargest`, `internalMinimum`, effective voice count, MIDI drops, render overruns, DMA underruns and render p99 alongside free bytes. The heap minima apply over the runtime so far: start comparison runs after a reboot in each case. More free bytes with fewer voices is not a gain.

For each candidate, create a separate build and compare `idf.py size`, `idf.py size-components` and, if needed, `idf.py size-files`. The current workspace contains no app mapfile directly under `build`; existing build metadata is no proof of the RAM usage of the current source state. Store settings permanently in `sdkconfig.defaults` or the matching board defaults; explicitly reconfigure the existing `sdkconfig` for test builds, since defaults do not simply overwrite existing values.

## 2. Prioritized measures

### P1 — Move NimBLE data specifically to PSRAM

**Finding:** `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y`; PSRAM is already enabled. `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y`, but `CONFIG_BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC` is off. NimBLE host stack: 4096 bytes.

**Work:** First test exclusively `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`. The locally available IDF Kconfig describes this selection for dynamic host allocations. Check the actual pool, peer and HCI allocation paths separately: the option moves neither all static data nor every hosted buffer across the board. Do not co-move the host stack and DMA buffers without evidence; BLE stores bonding data in NVS.

**Potential:** Share of the NimBLE dynamic data previously allocated internally; bytes must be measured. No fixed gain of, for example, "50 KB" is proven. Check whether the moved data contains the same pools that are to be reduced under P1 below.

**Acceptance:** Pairing/bonding across reboot, HID and MIDI simultaneously, maximum connection count, reconnect and WLAN load work without additional MIDI drops, cache errors or worse audio p99. PSRAM allocation failures must be handled in a controlled way.

### P1 — Adjust BLE pools to actual need

Current values from `sdkconfig`:

| Parameter | Current | Separate trial |
| --- | ---: | --- |
| `BT_NIMBLE_MAX_CONNECTIONS` | 3 | keep 3; 1/2 only as an explicitly restricted device profile |
| `BT_NIMBLE_MSYS_1_BLOCK_COUNT` / `BLOCK_SIZE` | 12 / 256 | keep for now |
| `BT_NIMBLE_MSYS_2_BLOCK_COUNT` / `BLOCK_SIZE` | 24 / 320 | count 16, then possibly 12 |
| `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` / `ACL_SIZE` | 24 / 255 | count 16; keep size for now |
| `BT_NIMBLE_TRANSPORT_EVT_COUNT` | 30 | 20 for now |
| `BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT` | 8 | keep for now; test scan bursts |
| `BT_NIMBLE_ATT_PREFERRED_MTU` | 256 | test 128 only later |
| `BT_NIMBLE_ATT_MAX_PREP_ENTRIES` | 64 | check use of prepare writes and the permissible Kconfig range |
| `BT_NIMBLE_GATT_MAX_PROCS` | 4 | reduce only after measuring parallel discovery/subscriptions |

All names in this table carry the prefix `CONFIG_` in `sdkconfig`. Confirm permissible values/dependencies in the IDF in use before each trial.

The two MSYS pools arithmetically give `12*256 + 24*320 = 10.752` bytes nominal block capacity. Reducing `MSYS_2` from 24 to 12 saves **3.840 bytes** therein, excluding management/alignment effects. ACL from 24 to 16 corresponds to **2.040 bytes** nominal payload capacity; events from 30 to 20 correspond to **700 bytes** at size 70. These figures are not measured internal heap gains and not an independent total: confirm the transport implementation, storage location and pool relationships first.

Reducing the MTU does not automatically save pool RAM as long as block sizes stay the same. It can affect larger BLE-MIDI messages and HID discovery. Therefore do not change pool count, MTU and block size at the same time.

`CONFIG_BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC=y` is a **downstream trial**: according to the local IDF Kconfig, blocks are then allocated/freed during use. This can lower idle consumption, but increases allocation activity and possible fragmentation; the load peak remains decisive. First prefer fixed, sufficiently sized pools.

### P1 — Limit WebSocket memory peaks

**Location:** [synth_web_api.hpp](../main/features/synth/synth_web_api.hpp), `websocket_handler()`, in particular `malloc(frame.len + 1)` around line 1633.

The length specified by the client is allocated before an upper bound. A cJSON tree is then created. The check against `sizeof(LiveParamMsg::json)` only happens when passing on to the engine and does not protect this first peak; locally handled message types have different requirements.

**Work:** Determine the protocol-wide maximum permissible length from all messages used, check it before addition/allocation and reject oversized frames in a controlled way or close the connection. Then explicitly allocate the payload in PSRAM, provided its use does not require cache-off access. Consider parser allocations separately. No per-request switched global cJSON hooks: other tasks may parse at the same time.

**Potential:** Load-dependent heap peak and fragmentation; no fixed permanent gain. With the current `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, small normal allocations prefer internal memory.

**Acceptance:** All valid web MIDI/parameter/device setting messages work; limit, oversize, invalid JSON and connection abort reliably free memory. Heap stable after repeated requests; no audio dropouts under request bursts.

### P2 — Reduce ESP-Hosted queues, keep DMA staging

**Finding:** `CONFIG_ESP_HOSTED_HOST_SDIO_TX_Q_SIZE=20`, `RX_Q_SIZE=20`, `RX_STAGING_SLOTS=2`; RPC at most 16 pending requests. Hosted stack configurations are 4096 bytes, external task stacks are off for hosted.

**Work:** In the hosted transport used, determine the actual queue elements, payload owners and allocation caps. Test TX/RX separately from 20 to 12. Reduce the RPC pending limit only after measuring the concurrent need of scan, BLE, status and OTA. No blanket calculation "40 slots times packet size": queues can only hold references; reserved memory and held payloads differ.

`RX_STAGING_SLOTS=2` is already the minimum according to the local `managed_components/espressif__esp_hosted/host/mcu/eh_host_mcu_transport/Kconfig.host.sdio`. Do not set it to 1. Shared SDIO must still serve BLE-MIDI during WiFi uploads. Lower throughput is acceptable only if UI and audio requirements remain met.

**Potential:** To be measured; queue management plus possibly fewer simultaneously held packets. Do not blindly move DMA/staging memory to PSRAM. Make changes preferably via public Kconfig options, not through unversioned edits under `managed_components`.

### P2 — Reduce unused BLE roles and discovery resources

**Finding:** Central, peripheral, broadcaster, observer as well as GATT client and server are enabled. The project describes its BLE operation in [Kconfig.projbuild](../main/Kconfig.projbuild) as central for HID/MIDI. [ble_manager.hpp](../main/features/ble/ble_manager.hpp), `init()`, calls `peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 32, 32)`.

**Work:** Disable peripheral/broadcaster and unnecessary local GATT server services individually after checking dependencies. Keep central, observer, GATT client, encryption, bonding and NVS persistence. The many enabled `BT_NIMBLE_*_SERVICE` options are only candidates: unreferenced services may already be removed by the linker.

For `peer_init`, first check the definition, allocation strategy and actual discovery utilization; do not multiply the numbers 20/32/32 by the connection count without evidence. Generic HID devices can need significantly more services/characteristics than pure MIDI devices. Smaller limits must cleanly terminate a failed discovery and release its resources.

**Potential:** Unknown; report flash and RAM deltas separately. Switching off security is not a memory optimization.

### P2 — Reduce task stacks after measurement

| Location | Reservation | Assessment |
| --- | ---: | --- |
| `web_server.hpp`, `Feature::Web::begin()` | 10.240 B | File explorer/upload need more than the standard stack according to the code |
| `display_lvgl.hpp`, `Feature::LVGL::init()` | 16.384 B | Consider PNG decoding and VFS as peak load |
| `synth_manager.hpp`, `preset_nav` | 4.096 B | Measure navigation and filesystem paths |
| `i2c_bus_manager.hpp`, worker | 4.096 B each | Per enabled bus; measure error/logging paths |
| NimBLE host | 4.096 B | Measure pairing, discovery, notifications and NVS |
| `app_startup.hpp`, `network_init` | 12.288 B | Boot peak only: task already deletes itself |
| `app_startup.hpp`, `sd_card_init` | 6.144 B | Evaluate boot reservation separately from permanent tasks |

First capture the reserves, then reduce candidates individually in small steps and keep a justified safety margin. Example: HTTP 10.240 to 8.192 bytes would save **2.048 bytes** permanently, but is not approved without a load measurement. Check large local JSON/upload buffers as a possible cause of the stack demand; moving them out only saves internal RAM if the reserved stack also becomes smaller.

The HTTP stack stays internal: `web_server.hpp` deliberately sets `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and documents the flash/cache error with a PSRAM stack. Check the same risk for NVS/filesystem paths of other tasks. `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` does not automatically move ordinary `xTaskCreate...` stacks. [ESP-IDF v6.1: External RAM](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-guides/external-ram.html).

### P2 — Create AP fallback only when needed

**Locations:** [wifi_manager.hpp](../main/features/wifi/wifi_manager.hpp), `init()`, `start_captive_portal()`, `stop_captive_portal()`.

The AP netif is created at startup when the captive portal function is enabled, although STA mode is used initially. On successful reconnect, DNS and AP mode are stopped; the netif remains. DNS already starts on demand; do not claim any further permanent consumption for it.

**Work:** Create the AP netif and possibly the retry timer only on fallback, and release them again after a clean end. Correctly unregister event handler/DHCP lifecycle; repeated create/delete must not produce leaks or races. First compare against a permanent netif: if the gain is small and fragmentation higher, keep the existing solution.

**Potential:** Small, not yet measured P4 gain in normal STA operation. Report AP radio resources on the C6 separately. Fully disabling the captive portal is a functional profile, not a transparent optimization.

### P3 — lwIP and global malloc policy only afterwards

`LWIP_MAX_SOCKETS=16`, `httpd.max_open_sockets=12`, TCP send buffer/window each 5760 bytes, IPv6 on. According to `web_server.hpp`, the socket count was deliberately raised because of parallel browser requests, WebSocket and watchdog problems. Therefore do not reduce it first. The TCP window may be tested separately at 4320 bytes; this is a trial value, not a guaranteed saving per connection. Ethernet is also enabled and uses lwIP as well.

`SPIRAM_MALLOC_ALWAYSINTERNAL=16384` may later be experimentally lowered to 4096, after allocations in audio, flash and driver paths have been checked. Explicit PSRAM allocations for known non-critical data are more targeted in the first step. `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` is not a proven "wasted 32 KiB buffer"; changing it creates no additional physical RAM. Keep DMA/stack reserves.

Disable IPv6 only in a profile explicitly intended as IPv4-only. `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` or targeted `EXT_RAM_BSS_ATTR` placement can move static, non-time-critical data; check the linker map first, then test cache-off paths. No blanket relocation of the audio state. The IDF documentation explains the malloc threshold, BSS placement and cache restrictions: [External RAM, v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-guides/external-ram.html).

## 3. Already implemented and further limits

- `Graph::allocate()` is already PSRAM-first according to [Graph.h](../main/audio/engine/Graph.h). Do not count this again as a new saving.
- Live parameter, MIDI debug and LVGL knob activity queues already use PSRAM (`synth_manager.hpp`, `SynthEngine::begin()`).
- `SynthEngine::loaderTaskLoop()` already requests a pre-free of the old graph on reload. A supposed general double allocation of old/new graphs would not be a new finding here. Examine timeout/error paths separately if diagnostics show peaks there.
- Display image cache uses PSRAM; certain MIPI variants have additional full-frame buffers. Reducing them is primarily PSRAM/bandwidth optimization and board-dependent. SPI buffers are DMA-marked; no blanket equal treatment.
- Do not push time-critical audio state, DSP tables and block buffers into PSRAM solely for a higher display of "free internal RAM". More cache competition with display and radio can worsen voice count and real-time capability.
- Browser JavaScript minification mainly saves browser/flash resources, not automatically P4 runtime RAM.
- An optional later radio-off mode requires a real stop/deinit lifecycle. Disconnect alone is no guaranteed memory release. The shared hosted transport must not be torn down while BLE or WiFi keeps running; consider Ethernet/web access. For the goal "both enabled", this is secondary for now.

## 4. Work plan

- [x] **M0 implementation:** Secure reference, fresh build/flash, add DMA/task diagnostics; A/C/D measured. B fails on the existing linker problem, see open acceptance.
- [x] **M1 implementation:** Test and adopt external NimBLE allocations; comparison with reference performed.
- [x] **M2 implementation:** MSYS2 16, ACL 16, events 20 built/flashed individually; available MIDI device works. Three devices and real WiFi data load remain open.
- [x] **M3:** WebSocket limit 8 KiB, payload/broadcast in PSRAM; additional preset upload as 1 KiB stream. Limit/abort tests passed.
- [x] **M4:** TX/RX each tested individually at 12; allocation paths checked. RPC 16 and DMA staging 2 remain.
- [x] **M5:** Peripheral/broadcaster disabled; central/observer/client and security retained. Shared peer pools 20/32/32 kept after review.
- [x] **M6:** Permanent task reserves measured; synth, loader, LVGL and HTTP reduced. Insufficiently loaded or tightly dimensioned tasks remain unchanged.
- [x] **M7 implementation:** Create AP netif on demand and release after reconnect. Boot measurement done; actual AP/STA switching remains open.
- [x] **M8 assessment:** Keep global malloc policy, lwIP and BSS unchanged after RAM gain; no proven benefit versus additional risks.
- [x] **M9 implementation:** Defaults updated, 100 identical reloads, comparison test, 30-minute run and controlled BLE reconnect passed. Additional hardware acceptance remains explicitly open below.

Every measure receives a result in this table; ineffective or unstable trials are discarded:

| Measure / configuration | Internal free before/after | Largest block before/after | Minimum under load | PSRAM delta | Voices / audio p99 / drops | Decision |
| --- | --- | --- | --- | --- | --- | --- |
| Reference / optimized, identical short load test | 51.515 / 110.179 B after load | 31.744 / 31.744 B in this comparison boot | 20 / 47.216 B | −21.064 B free | 3 voices; p99 median 2.176 / 2.112 µs; 0 MIDI drops | RAM gain confirmed; audio base load remains |

**Release criterion:** With identical preset and same voice count, the usable internal memory rises or the load peak falls reproducibly, without regression in MIDI, audio, UI, bonding, reconnect or flash access. Only then try higher voice counts.

## 5. Implementation and hardware measurements

### Measurement conditions

ESP32-P4 on `COM15`, web at `http://192.168.1.121`. Exclusively `FatSuperSaw`, unchanged preset file and three voices. Preset remains as the last loaded startup preset. All successful stages were compiled individually with `idf.py build`, uploaded with `idf.py -p COM15 app-flash` and then measured via `/api/diagnostics/memory`. LittleFS, web partition and NVS were not reflashed. Settings are stored permanently in `sdkconfig.defaults`; the existing `sdkconfig` was explicitly adjusted for trials.

WiFi and BLE are enabled in the final target profile. WLAN SSID is empty; web traffic runs over Ethernet. A stored BLE-MIDI device reconnects after restarts. New pairing, encryption proof and parallel HID/three-device use were not simulated. WiFi load on the C6 cannot be derived from Ethernet traffic.

Momentary values fluctuate by several KiB due to TCP/RPC buffers and connection setup. Table values are concrete snapshots, not individually additive savings. The heap minimum applies since boot; the original long-term value of 20 bytes was reported again under load even in the fresh reference comparison. `heap_caps_get_minimum_free_size()` adds the low points of individual regions that may have occurred at different times: a conservative lower bound, not proof that only 20 bytes were free overall at the same time. The largest internal block is optimized at 31.744 or 45.056 bytes depending on boot/allocation order; growth to 44 KiB is not guaranteed on every boot.

### Individual stages

All numbers in bytes. Build/flash logs and JSON snapshots for each successful stage are stored locally under `build/memory-audit/` (ignored working artifacts).

| Stage | Change | Internal free | Largest block | Previous minimum | PSRAM free | Build / upload |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 00 | Fresh baseline firmware | 54.491 | 31.744 | 41.160 | 26.271.352 | passed |
| 01 | Add DMA, MIDI/overrun and task diagnostics | 52.831 | 31.744 | 24.472 | 26.271.344 | passed |
| 02 | NimBLE `MEM_ALLOC_MODE_EXTERNAL` | 79.015 | 31.744 | 44.844 | 26.243.432 | passed |
| 03 | MSYS2 blocks 24 to 16 | 83.059 | 31.744 | 60.816 | 26.245.996 | passed |
| 04 | ACL RX blocks 24 to 16 | 83.039 | 31.744 | 60.816 | 26.248.428 | passed |
| 05 | Event blocks 30 to 20 | 83.075 | 31.744 | 59.276 | 26.249.312 | passed |
| 06 | WS payload/broadcast in PSRAM, limit 8 KiB | 83.075 | 31.744 | 60.816 | 26.249.312 | passed |
| 07 | Hosted TX queue 20 to 12 | 81.195 | 31.744 | 52.620 | 26.249.312 | passed |
| 08 | Hosted RX queue 20 to 12 | 81.839 | 31.744 | 52.620 | 26.249.312 | passed |
| 09 | Save preset: 1 KiB stream instead of whole body | 83.775 | 31.744 | 60.816 | 26.249.320 | passed |
| 10 | Disable BLE peripheral | 76.783 | 31.744 | 43.512 | 26.250.264 | passed |
| 11 | Disable BLE broadcaster | 84.335 | 31.744 | 60.832 | 26.250.276 | passed |
| 12 | Synth stack 32 to 12 KiB | 102.355 | 45.056 | 74.152 | 26.250.276 | passed |
| 13 | LVGL stack 16 to 12 KiB | 106.479 | 45.056 | 79.268 | 26.250.276 | passed |
| 14 | HTTP stack 10 to 8 KiB | 108.863 | 31.744 | 69.552 | 26.250.276 | passed |
| 15 | Loader stack 8 to 6 KiB | 110.551 | 45.056 | 75.216 | 26.250.276 | passed |
| 16 | Create AP netif only in fallback | 113.651 | 45.056 | 85.428 | 26.250.284 | passed |
| 17 | Check permanent defaults | 111.231 | 45.056 | 77.232 | 26.250.276 | passed |
| Final | Explicitly reconfigure target configuration | 110.815 | 45.056 | 79.268 | 26.250.276 | passed |

The largest permanent gain comes from the NimBLE relocation and stack reservations. Four stack reductions together save exactly **28.672 bytes of reservation**. The AP trial showed about 3 KiB difference in the boot snapshot; fluctuations prevent exact isolated attribution. Web changes mainly limit load peaks instead of delivering an additional fixed idle gain. Smaller BLE pools partly save PSRAM after relocation to PSRAM.

### Configuration and deliberately retained limits

- BLE: central, observer, GATT client, three connections, three bonds, MTU 256, security and NVS persistence remain active. Peripheral/broadcaster are dropped. MSYS1 stays 12 × 256; MSYS2 16 × 320; ACL 16 × 255; events 20 × 70; discard events 8; four GATT procedures. Prepare-write limit 64 unchanged; as central, no proven gain from further reduction.
- `peer_init(3, 20, 32, 32)` reserves shared service/characteristic/descriptor pools. No unproven multiplication by three peers. Limits remain for generic HID. Runtime mempool allocation stays off; fixed pools avoid additional heap activity in the MIDI path.
- Hosted: TX/RX 12 each. Queue elements contain management data/references; no proven gain from 40 full SDIO packets. SDIO DMA staging stays at the minimum of 2; RPC limit 16 and transport stacks 4 KiB remain. No unversioned changes in `managed_components`.
- HTTP stack stays internal. Payload and broadcast may lie in PSRAM; file write buffer stays internal due to flash/cache access. No switched global cJSON hooks.
- Preset POST writes to a temporary file and replaces the target only after complete reception and successful write/close. Abort/write error deletes the temporary file and keeps the original preset. Flash files that are in use need space; if space is short, an error is reported.
- AP netif uses the native IDF lifecycle. STA netif remains; IDF keeps its global handlers when the AP netif is deleted. Retry timer remains with a small fixed reservation. DNS was already on demand.
- Navigation stays at 4 KiB: the earlier 2 KiB crash is documented, navigation depth with the fixed preset was not stressed. I2C workers stay at 4 KiB each: full CV/error load is missing. NimBLE, hosted, USB and main stacks remain; some reserves are small or security/OTA/driver paths are untested.
- lwIP: 16 sockets, HTTP 12, TCP window/send buffer 5.760 bytes, IPv6 retained. malloc threshold 16 KiB and internal reserve 32 KiB remain. No blanket BSS/DSP relocation to PSRAM. The linker map shows static DIRAM consumption of 276.924 bytes, including 156.494 bytes of text; the reported linker remainder is not free runtime heap.

### Stack reserves under load

| Task | Reservation before / after | Smallest measured reserve after change |
| --- | ---: | ---: |
| Synth | 32.768 / 12.288 B | 9.292 B |
| Preset loader | 8.192 / 6.144 B | 2.212 B in endurance test; 3.188 B after isolated FatSuperSaw reloads |
| LVGL | 16.384 / 12.288 B | 7.560 B including splash PNG |
| HTTP | 10.240 / 8.192 B | 4.716 B including uploads |

These reserves refer to the specified preset and existing hardware. Other DSP graphs, USB audio mixing and full file explorer/HID/CV load require additional acceptance; do not reduce stack sizes further based on this single profile.

### Radio matrix after optimization

Ethernet, display, USB and `FatSuperSaw` remain the same. The matrix measures P4 RAM, not C6 RAM.

| Variant | Internal free | Largest block | Boot minimum | Result |
| --- | ---: | ---: | ---: | --- |
| A: WiFi/BLE off | 179.991 B | 49.152 B | 122.152 B | measured with temporarily conditionally registered NimBLE helper component |
| B: WiFi on, BLE off | — | — | — | linker error, no runtime measurement |
| C: WiFi off, BLE on | 116.595 B | 45.056 B | 85.968 B | build, flash and MIDI reconnect passed |
| D: WiFi/BLE on | 111.595 B | 45.056 B | 72.916 B | restored target build passed |

Fully disabled Bluetooth does not build with the original unconditional IDF example component: `host/ble_hs.h` is missing. Conditional local registration allowed measurement A, which was subsequently discarded. WiFi-only fails even with the originally registered component and merely disabled app BLE with `--enable-non-contiguous-regions discards section .bss.g_wifi_osi_funcs` / `unresolvable R_RISCV_HI20 relocation`. No unproven number for B. A future radio-off profile requires its own build/linker acceptance.

When restoring a configuration copy, `idf.py reconfigure` is explicitly executed: `Copy-Item` can preserve an old modification time, and Ninja would otherwise keep using stale generated configuration. Final generated headers confirm WiFi and app Bluetooth are enabled.

### Load and acceptance

**100 reloads:** exclusively `POST /api/preset/select?name=FatSuperSaw`, generation 3 to 103, all successful. Internal free before/after 111.259 / 110.195 bytes; PSRAM free identical 26.250.276 bytes; largest internal block 45.056 bytes; lifetime minimum 38.908 bytes. Three voices, 25 graph instances, 57 internal block buffers totaling 29.184 bytes, no output PSRAM fallbacks and no MIDI drops. Individual network buffers cause small momentary differences; no continuous graph/PSRAM loss proven.

**Web limits:** invalid JSON discards the message without a crash; an 8.192-byte text frame stays connected, an 8.193-byte frame closes the session. An aborted HTTP save preserves the original file byte-for-byte. A FatSuperSaw expanded by 96 KiB of whitespace, semantically identical, can be saved; afterwards the original bytes were restored. Tests passed again after the HTTP stack reduction.

**Direct reference comparison:** the stored reference contains additional diagnostics but no memory optimizations. Reference and final binary each freshly flashed to the app partition. Identical: 300 triad cycles with 70 ms note-on/30 ms pause, 20 preset downloads in parallel, then five reloads and two seconds of tail.

| Measurement | Reference | Optimized |
| --- | ---: | ---: |
| Internal free before / after test | 52.123 / 51.515 B | 110.771 / 110.179 B |
| Minimum since boot | 20 B | 47.216 B |
| Largest block after test | 31.744 B | 31.744 B |
| PSRAM free after test | 26.271.340 B | 26.250.276 B |
| Voices / graph instances | 3 / 25 | 3 / 25 |
| MIDI drops | 0 | 0 |
| New render overruns in test | 271 | 276 |
| Median of reported render p99 values | 2.176 µs | 2.112 µs |
| Mean of reported render p99 values | 2.377 µs | 2.395 µs |
| Largest reported p99 value | 5.888 µs | 6.656 µs |

**Assessment:** About 57.3 KiB additional internal heap in the comparable load state; about 20.6 KiB less free PSRAM. Mean audio p99 nearly identical; a single measurement provides no statistical proof of identical extremes. Audio overruns and DMA underruns already exist in the reference. The user explicitly accepted web-load underruns; these do not count as acceptance failures. Unplanned restarts, MIDI drops, voice reduction or PSRAM fallbacks remain acceptance failures.

**30-minute test:** second run successful over 1.801,6 seconds. 3.527 triad cycles, 873 preset downloads, 29 requested FatSuperSaw reloads and six BLE scans. No unplanned restarts, no MIDI drops, internal graph instances/block buffers throughout and no output PSRAM fallbacks. The first run was interrupted after 395 seconds by a manual reset confirmed by the user; it was not counted as a firmware crash. MIDI is injected via the existing web MIDI path; an actual BLE-MIDI burst from three devices is not replaced by this.

| Endurance test measurement | Result |
| --- | ---: |
| Internal free at start / end | 106.803 / 112.443 B |
| Range of minute snapshots | 104.539–112.867 B |
| Conservative heap lower bound since boot | 46.440 B |
| Largest internal block at end | 45.056 B |
| PSRAM free at start / end | 26.250.284 / 22.318.120 B |
| MIDI drops | 0 |
| Median of reported render p99 values | 2.112 µs |
| New render overruns during run | 5.214; including accepted web/reload load |

Measurement deviation: generation 1 to 31 contains one additional reload outside the 29 requested reloads. Generation 2 briefly reported five configured instead of three voices; at most three active voices were measured. Afterwards three configured voices again. PSRAM showed a one-time additional occupation of about 3.75 MiB, then a plateau with temporary reload fluctuations; no continuous internal heap loss. The cause of this additional runtime activity is not proven. Therefore read the endurance test as stability under combined activity; the strictly equal three-voice comparison is documented separately above. The stored FatSuperSaw file at the end is semantically identical to the original backup.

**BLE reconnect:** stored, connected MIDI device disconnected in a controlled way, disconnect confirmed, existing autoconnect re-enabled. MIDI connection restored after 1.046 ms; original auto setting retained, no bond deleted. Afterwards 110.871 bytes internal free, largest block 45.056 bytes, MIDI drops still 0 and identical internal graph.

**Build size:** `idf.py size` and `idf.py size-components` run. Reference binary with diagnostics 2.277.824 bytes, optimized 2.258.608 bytes, difference −19.216 bytes flash. Final static DIRAM consumption 276.924 bytes. Flash saving is not counted as additional heap gain. Failed variants were not uploaded; the confirmed target configuration is active again.

### Remaining hardware acceptance

- [x] Complete the 30-minute run, enter deviations and final values; check BLE reconnect.
- [ ] Three BLE devices, HID and MIDI in parallel, new pairing/bond key persistence and real BLE-MIDI bursts under WLAN upload. Hardware missing; three connections remain configured.
- [ ] Configure WLAN SSID, check real WiFi data load and repeated STA/AP fallback/reconnect cycles. No credentials available; Ethernet is no substitute for this test.
- [ ] Add WiFi-only runtime measurement; linker problem fixed in the addendum below.
- [ ] Additionally accept other large DSP presets, USB audio mixing, full CV/file explorer load and a longer endurance test.

**Final device check:** Final compilation successful; SHA-256 of the build binary matches the uploaded firmware. Web diagnostics: 113.043 bytes internal free (110.4 KiB), largest block 45.056 bytes (44 KiB), DMA-capable internal free 67.247 bytes; FatSuperSaw active, BLE ready and one stored auto device connected, MIDI drops 0. Momentary value fluctuates with network activity; comparable gain remains 57.3 KiB. Individual radio/hardware acceptances remain explicitly open above.

### Addendum: WiFi-only build error

After the audit, locally `CONFIG_APP_ENABLE_WIFI=y`, `CONFIG_APP_ENABLE_BLUETOOTH=n` and SDK Bluetooth remained enabled. This profile reproduced the linker error documented above. Cause: IDF 6.1 defines `g_wifi_osi_funcs` on P4 without native WiFi hardware as a weak object of an empty struct, i.e. with size zero. Binutils discards its section during linking with `--enable-non-contiguous-regions`; remaining references then cause RISC-V relocation errors. No proven RAM overflow.

`main/wifi_remote_compat.c` replaces the unused placeholder with a strong one-byte object only for remote WiFi without native WiFi hardware. Hosted transfers the configuration values via RPC, not an OSI function table. Native WiFi configurations keep their real table. The global RAM layout and SDK files remain unchanged. The file is registered via `main/CMakeLists.txt`. WiFi-only build and cross-check with WiFi + BLE successful. The original WiFi-only configuration was then restored and rebuilt; runtime measurement of matrix B remains open.

The existing build cache uses `C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe`. A shell with a different IDF Python environment reports a cache conflict separately. For verification, the same Python environment as in the existing cache was used.
