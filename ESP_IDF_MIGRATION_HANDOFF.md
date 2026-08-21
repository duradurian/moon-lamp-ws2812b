# Native ESP-IDF Migration Handoff

## 1. Architecture decision

Migrate the entire firmware repository from the current Arduino/PIOArduino hybrid to a native ESP-IDF project using ESP-Matter as a managed component.

The migration is complete only when the production build, application lifecycle, networking, storage, Matter endpoints, LEDs, Bluetooth audio, diagnostics, tests, and developer commands are all native ESP-IDF. Arduino must not remain as a component or compatibility layer in the final build.

This document is the architecture handoff. [PRD.md](PRD.md) remains the product-behavior and hardware acceptance contract. If an architectural direction in the PRD conflicts with this document, this document takes precedence; user-facing behavior must still follow the PRD.

## 2. Why this migration is required

The current application has outgrown the Arduino abstraction layer. It already uses native ESP-IDF Bluetooth, heap, and FreeRTOS APIs, while Arduino remains responsible for startup, Wi-Fi, Matter wrappers, NVS convenience APIs, LED timing, logging, and the main loop.

That split ownership has produced concrete problems:

- Arduino Wi-Fi and ESP-Matter can both initialize or drive the same `esp_netif`, causing duplicate-interface assertions or Network Commissioning events before the Matter data model is ready.
- The Arduino Matter backend initializes Bluetooth for BLE while Music Sync requires later Bluetooth Classic A2DP operation. Coordinating controller modes through the wrapper has caused mode mismatches and startup failures.
- PIOArduino's hybrid custom-`sdkconfig` build keeps parts of the precompiled Arduino link configuration. The current repository needs an explicit ROM-libc linker script to prevent an early cache-disabled startup panic.
- Arduino Matter force-links endpoint wrappers that retain clusters and client code the product does not use.
- The application needs a custom temporary-directory build wrapper because the current repository path contains spaces and ESP-IDF's CMake configuration rejects it.
- `src/main.cpp` is a 1,157-line monolith. Wi-Fi, Matter, Bluetooth, LED rendering, persistence, diagnostics, and state transitions cannot be tested or optimized independently.
- Arduino's loop task, serial layer, `Preferences`, Matter wrapper objects, Wi-Fi event bridge, and FastLED add RAM, flash, tasks, callbacks, and indirection that a nine-pixel product does not need.
- Wrapper APIs hide or discard useful return values. Native ESP-IDF APIs make startup failures explicit and allow one component to own each subsystem.

Native ESP-IDF will not make Matter cryptography itself dramatically faster, but it will remove framework duplication, reduce memory pressure, make task sizing deliberate, shorten hot paths, and eliminate the fragile Arduino-to-IDF boundary responsible for the current boot defects.

## 3. Product behavior that must not change

The migration is an architecture change, not a product redesign. Preserve all behavior defined in the PRD, especially:

- Classic ESP32-WROOM target, 4 MB flash, no PSRAM.
- Nine GRB WS2812 pixels on GPIO 13.
- Matter color-light control and a second Matter on/off control for Music Sync.
- Smooth 700 ms light fades and shortest-path hue interpolation.
- Fluid circular rainbow animation for the entire Wi-Fi connection period, followed by an approximately 900 ms fade to off.
- Selected HSV color persists; power state always boots off.
- Bluetooth Classic A2DP sink named `ESP32 Music Sync` starts only when Music Sync is enabled.
- Matter commissioning BLE owns BLE until commissioning is complete; Classic Bluetooth must not interfere with commissioning.
- Allocation-free, bounded PCM analysis and the existing lightweight energy/bass visualization.
- Matter remains responsive while Bluetooth starts, streams, disconnects, and restarts.
- USB diagnostics remain available at 115200 baud, including the `m` Music Sync toggle and `h` memory/stack report, unless equivalent commands are documented.

Do not erase the existing Matter fabric, Wi-Fi provisioning, or saved color during ordinary migration flashes. A factory reset remains an explicit, owner-approved test step.

## 4. Current baseline

As of 2026-08-18, the repository is still an Arduino ESP32 3.3.8 / PIOArduino project using ESP-IDF 5.5.4 libraries and ESP-Matter 1.4.1. The custom build is configured by `platformio.ini` and `sdkconfig.memory` and invoked through `scripts/pio-memory-build.sh`.

The freshly rebuilt and hardware-tested hybrid candidate provides this migration baseline:

| Metric | Hybrid baseline |
| --- | ---: |
| Static DRAM sections | 94,219 bytes |
| Total image | 2,258,350 bytes |
| IRAM | 96,167 bytes (73.37%) |

These figures came from `./scripts/pio-memory-build.sh -t upload --upload-port /dev/cu.usbserial-210` on 2026-08-18. The flash write and hash verification succeeded. A subsequent `POWERON_RESET` connected to Wi-Fi, acquired IPv4 and IPv6, restored both commissioned Matter endpoints, and remained stable beyond the earlier failure window without warnings, errors, panic, assertion, watchdog, or reboot. NVS and the existing Matter fabric were not erased.

The hybrid baseline now transitions the commissioned BTDM controller to Classic-only mode before starting A2DP. In the latest clean run, releasing unused BLE controller/host memory raised free internal heap from 18,888 to 92,176 bytes during that transition. With `ESP32 Music Sync` discoverable, completed startup heap was 19,876 bytes with an 18,420-byte largest block; after Matter subscription recovery and 60 seconds of concurrent advertising, it stabilized at 17,012 bytes with a 16,372-byte largest block and a 7,692-byte minimum-ever heap. The temporary startup task was reaped (17 live tasks to 16), and the 4 KiB BTC/BTU stacks retained 2,564 and 2,484 bytes of minimum headroom. Five rapid OFF/ON cycles then returned to 16,848 bytes, the same largest block, and 16 live tasks without an error, although the transient minimum-ever heap reached a tight 1,408 bytes. The final state was independently reported as `Connected` in macOS Bluetooth settings, and no Matter packet-buffer, DNS-SD, subscription, Bluetooth, panic, or reboot errors occurred during the accepted observation window.

Recent changes in the working tree address early ROM-libc startup, Matter endpoint capacity, Bluetooth controller mode, Wi-Fi/Matter event ordering, single-flight Bluetooth startup, partial-startup rollback, and runtime memory diagnostics. The clean boot above establishes a usable migration baseline, but it does not replace Phase 0's full timing, runtime-heap, stack high-water, endpoint-ID, repeated-boot, Bluetooth streaming, failure-injection, and cycle-stress captures. See [`MEMORY_LEAK_EVALUATION.md`](MEMORY_LEAK_EVALUATION.md) for the resolved findings and remaining proof obligations.

## 5. Target toolchain and build system

Start from the versions already represented in the working build:

- ESP-IDF 5.5.4
- ESP-Matter 1.4.1
- Target: `esp32`
- Build tool: `idf.py` and ESP-IDF CMake
- Dependency management: ESP-IDF Component Manager with committed `dependencies.lock`

Pin exact versions before porting behavior. Do not combine the architecture migration with a framework-version upgrade. A later ESP-Matter update, including 1.4.2 or newer, requires its own changelog review and hardware acceptance run.

The final repository must build from a path without spaces. Rename or clone it to a path such as `esp32-music-sync` before removing the temporary wrapper. Do not preserve a permanent build workaround solely for the current `untitled folder/esp32 test` path.

Expected final commands:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-210 flash
idf.py -p /dev/cu.usbserial-210 monitor
```

The serial port is not stable and must be rediscovered when absent. Never put `erase-flash` into a normal build, flash, or test command.

## 6. Target repository layout

Use independently owned components rather than another monolithic `app_main.cpp`:

```text
.
├── CMakeLists.txt
├── README.md
├── PRD.md
├── ESP_IDF_MIGRATION_HANDOFF.md
├── dependencies.lock
├── partitions.csv
├── sdkconfig.defaults
├── main
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── app_main.cpp
└── components
    ├── app_state
    ├── bluetooth_audio
    ├── diagnostics
    ├── led_renderer
    ├── matter_node
    ├── persistent_store
    └── wifi_manager
```

Component boundaries may be adjusted, but ownership must remain unambiguous:

- `app_state`: product state machine and cross-component event definitions.
- `wifi_manager`: Wi-Fi lifecycle, IPv4/IPv6 event handling, retry policy, and connection-state publication.
- `matter_node`: root node, endpoint creation, Matter callbacks, commissioning events, and attribute synchronization.
- `bluetooth_audio`: controller/host/A2DP lifecycle, pairing state, PCM callback, and reconnect behavior.
- `led_renderer`: the only component allowed to write LED frames; owns animation timing and transitions.
- `persistent_store`: NVS schema and saved HSV access.
- `diagnostics`: heap, largest block, task high-water marks, reset reason, and serial command handling.

Keep public component headers small. Avoid circular dependencies by communicating through typed events and immutable state snapshots rather than direct component-to-component calls.

## 7. Arduino-to-ESP-IDF replacement map

| Current dependency | Native replacement | Migration notes |
| --- | --- | --- |
| `Arduino.h`, `setup()`, `loop()` | `app_main()`, FreeRTOS tasks, queues, and timers | No permanent polling super-loop. Use a product event loop/state machine. |
| `millis()` and `delay()` | `esp_timer_get_time()`, `vTaskDelay()`, `vTaskDelayUntil()` | Use monotonic microseconds and wrap-safe duration helpers. |
| `Serial` | `ESP_LOGx`, UART/VFS, or a small console task | Keep diagnostics at 115200 and never log secrets. |
| `WiFi.h` | `esp_wifi`, `esp_netif`, ESP event loop | Exactly one component owns station initialization and reconnect policy. |
| `Preferences` | `nvs_flash` and `nvs` | Read the existing `color-light` namespace and `hsv` `u32` key to preserve saved color. |
| `Matter.h` wrappers | native `esp_matter` node, endpoint, cluster, attribute, and event APIs | Preserve endpoint identities and Google Home semantics. Check every return value. |
| `MatterColorLight` | native extended-color-light endpoint | Implement on/off, level, hue, and saturation callbacks with the existing fade state machine. |
| `MatterOnOffPlugin` | native on/off plug-in-unit endpoint | Preserve Music Sync as a separate user-operable endpoint. |
| FastLED | ESP-IDF `led_strip` RMT backend or a product-sized RMT encoder | Only nine pixels are required. Keep GRB order and existing HSV behavior. |
| Arduino Bluetooth helpers | `esp_bt_controller`, Bluedroid, GAP, and A2DP APIs | Most current A2DP code is already native and should be moved, not rewritten conceptually. |
| PlatformIO libraries and flags | ESP-IDF components and Kconfig | No Arduino framework, PIOArduino package, or Arduino linker-response workaround at cutover. |
| `include/secrets.h` | Matter Wi-Fi provisioning plus NVS | Use Matter as the production credential owner. Keep the private header only as a temporary migration fallback and never commit or print it. |

## 8. Target runtime architecture

### 8.1 Boot sequence

Use one explicit, error-checked boot sequence:

1. Print reset reason and firmware version.
2. Initialize NVS without erasing it. Handle only the documented recoverable NVS initialization cases, and require explicit approval before any destructive recovery.
3. Initialize the LED renderer and immediately start the boot rainbow.
4. Create the native ESP-Matter node and both application endpoints.
5. Initialize the Bluetooth controller in the mode required to support Matter BLE followed by Classic A2DP. Matter owns BLE commissioning until the appropriate deinitialization/commissioning event.
6. Start ESP-Matter and let its ESP-IDF platform layer own the Wi-Fi stack and `esp_netif` lifecycle.
7. Continue the rainbow from a renderer timer/task while Wi-Fi associates. Do not block the Matter event task or LED renderer in a connection loop.
8. On IPv6 link-local availability and operational Matter readiness, fade the boot animation to off.
9. Restore the selected HSV value but never restore power-on state.
10. If already commissioned, synchronize endpoint attributes only after the Matter server and network are ready.

There must be no competing Arduino and Matter Wi-Fi owners, no duplicate default netif creation, and no application callback that performs long work on the Matter platform thread.

### 8.2 Event flow

Use bounded queues or task notifications for cross-component work:

```text
ESP-IDF Wi-Fi/Matter events ──> app_state ──> LED render commands
                                      └────> Matter readiness/state

Matter attribute callbacks ──> app_state ──> fade targets
                                      └────> Bluetooth start/stop request

A2DP callbacks ───────────────> fixed-size audio snapshot ──> LED renderer
```

Callbacks must not allocate, wait for network state, perform LED I/O, initialize Bluetooth, or write NVS. They should copy bounded data or set a task notification and return.

### 8.3 LED rendering

Prefer the ESP-IDF `led_strip` component with the RMT backend. If its measured memory footprint is excessive for nine pixels, implement a small fixed-buffer RMT encoder as an isolated component.

Requirements:

- One renderer task owns the strip and refreshes at approximately 60 Hz using `vTaskDelayUntil()` or `esp_timer` scheduling.
- Keep all long-lived frame and animation state statically allocated.
- Port the current HSV-to-RGB appearance deliberately; validate representative colors because FastLED and ESP-IDF conversion curves may differ.
- Preserve exactly one full hue wheel across nine pixels and a seamless pixel-9-to-pixel-1 boundary.
- Avoid floating-point work in the frame path.
- No LED refresh may block Matter or Bluetooth callbacks.

### 8.4 Matter data model

Build the data model directly with ESP-Matter:

- Root node on endpoint 0.
- Extended color light as the first application endpoint.
- On/off plug-in unit for Music Sync as the second application endpoint.
- Configure capacity for the root plus two application endpoints; the current hybrid build requires `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=3`.
- Preserve the current vendor/product identity, discriminator, setup PIN policy, device types, endpoint order, and cluster semantics unless an intentional recommissioning migration is approved.
- Preserve endpoint IDs across the cutover. Determine the actual IDs from the current device and use the supported ESP-Matter resume/persistence mechanism; do not assume new dynamic creation will be invisible to an existing Google Home fabric.
- Keep attribute callbacks short. Queue fade and Bluetooth requests to application tasks.
- Make endpoint-creation and `esp_matter::start()` failures fatal to application startup with a clear serial reason and a safe LEDs-off state.

### 8.5 Wi-Fi, IPv6, mDNS, and time

- ESP-Matter/ESP-IDF is the only Wi-Fi owner.
- Use Matter-provisioned credentials in production. A compile-time SSID/password is a temporary transition aid, not the target architecture.
- Use ESP event handlers for station state, IP assignment, and retry timing.
- Keep retries asynchronous and bounded; never stop the boot animation while waiting.
- Require an IPv6 link-local address before declaring Matter operational.
- Use the CHIP minimal mDNS backend when supported and validated for this target to avoid duplicate ESP mDNS service ownership.
- Enable SNTP/system real-time support so restored CASE sessions do not emit a real-clock fallback error.

### 8.6 Bluetooth Classic and Matter BLE coexistence

Preserve the existing native A2DP design and formalize ownership:

- One Bluetooth task owns controller, Bluedroid, GAP, and A2DP state transitions.
- Initialize the controller with a mode compatible with both Matter BLE commissioning and later Classic Bluetooth use.
- Do not start A2DP until the device is commissioned and Matter has released or deinitialized BLE as required by the selected ESP-Matter configuration.
- Keep one Classic ACL connection and one BLE commissioning connection.
- Keep AVRCP, cover art, SPP, HFP, BLUFI, BLE Mesh, I2S output, and unrelated profiles disabled.
- The PCM callback remains allocation-free and bounded to a fixed maximum number of analyzed frames.
- A start/stop request has an explicit state machine and deadline. A timeout must leave Matter and normal lighting usable.
- Verify whether BLE controller memory can safely be released in the chosen BTDM lifecycle. Do not release memory that Classic operation or later commissioning still needs.

## 9. Kconfig and partition strategy

Create a small, reviewed `sdkconfig.defaults`; do not carry forward the current generated, thousands-of-lines `sdkconfig.defaults` as the native source of truth.

Port relevant settings from `sdkconfig.memory`, including:

- ESP-Matter data model and the exact endpoint/device-type capacities.
- 6 KiB CHIP task as the initial value, subject to measured high-water evidence.
- One BLE and one Classic ACL connection.
- Required A2DP, Bluedroid, coexistence, and Matter BLE settings.
- Disabled unused Bluetooth profiles, diagnostics, telemetry, and clusters.
- No PSRAM and no PSRAM cache workaround.
- Bounded Wi-Fi dynamic buffers.
- Minimal Matter mDNS and SNTP time support.
- Stack overflow checking, heap poisoning appropriate for development builds, and task high-water diagnostics.

Do not copy the Arduino-specific ROM-libc linker workaround into the native build. A coherent ESP-IDF link should select the correct ROM functions itself. If it does not, stop and diagnose the native configuration rather than hiding the failure with the old PIOArduino workaround.

Initially preserve the current partition offsets and sizes exactly:

```text
nvs       0x009000  0x005000
otadata   0x00e000  0x002000
app0      0x010000  0x300000
spiffs    0x310000  0x0e0000
coredump  0x3f0000  0x010000
```

Changing the NVS offset or erasing NVS can destroy the Matter fabric and Wi-Fi configuration. Partition redesign, real dual-slot OTA, and removal of unused SPIFFS are worthwhile later optimizations, but they are separate changes after functional parity.

## 10. Memory and performance objectives

The native migration must produce measured improvement, not merely different source code.

### 10.1 Required measurements

Capture all measurements for both the final hybrid baseline and each native milestone:

- Static DRAM, IRAM, and flash from the linker/map report.
- Free internal heap and largest free internal block at:
  - early boot;
  - Matter ready;
  - immediately before Bluetooth startup;
  - controller ready;
  - Bluedroid ready;
  - A2DP ready;
  - active streaming;
  - Bluetooth stopped/disconnected.
- Minimum stack headroom for every application, CHIP, Wi-Fi, BTU, BTC, and A2DP task.
- Reset-to-Wi-Fi-connected, reset-to-IPv6, and reset-to-Matter-operational timings.
- LED frame period and missed-frame count during Matter traffic and Bluetooth streaming.
- Heap/largest-block deltas across 20 Music Sync and 20 Bluetooth reconnect cycles.

### 10.2 Acceptance targets

- Native static DRAM and flash must both be lower than the freshly measured hybrid baseline. If either is larger, document the exact component cost and continue optimizing before cutover.
- Remove the permanent Arduino loop task and all Arduino wrapper tasks/callback bridges.
- Size each application stack from worst-case high-water data plus a documented safety margin; do not shrink stacks by guesswork.
- Post-Matter and post-A2DP largest free blocks must be at least as large as the hybrid baseline and stable across stress cycles.
- Median and 95th-percentile native boot milestones must be no slower than the hybrid baseline under the same AP and test setup.
- LED rendering must sustain the existing approximately 60 Hz cadence without starving Matter or Bluetooth.
- No unbounded waits, repeated dynamic allocation in steady state, or floating-point work in audio/LED hot paths.

Prefer component-level size/performance tuning over enabling global performance optimization without measuring the flash cost. Keep interrupt/cache-disabled code in IRAM only when required; ordinary logic should remain in flash.

## 11. Migration phases

### Phase 0: freeze behavior and collect a golden baseline

- Preserve the current Arduino candidate on a branch or tag.
- Build it from a clean temporary directory and retain the ELF, map, binaries, full configuration, and serial log.
- Perform a cold boot without erasing NVS.
- Record endpoint IDs, device identity, saved color behavior, boot animation, Wi-Fi/IPv6 timings, Matter readiness, and Bluetooth diagnostics.
- Record all memory and stack checkpoints listed above.
- Do not proceed using an unstable or undocumented baseline.

### Phase 1: create the native skeleton

- Convert the root CMake project to a normal native ESP-IDF application.
- Add ESP-Matter through `main/idf_component.yml` and commit the dependency lock.
- Add the preserved partition table and a curated `sdkconfig.defaults`.
- Implement `app_main()`, NVS initialization, reset diagnostics, and a minimal serial log.
- Prove `idf.py build`, flash, and a clean 60-second hardware boot before adding features.

### Phase 2: port LEDs, timing, storage, and diagnostics

- Implement the native RMT LED component and all static animations.
- Port the boot rainbow, fade-out, normal fade state, pairing pulse, and audio visualization.
- Port the existing NVS namespace/key so saved color survives the architecture change.
- Port `m` and `h` diagnostics.
- Validate LEDs without Matter or Bluetooth complexity.

### Phase 3: port Wi-Fi and native Matter

- Add the native node and both endpoints.
- Establish one Wi-Fi/netif owner and event-driven IPv6 readiness.
- Preserve the existing fabric, endpoint IDs, and user-visible Google Home controls.
- Verify light power, level, hue, saturation, persistence, and boot-off behavior.
- Require clean boot logs before moving to Bluetooth.

### Phase 4: port Bluetooth audio and coexistence

- Move the current native controller, Bluedroid, GAP, A2DP, PCM, and reconnect logic into `bluetooth_audio`.
- Gate Classic startup on the correct Matter commissioning/BLE lifecycle event.
- Validate discovery, pairing, streaming, disconnect, re-advertising, stop, and restart.
- Measure heap, largest block, and all Bluetooth-related stack high-water marks.

### Phase 5: integrate and optimize

- Exercise Matter commands during Bluetooth startup and active streaming.
- Remove duplicate buffers, tasks, logs, and unused components based on map/runtime evidence.
- Tune task priorities and stacks from measurements.
- Run cold-boot, control-cycle, reconnect, and long-stream acceptance suites.
- Compare native results against the frozen hybrid baseline.

### Phase 6: cut over the repository

Only after full hardware parity:

- Remove `platformio.ini`.
- Remove `scripts/pio-memory-build.sh`.
- Remove the Arduino-specific `sdkconfig.memory` merge workflow.
- Remove all Arduino, PIOArduino, FastLED, `Matter.h`, `WiFi.h`, and `Preferences` dependencies.
- Remove linker and logging wrappers that existed only for PIOArduino.
- Replace old build instructions in the PRD/README with `idf.py` commands.
- Retain the archived/tagged Arduino implementation for history, not in the production build graph.

## 12. Verification gates

Every phase must pass its own build and physical boot gate. Do not defer boot validation until the end.

### 12.1 Clean boot gate

For the final native firmware:

- Flash without erasing NVS.
- Perform a real reset/cold boot while monitoring at 115200 baud.
- Capture at least 60 seconds of serial output and extend through controller reconnection activity.
- Observe the rainbow for the full Wi-Fi connection interval and its fade to off.
- Confirm Wi-Fi, IPv6 link-local, Matter server readiness, and both endpoint states.
- Reject any boot containing `Guru Meditation`, panic, assertion, watchdog, brownout, stack overflow, reboot loop, failed endpoint creation, Bluetooth controller/host failure, packet-buffer exhaustion, or any unexplained `E`/error log.
- Complete 20 consecutive cold boots with no error.

Known framework messages are not automatically acceptable. Either fix their cause, configure the correct supported backend, or document with upstream evidence why they are harmless and obtain owner acceptance. Do not merely suppress error logs to make the trace look clean.

### 12.2 Functional and stress gate

Run every acceptance criterion in PRD sections 10 and 13, including:

- Matter light on/off, brightness, and representative colors.
- Selected color retained and power always off after reboot.
- Music Sync control through Matter and the serial diagnostic path.
- Bluetooth discovery within five seconds, pairing, PCM delivery, LED reaction, disconnect, and re-advertising.
- 20 Music Sync on/off cycles.
- 20 Bluetooth connect/disconnect cycles.
- Matter commands during Bluetooth startup and active audio.
- At least 60 minutes of streaming with stable heap and largest block.

## 13. Risks and controls

| Risk | Control |
| --- | --- |
| Existing Google Home fabric is lost | Preserve partition offsets, never use routine flash erase, and back up/capture NVS only with owner approval and secure handling. |
| Endpoint IDs change and subscriptions break | Record current endpoint IDs and use ESP-Matter's supported persistent/resume mechanism before testing on the existing fabric. |
| LED colors differ after removing FastLED | Create golden HSV/RGB samples and visually compare representative colors and seam continuity. |
| Native Matter adds unfamiliar lifecycle races | Use one network owner, short callbacks, explicit readiness events, and a boot test after each milestone. |
| Bluetooth memory is released too early | Gate Classic startup from documented Matter BLE events and verify controller/host state before every transition. |
| Aggressive stack reduction causes rare crashes | Use worst-case high-water measurements and retain explicit safety margins. |
| A big-bang rewrite obscures regressions | Keep each phase buildable and hardware-testable; port one subsystem at a time against the golden baseline. |
| Toolchain upgrades change behavior during port | Pin ESP-IDF and ESP-Matter first; upgrade only after native parity. |
| Secrets leak into build logs or commits | Never print `include/secrets.h`; move production credentials to Matter/NVS provisioning. |

## 14. Required migration deliverables

The follow-on engineer or agent must hand back:

- Native ESP-IDF source organized by component.
- Pinned ESP-Matter dependency manifest and lock file.
- Curated `sdkconfig.defaults` and preserved partition table.
- Updated build/flash/monitor documentation.
- A baseline-versus-native memory and timing report.
- Complete serial evidence for a clean boot and the stress suite.
- Recorded endpoint/device identity and confirmation that the existing Matter fabric remained usable, or explicit owner-approved recommissioning notes.
- A list of every removed Arduino/PIOArduino dependency and its native replacement.
- No production references to Arduino or PlatformIO in the build graph.

## 15. Definition of done

The repository is considered migrated only when:

1. A clean clone in a no-space path builds with `idf.py build` using pinned dependencies.
2. Ordinary flashing preserves NVS, Matter fabric, Wi-Fi configuration, and selected color.
3. The physical ESP32 completes the clean-boot and full PRD acceptance suites without error.
4. Matter lighting and Music Sync behave the same from the user's perspective.
5. Native memory, largest-block, and boot-performance measurements meet section 10 targets.
6. Arduino, PIOArduino, PlatformIO, FastLED, and Arduino Matter/Wi-Fi/Preferences layers are absent from the production build.
7. Obsolete hybrid-build files are removed only after all preceding gates pass.

Build success alone is not completion. The final proof is a clean physical boot, preserved Matter operation, working Bluetooth audio, correct LED behavior, and measured memory/performance improvement on the actual ESP32-WROOM.
