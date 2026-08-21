# ESP32 Matter Color Light with Music Sync

## 1. Purpose of this document

This is the product and engineering handoff for an ESP32-WROOM lighting project. A follow-on agent should use it to understand the intended behavior, reproduce the original failure, and validate the optimized candidate on real hardware.

> Architecture direction: the repository is to be migrated completely from the current Arduino/PIOArduino hybrid to native ESP-IDF. See [ESP_IDF_MIGRATION_HANDOFF.md](ESP_IDF_MIGRATION_HANDOFF.md). This PRD remains the product-behavior contract.

The firmware is not yet product-complete. A custom memory-optimized framework build now compiles successfully, but Bluetooth Music Sync and the complete acceptance suite still require physical-device validation. The original precompiled build exhausted internal RAM before becoming discoverable.

## 2. Product summary

The product is a nine-pixel circular WS2812 light controlled through Google Home as a Matter device.

It has two user-facing functions:

1. A normal Matter color light with power, brightness, and color controls.
2. A Google Home-controlled Music Sync mode. Enabling this mode starts a Bluetooth Classic A2DP sink named `ESP32 Music Sync`. A phone can connect and stream audio, and the LEDs render a circular rainbow wave driven by the audio energy and bass.

The ESP32 only analyzes the received PCM stream. It does not play the audio through a speaker or I2S output, so a phone connected to the ESP32 may produce no audible sound unless audio is routed elsewhere by the source device.

## 3. Hardware

- MCU: classic ESP32-WROOM, observed as ESP32-D0WD-V3 revision 3.1
- Flash: 4 MB
- PSRAM: none
- LEDs: 9 WS2812-compatible RGB LEDs
- LED data pin: GPIO 13 (`D13`)
- Pixel order: GRB
- Physical arrangement: circle; LED 1 and LED 9 are adjacent
- Serial monitor: 115200 baud
- Last observed macOS serial port: `/dev/cu.usbserial-210`; rediscover it rather than assuming it is unchanged

Electrical assumptions to verify before extended testing:

- ESP32 and LED supply share ground.
- The LED supply can safely power all nine pixels at the configured brightness.
- GPIO 13 reaches the first LED data input.
- A level shifter is recommended if the LED data signal is unreliable at 3.3 V.

An ESP32-S3 is not a drop-in replacement for this design because it does not support Bluetooth Classic A2DP.

## 4. Repository layout

- `src/main.cpp`: all current firmware behavior
- `platformio.ini`: current Arduino/PlatformIO build
- `include/secrets.h`: real Wi-Fi credentials; do not print, commit, replace, or disclose this file
- `include/secrets.example.h`: credential template

Current framework versions resolved by the build:

- PIOArduino Espressif32 platform 55.03.38-1
- Arduino ESP32 3.3.8
- ESP-IDF libraries 5.5.4
- FastLED 3.10.3

## 5. User-facing requirements

### 5.1 Matter lighting

- The device must commission into Google Home as a Matter color light.
- Google Home must control on/off, brightness, and color.
- Power changes must fade smoothly over approximately 700 ms.
- Color and brightness changes must interpolate smoothly over approximately 700 ms.
- Hue transitions must take the shortest direction around the hue wheel.
- The selected color should be retained across reboot.
- The on/off state must not be retained across reboot.
- After any boot or power restoration, the normal light must start off.
- Matter configuration and Wi-Fi secrets must survive ordinary firmware updates.
- A factory reset must only occur as an explicit test step; it requires recommissioning.

### 5.2 Boot presentation

- While waiting for Wi-Fi, all nine LEDs display a fluid, multicolor circular portal/wave animation.
- The boot animation continues until Wi-Fi connects.
- Once Wi-Fi connects, the animation fades out elegantly over approximately 900 ms.
- The normal light remains off after the boot animation.
- Wi-Fi retry behavior must not permanently block boot.
- Matter must not start until an IPv6 link-local address is available.

### 5.3 Music Sync control

- Music Sync must be exposed in Google Home as a user-operable Matter control. The current implementation uses a second `MatterOnOffPlugin` endpoint.
- Music Sync must not start automatically after boot.
- Enabling Music Sync must not block, reset, or make Matter unresponsive.
- Disabling Music Sync must stop accepting Bluetooth connections and restore the normal light rendering.
- Repeated on/off cycles must not fragment memory or degrade Matter.

### 5.4 Bluetooth behavior

- Music Sync uses Bluetooth Classic A2DP sink mode.
- Advertised device name: `ESP32 Music Sync`.
- The device is headless and uses Secure Simple Pairing/Just Works.
- Pairing mode is indicated by a smooth blue breathing pulse on all nine LEDs.
- Once a source connects, the pairing pulse ends and the music visualization begins.
- If the source disconnects or unpairs while Music Sync remains enabled, the ESP32 must immediately become connectable and discoverable again.
- Only one Bluetooth audio source is required.
- AVRCP controls, metadata, cover art, SPP, I2S output, and audio playback are not required.

### 5.5 Music visualization

- All nine LEDs must represent one complete hue spectrum at every frame.
- Hue spacing must be exactly one ninth of the FastLED hue wheel per LED so the physical LED 9-to-LED 1 seam is continuous.
- The rainbow rotates continuously around the circle.
- Overall movement and brightness accelerate with audio energy.
- Bass produces a moving brightness wave and beat pulse.
- The implementation should remain lightweight; the current design uses average PCM energy and a simple low-pass filter rather than an FFT.
- Silence or a stopped stream should decay smoothly instead of leaving stale peaks.

## 6. Non-functional requirements

- Matter commands must remain responsive while Music Sync starts and while audio is streaming.
- No operation may contain an unbounded retry loop on the Arduino loop/Matter execution path.
- Bluetooth initialization failures must fail safely and leave the normal Matter light usable.
- The firmware must recover cleanly after power loss in every mode.
- There must be sufficient internal heap headroom for Matter packet buffers after Bluetooth is fully initialized.
- Validation must measure both total free internal heap and the largest free internal block. Total heap alone is insufficient because fragmentation matters.
- No `PacketBuffer: pool EMPTY`, watchdog, brownout, panic, stack-overflow, or subscription-load errors are acceptable in the final build.

## 7. Current implementation

The current code already implements:

- Matter color light and a second Music Sync Matter endpoint
- Persistent selected HSV color but non-persistent power state
- Boot Wi-Fi animation and fade-out
- Normal on/off, brightness, and color fades
- Nine-LED circular rainbow music renderer
- Blue Bluetooth-pairing pulse
- Re-advertising request after A2DP disconnect
- Direct ESP-IDF A2DP calls without the third-party ESP32-A2DP wrapper
- No AVRCP or I2S initialization in application code
- Bluetooth startup in a separate FreeRTOS task
- Internal heap/largest-block logging
- An eight-second application-level startup deadline
- A USB diagnostic command: sending `m` or `M` at 115200 baud mirrors the Music Sync switch

The current `platformio.ini` intentionally has only FastLED as an external dependency. ESP-IDF Bluetooth APIs are called directly from `src/main.cpp`.

The optimized candidate additionally implements:

- A source-built ESP-IDF/ESP-Matter configuration in `sdkconfig.memory`
- Two dynamic Matter endpoint/device-type slots instead of the upstream 32/16 defaults
- A 6 KiB CHIP task, 4 KiB Arduino loop task, and smaller timer/infrastructure tasks
- One Classic ACL connection and one BLE commissioning connection
- Disabled unused Bluetooth profiles, ESP Insights, Matter telemetry, and unused clusters
- BLE-only Matter commissioning followed by irreversible BLE-memory release before Classic Bluetooth starts
- Static lifetime for long-lived application objects and allocation-free PCM analysis
- Fixed-point music dynamics with no floating-point work in the render loop
- Runtime heap, largest-block, and task stack high-water diagnostics on serial command `h`
- A no-space temporary build wrapper because ESP-IDF rejects this repository's current path containing spaces

Clean build comparison on 2026-08-18:

| Metric | Original precompiled build | Optimized production build | Reduction |
| --- | ---: | ---: | ---: |
| Static RAM | 124,544 bytes | 92,532 bytes | 32,012 bytes (25.7%) |
| Flash | 2,559,248 bytes | 2,265,290 bytes | 293,958 bytes (11.5%) |

The successful build artifacts are copied to `.pio/memory-build/`. These link-time improvements are not substitutes for the runtime heap and stress evidence required by section 10.

## 8. Original confirmed blocking issue

### 8.1 Reproduction

On the pre-optimization firmware:

1. Boot and allow Matter to reconnect.
2. Open a serial monitor at 115200 baud.
3. Enable Music Sync in Google Home, or send `m` over USB serial.
4. Observe the startup log.

### 8.2 Observed log and memory

The latest pre-optimization hardware test reached these checkpoints:

```text
Before Bluetooth startup: internal heap=27732, largest block=25588
Bluetooth controller initial status: 0
Initializing the Classic Bluetooth controller...
Enabling the Classic Bluetooth controller...
After controller startup: internal heap=17684, largest block=16372
```

The next call was `esp_bluedroid_init_with_cfg()`. It did not return. The A2DP sink was therefore never initialized, scan mode was never enabled, and `ESP32 Music Sync` never appeared in a Bluetooth scan.

At the same time Matter reports errors including:

```text
chip[CSL]: PacketBuffer: pool EMPTY
chip[IN]: SendMessage() ... failed
chip[DMG]: Failed to load subscription ...
```

The eight-second deadline updates application state, but it cannot cancel the FreeRTOS task blocked inside the IDF call. A processor reset is currently required to fully recover the consumed resources. Do not consider the current timeout a complete recovery mechanism.

### 8.3 Root cause

The original Arduino framework build used precompiled ESP-IDF and ESP-Matter libraries with memory-heavy defaults. Relevant observed configuration included:

```text
CONFIG_BT_BTC_TASK_STACK_SIZE=8192
CONFIG_BT_BTU_TASK_STACK_SIZE=8192
CONFIG_BT_ACL_CONNECTIONS=4
CONFIG_BTDM_CTRL_BR_EDR_MAX_ACL_CONN=2
CONFIG_BT_AVRCP_ENABLED=y
CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED=y
CONFIG_CHIP_TASK_STACK_SIZE=8192
CONFIG_ESP_MATTER_MAX_DEVICE_TYPE_COUNT=16
CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=32
```

These settings could not be meaningfully corrected with application `build_flags` because the affected libraries were already compiled. This is why the optimized candidate now rebuilds ESP-IDF and ESP-Matter from source.

## 9. Implemented direction and remaining decision

The preferred custom ESP-IDF plus ESP-Matter build is implemented for the same ESP32-WROOM. It preserves the product behavior while compiling the frameworks with the deliberately small `sdkconfig.memory` profile.

The implementation now covers the following items, but each still requires hardware validation:

- Matter dynamic endpoint and device-type capacities are both 2; the root endpoint is static and excluded from these dynamic capacities.
- The CHIP task uses Espressif's documented 6 KiB small-device profile. Serial command `h` reports its high-water mark, and this value must be increased if the PRD stress suite does not leave a safe margin.
- Bluetooth BTC and BTU stacks deliberately remain at 8 KiB until pairing, streaming, disconnect, and reconnect high-water marks have been captured.
- The controller is limited to one Classic ACL connection and one BLE commissioning connection.
- Application AVRCP, cover art, SPP, HFP, BLUFI, BLE Mesh, and unrelated profiles are disabled. A2DP's internal dependency code may still be built, but the application never initializes an AVRCP service.
- Unused Matter clusters and diagnostics are disabled. Binding and Occupancy Sensing remain only because PIOArduino force-links wrapper objects that reference them.
- BLE remains available for initial Matter commissioning and is irreversibly released before Classic Bluetooth starts.
- Safe heap and FreeRTOS routines are placed in flash to reclaim internal IRAM.
- The A2DP PCM callback is bounded and allocation-free; the visualization path uses fixed-point arithmetic.

Do not blindly shrink task stacks. Enable stack-overflow checking and capture task high-water marks under worst-case traffic before accepting any value.

If a stable custom build is not possible on this WROOM, document the measured shortfall and move to one of these fallback architectures:

1. A second classic ESP32 dedicated to A2DP and audio analysis, communicating compact energy/bass values to the Matter ESP32.
2. An external I2S microphone such as an INMP441, eliminating Bluetooth audio entirely.
3. A classic ESP32 module with PSRAM, while verifying which Matter allocations can actually be moved out of internal RAM.

## 10. Acceptance criteria

### 10.1 Build and boot

- Firmware builds without errors or linker overflow.
- A normal upload does not erase Matter or Wi-Fi configuration.
- Device completes 20 consecutive cold boots without panic, watchdog, or brownout.
- LEDs display the boot pattern until Wi-Fi connects, fade out, and remain off.
- Matter returns online after every boot.

### 10.2 Matter light

- Google Home on/off works for 20 consecutive cycles.
- Every on/off transition fades smoothly.
- At least red, green, blue, white, and two intermediate colors are verified.
- Every visible color change fades smoothly and follows the shortest hue path.
- Brightness changes are smooth and reach both low and high levels.
- Selected color survives reboot.
- Power state never restores as on after reboot.

### 10.3 Bluetooth discovery and pairing

- Enabling Music Sync leaves Matter responsive.
- `ESP32 Music Sync` appears in a phone Bluetooth scan within five seconds.
- A phone pairs without an external button or PIN entry.
- The LEDs pulse blue while waiting for a source.
- The pairing pulse stops after connection.
- Disabling Music Sync makes the device non-discoverable and disconnects an active source.

### 10.4 Audio reaction

- PCM callbacks arrive during playback.
- All nine LEDs visibly contain a complete, seamless spectrum.
- Brightness and motion respond to quiet and loud material.
- Bass-heavy material produces visible beat pulses.
- Silence decays smoothly.
- Streaming for at least 60 minutes produces no crash, heap exhaustion, stack overflow, or increasing memory loss.

### 10.5 Disconnect and stress recovery

- Disconnecting or forgetting the ESP32 from the phone causes advertising to resume while Music Sync stays on.
- A second phone can subsequently discover and pair with it.
- Perform at least 20 Music Sync on/off cycles.
- Perform at least 20 Bluetooth connect/disconnect cycles.
- Run normal Matter color commands during Bluetooth startup and during active streaming.
- Matter response remains reliable with no packet-buffer exhaustion.
- Free internal heap and largest block stabilize rather than decreasing each cycle.

## 11. Test procedure and commands

### 11.1 Safety before testing

- Do not display `include/secrets.h` in logs or responses.
- Do not run a full flash erase unless recommissioning is explicitly part of the test.
- An ordinary `./scripts/pio-memory-build.sh -t upload` preserves the Matter/NVS partitions with the current partition layout.
- After a failed Bluetooth test on the current build, perform a non-erasing processor reset before judging normal Matter behavior.

### 11.2 Current PlatformIO commands

ESP-IDF rejects the repository's current path because `untitled folder` contains a space. Use the wrapper, which builds in a secure temporary no-space path and copies final artifacts back to `.pio/memory-build/`:

```bash
./scripts/pio-memory-build.sh
./scripts/pio-memory-build.sh -t upload --upload-port /dev/cu.usbserial-210
pio device monitor --port /dev/cu.usbserial-210 --baud 115200
```

Rediscover the serial port when necessary:

```bash
ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* 2>/dev/null
```

The USB diagnostic trigger in the current firmware is a single `m` character. It invokes the same Matter Music Sync state path and is useful for separating Google Home delivery problems from Bluetooth initialization problems.

### 11.3 Evidence to capture

For every candidate build, retain:

- Framework and component versions
- Full `sdkconfig` or `sdkconfig.defaults`
- Static DRAM and IRAM link summary
- Free internal heap and largest block at boot, after Matter commissioning, before Bluetooth, after controller start, after Bluedroid start, after A2DP initialization, while streaming, and after disconnect
- Stack high-water marks for Matter/CHIP, BTC, BTU, A2DP, Arduino/main, and any application task
- Serial logs for all acceptance tests
- Phone model and operating-system version used for Bluetooth testing
- Google Home platform and app version
- Pass/fail result for each acceptance criterion

## 12. Matter commissioning and secrets

- Keep the existing Wi-Fi credentials in `include/secrets.h` unchanged unless the owner requests otherwise.
- Do not hard-code or publish credentials in this document.
- The current device was commissioned previously; normal builds and uploads should preserve that fabric.
- If a factory reset is intentionally performed, obtain the manual pairing code or QR URL from the serial output generated by `Matter.getManualPairingCode()` and `Matter.getOnboardingQRCodeUrl()`.
- Confirm that the normal light and Music Sync control appear exactly once after recommissioning; duplicate devices indicate stale Google Home fabric entries.

## 13. Definition of done

This project is done only when all acceptance criteria pass on the physical ESP32-WROOM, not merely when the firmware builds. In particular, seeing the Classic Bluetooth controller start is insufficient: Bluedroid, A2DP, discoverability, pairing, PCM delivery, the LED reaction, Matter concurrency, disconnect recovery, and long-duration stability must all be demonstrated with logs and real-device observations.
