# Memory Leak Evaluation

Date: 2026-08-18

## Scope

This review covered the application source, Bluetooth and FreeRTOS lifecycle paths, memory-related build configuration, and live boot/runtime samples from the connected ESP32. It was updated after the lifecycle corrections and hardware validation described below.

## Verdict

The normal steady-state application path does not contain an obvious direct heap leak. Long-lived application state is statically allocated, the audio callback is allocation-free, and no application-level `new`, `malloc`, growing container, or per-frame allocation was found.

The two identified Bluetooth lifecycle defects have been corrected: startup is single-flight with an owned task handle, and partial initialization unwinds owned A2DP, Bluedroid, and controller stages in reverse order. A clean hardware run showed stable task count and memory after Bluetooth startup, with no error during the observation window.

This is evidence that the normal path is leak-safe; it is not a proof that every failure path is leak-free. The 50-cycle tests, forced failure injection, and allocation-attributed heap tracing in the final section remain required before making that stronger claim.

## Findings

### Resolved — High: Bluetooth startup tasks could accumulate after timeouts

The reviewed implementation created a temporary FreeRTOS task in `applyMusicSyncRequest()` and discarded its handle.

The old `applyBluetoothStartupDeadline()` also changed the shared state from `Starting` to `Failed` after eight seconds without establishing that the task had exited. A later enable request could therefore create another task while the previous one was still executing inside a Bluetooth API.

If startup remains blocked across retries, each attempt consumes another task stack and task-control block. With the measured heap headroom, only a few overlapping tasks could cause allocation failure.

Implemented correction:

- `bluetoothStartTaskHandle` retains ownership and prevents a second startup while one is running.
- A deadline is now diagnostic only; it cannot make the operation retryable while its task is still inside framework code.
- The task publishes its result, marks itself finished, and suspends itself. `reapBluetoothStartTask()` then deletes it from the main loop, outside Bluetooth framework calls.
- Live task count returned from 17 during startup to 16 afterward, confirming that the temporary task was reclaimed on the tested path.

### Resolved — Medium: Partial Bluetooth initialization was not rolled back

The reviewed `startClassicBluetoothController()` and `initializeMinimalBluetoothSink()` returned immediately when an initialization stage failed. No staged cleanup was performed after the controller, Bluedroid host, callbacks, or A2DP sink had been initialized.

Because Bluedroid uses dynamically allocated state, a failure after an earlier successful stage could retain tasks, state machines, callbacks, and heap allocations while the application reported `BluetoothRuntimeState::Failed`.

The exact ESP-IDF dependency provides the corresponding cleanup APIs, including `esp_a2d_sink_deinit()`, `esp_bluedroid_disable()`, `esp_bluedroid_deinit()`, `esp_bt_controller_disable()`, and `esp_bt_controller_deinit()`.

Implemented correction:

- `cleanupBluetoothStartup()` unwinds only the Classic startup stages owned by this application, in reverse dependency order.
- The A2DP profile-state callback and revision counter let startup wait for initialization success and cleanup wait for asynchronous deinitialization completion.
- Bluedroid and the controller are disabled/deinitialized only after the A2DP stage has finished unwinding.
- Forced failure injection at every stage is still required to dynamically validate these branches.

### Resolved — Runtime headroom: oversized Bluetooth task stacks

The first post-fix hardware image retained 8 KiB each for `BTC_TASK` and `BTU_TASK`. After A2DP startup its minimum-ever heap fell to 988 bytes, followed by Matter `PacketBuffer: pool EMPTY` errors. This was memory exhaustion rather than evidence of an unbounded leak.

Measured high-water marks showed at least 6.6 KiB unused in each 8 KiB Bluetooth stack. [`sdkconfig.memory`](sdkconfig.memory) now assigns 4 KiB to each, returning 8 KiB of internal DRAM while preserving more than 2.4 KiB observed headroom under the tested workload. The one-shot Bluetooth startup task was reduced from 4 KiB to 3 KiB and retained 1,256 bytes of observed headroom. The corrected image completed the same window without packet-buffer errors.

### Informational: Music Sync OFF retains A2DP memory by design

The disable path hides the device and disconnects an active peer, but it does not deinitialize the A2DP sink ([`src/main.cpp`](src/main.cpp)).

This is bounded, reusable retention rather than an unbounded leak: the allocated A2DP state remains reachable and is reused on the next enable. It does mean that heap consumed by the first A2DP initialization will not return when Music Sync is switched off. If the product requirement is to reclaim that memory while off, an explicit asynchronous A2DP teardown/restart lifecycle is needed.

### Improved, still open: test coverage cannot yet prove leak freedom

The runtime diagnostic now reports current free internal heap, minimum-ever free heap, largest free block, and live task count. This spots drift, fragmentation, transient low-water events, and orphaned tasks, but it does not identify individual outstanding allocations.

Heap tracing and heap task tracking are disabled in [`sdkconfig.defaults`](sdkconfig.defaults#L1952). Therefore, snapshots alone cannot attribute lost memory or prove that all resources are released.

## Positive observations

- The PCM analysis callback uses only fixed-size local and static state and performs no heap allocation ([`src/main.cpp`](src/main.cpp)).
- LED storage and long-lived application objects have static lifetime.
- The main render and fade loops do not allocate dynamically.
- The temporary boot fade buffer is a small bounded stack array.
- The temporary Bluetooth startup task has explicit ownership and is reaped after publishing completion.
- Preferences, Matter callbacks, Wi-Fi, FastLED, and endpoint objects are initialized once rather than repeatedly in the main loop.

## Live evidence

The corrected image was built, uploaded, hash-verified, and cold-booted on the connected ESP32. After Matter restored its subscriptions and enabled Music Sync, A2DP initialized and remained discoverable during a 60-second concurrent Matter/Bluetooth observation window:

| Metric | Observed value |
| --- | ---: |
| Free internal heap after 60 seconds | 17,012 bytes |
| Minimum-ever heap after the 60-second normal run | 7,692 bytes |
| Largest free internal block | 16,372 bytes |
| Live tasks after startup cleanup | 16 |
| `loopTask` minimum stack headroom | 1,772 bytes |
| `CHIP` minimum stack headroom | 1,376 bytes |
| `BTC_TASK` minimum stack headroom | 2,564 bytes |
| `BTU_TASK` minimum stack headroom | 2,484 bytes |
| Bluetooth startup task minimum headroom | 1,256 bytes |

The cold boot completed without an error, panic, assertion, watchdog, or reboot. No Matter packet-buffer, DNS-SD, subscription, or Bluetooth error occurred during the post-startup observation window. Task count dropped from 17 to 16 when the startup task was reaped, and the steady-state sample retained substantially more usable headroom than the rejected 8 KiB-stack candidate.

A follow-up burst of five Music Sync OFF/ON cycles also completed without an error or task-count growth and returned to 16,848 bytes free with the same 16,372-byte largest block and 16 live tasks. The minimum-ever heap reached 1,408 bytes during that deliberately rapid burst, reinforcing that transient headroom in the hybrid build is still tight and that the longer stress plan below must not be skipped. The final ON state was independently reported as `ESP32 Music Sync — Connected` in macOS Bluetooth settings.

Bluetooth audio streaming, forced initialization failures, deadline expiration/retry, and the recommended 50-cycle tests were not exercised in this short validation. Those tests remain the boundary between normal-path evidence and a repository-wide leak-free claim.

## Recommended verification

To finish the leak-safety proof, use a development build with standalone heap tracing and task tracking enabled. Capture free heap, minimum-ever free heap, largest free block, live task count, and outstanding allocations at these checkpoints:

1. Stable commissioned idle state.
2. First Music Sync enable and completed A2DP initialization.
3. Music Sync disable.
4. At least 50 enable/disable cycles after the one-time warm-up allocation.
5. At least 50 connect/disconnect/re-advertise cycles while streaming audio.
6. Forced failure at every Bluetooth initialization stage.
7. Repeated startup-deadline expirations and retries.
8. A final return to the same stable idle state.

After one-time framework initialization has completed, free heap, largest block, and live task count should plateau rather than decline with cycle count. Heap traces should contain no allocations attributable solely to completed or failed startup attempts.
