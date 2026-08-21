#include <Arduino.h>
#include <FastLED.h>
#include <Matter.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdarg.h>

#include "esp32-hal-bt.h"
#include "esp32-hal-bt-mem.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "secrets.h"

// PIOArduino 55.03.38 always links with --wrap=log_printf, while an
// ESP-Insights-free framework does not provide the diagnostics wrapper.
// Forward directly to Arduino's allocation-conscious va_list implementation.
extern "C" int log_printfv(const char *format, va_list args);
extern "C" int __wrap_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int written = log_printfv(format, args);
  va_end(args);
  return written;
}

constexpr uint8_t LED_PIN = 13;
constexpr uint16_t NUM_LEDS = 9;
constexpr uint32_t FADE_DURATION_MS = 700;
constexpr uint32_t FRAME_INTERVAL_MS = 16;
constexpr uint32_t WIFI_TIMEOUT_MS = 30000;
constexpr uint32_t IPV6_TIMEOUT_MS = 10000;
constexpr uint32_t AUDIO_TIMEOUT_MS = 300;
constexpr uint32_t BEAT_COOLDOWN_MS = 140;
constexpr uint32_t BOOT_FADE_DURATION_MS = 900;
constexpr uint32_t BLUETOOTH_START_TIMEOUT_MS = 2500;
constexpr uint32_t A2DP_PROFILE_TIMEOUT_MS = 2500;
constexpr uint32_t BLUETOOTH_MODE_DEADLINE_MS = 8000;
constexpr uint8_t MUSIC_PASTEL_SATURATION = 150;
constexpr uint8_t MUSIC_COLOR_BLEND_AMOUNT = 56;
// Measured with at least 2.2 KiB unused at 4 KiB. A 3 KiB stack retains more
// than 1.2 KiB headroom and lowers transient pressure during BT startup.
constexpr uint32_t BLUETOOTH_START_TASK_STACK = 3072;
constexpr uint16_t MAX_ANALYZED_AUDIO_FRAMES = 4096;

constexpr char BLUETOOTH_NAME[] = "ESP32 Music Sync";

constexpr char PREFERENCES_NAMESPACE[] = "color-light";
constexpr char COLOR_KEY[] = "hsv";

// White at 50% brightness is used the first time the light is turned on.
constexpr espHsvColor_t DEFAULT_COLOR = {0, 0, 128};

// Long-lived objects are static so boot does not split the scarce internal
// heap into several permanent allocations before Matter and Bluetooth start.
CRGB leds[NUM_LEDS];
MatterColorLight matterLight;
MatterOnOffPlugin matterMusicSync;
Preferences preferences;

bool matterStarted = false;
bool commissioningHandled = false;
bool classicBluetoothAvailable = false;

portMUX_TYPE musicRequestMux = portMUX_INITIALIZER_UNLOCKED;
bool requestedMusicSync = false;
uint32_t musicRequestRevision = 0;
uint32_t handledMusicRevision = 0;
volatile bool musicSyncActive = false;

enum class BluetoothRuntimeState : uint8_t {
  Off,
  Starting,
  Ready,
  Failed,
};

BluetoothRuntimeState bluetoothState = BluetoothRuntimeState::Off;
bool bluetoothFailurePending = false;
bool bluetoothStartupDeadlineReported = false;
bool bluetoothStartTaskFinished = false;
uint32_t bluetoothModeStartedAt = 0;
TaskHandle_t bluetoothStartTaskHandle = nullptr;
esp_bd_addr_t bluetoothPeerAddress = {0};

portMUX_TYPE a2dpProfileMux = portMUX_INITIALIZER_UNLOCKED;
esp_a2d_init_state_t a2dpProfileState = ESP_A2D_DEINIT_SUCCESS;
uint32_t a2dpProfileRevision = 0;

portMUX_TYPE audioLevelMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t bassEnergyRaw = 0;
uint32_t lastAudioAt = 0;
uint32_t audioSampleRate = 44100;
// Q15 coefficients for two cascaded one-pole filters at the default 44.1 kHz
// A2DP rate: a 300 Hz low-pass.
uint16_t bassFilterAlphaQ15 = 1371;

struct MusicDynamics {
  // Bass levels are 0..255 fixed point and the rainbow phase is unsigned 8.8
  // fixed point. This keeps the 60 Hz path free of floating-point arithmetic.
  uint16_t rainbowPhase = 0;
  uint16_t bassCeiling = 80;
  uint32_t lastBeatAt = 0;
  uint8_t bass = 0;
  uint8_t bassAverage = 0;
  uint8_t beatPulse = 0;
  bool advertiseRequested = false;
  bool bluetoothConnected = false;
};

MusicDynamics music;

struct LightDynamics {
  portMUX_TYPE requestMux = portMUX_INITIALIZER_UNLOCKED;
  uint32_t requested = 0x000080;
  uint32_t requestRevision = 0;
  espHsvColor_t activeColor = DEFAULT_COLOR;
  espHsvColor_t fadeStartColor = DEFAULT_COLOR;
  espHsvColor_t fadeTargetColor = DEFAULT_COLOR;
  uint8_t currentBrightness = 0;
  uint8_t fadeStartBrightness = 0;
  uint8_t fadeTargetBrightness = 0;
  uint32_t fadeStartedAt = 0;
  uint32_t lastFrameAt = 0;
  uint32_t handledRevision = 0;
  bool fadeActive = false;
};

LightDynamics light;

uint8_t approach8(uint8_t current, uint8_t target, uint8_t rate) {
  const int16_t difference = int16_t(target) - current;
  return uint8_t(int16_t(current) + ((difference * rate) >> 8));
}

uint8_t normalizeAudioLevel(uint16_t value, uint16_t ceiling) {
  if (value >= ceiling) {
    return 255;
  }
  return uint8_t((uint32_t(value) * 255U) / ceiling);
}

bool credentialsConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

uint32_t packColor(espHsvColor_t color) {
  return (uint32_t(color.h) << 16) | (uint32_t(color.s) << 8) | color.v;
}

espHsvColor_t unpackColor(uint32_t packed) {
  return {
    uint8_t(packed >> 16),
    uint8_t(packed >> 8),
    uint8_t(packed),
  };
}

void renderLight(uint8_t brightness) {
  if (brightness == 0) {
    FastLED.clear();
  } else {
    fill_solid(
      leds,
      NUM_LEDS,
      CHSV(light.activeColor.h, light.activeColor.s, brightness)
    );
  }
  FastLED.show();
}

void renderBootPattern(uint32_t now) {
  // Two differently paced waves orbit the ring in opposite directions. Their
  // interference creates a bright, fluid portal-like pattern on nine LEDs.
  const uint8_t colorPhase = uint8_t(now / 7);
  const uint8_t clockwisePhase = uint8_t(now / 3);
  const uint8_t counterClockwisePhase = uint8_t(255 - (now / 5));

  for (uint16_t index = 0; index < NUM_LEDS; ++index) {
    const uint8_t circleOffset = (uint16_t(index) * 256U) / NUM_LEDS;
    const uint8_t clockwiseWave = sin8(clockwisePhase + circleOffset);
    const uint8_t counterClockwiseWave =
      sin8(counterClockwisePhase + circleOffset * 2);
    const uint16_t brightness =
      24 + scale8(clockwiseWave, 142) + scale8(counterClockwiseWave, 70);

    leds[index] = CHSV(
      colorPhase + circleOffset + scale8(counterClockwiseWave, 30),
      255,
      min<uint16_t>(brightness, 255)
    );
  }
  FastLED.show();
}

void fadeBootPatternOut() {
  CRGB fadeStart[NUM_LEDS];
  memcpy(fadeStart, leds, sizeof(fadeStart));
  const uint32_t startedAt = millis();

  while (millis() - startedAt < BOOT_FADE_DURATION_MS) {
    const uint8_t progress =
      ((millis() - startedAt) * 255UL) / BOOT_FADE_DURATION_MS;
    const uint8_t remaining = 255 - ease8InOutCubic(progress);

    for (uint16_t index = 0; index < NUM_LEDS; ++index) {
      leds[index] = fadeStart[index];
      leds[index].nscale8_video(remaining);
    }
    FastLED.show();
    delay(FRAME_INTERVAL_MS);
  }

  FastLED.clear(true);
}

void receiveBluetoothAudio(const uint8_t *data, uint32_t length) {
  // A2DP supplies interleaved stereo, signed 16-bit PCM samples. Keep this
  // callback short because it runs in the Bluetooth audio task.
  const int16_t *samples = reinterpret_cast<const int16_t *>(data);
  const uint32_t frameCount = length / (sizeof(int16_t) * 2);
  if (frameCount == 0) {
    return;
  }

  static int32_t bassLowPass1 = 0;
  static int32_t bassLowPass2 = 0;
  uint32_t bassSum = 0;
  uint16_t bassAlpha;
  portENTER_CRITICAL(&audioLevelMux);
  bassAlpha = bassFilterAlphaQ15;
  portEXIT_CRITICAL(&audioLevelMux);
  const uint32_t stride = max<uint32_t>(
    1,
    (frameCount + MAX_ANALYZED_AUDIO_FRAMES - 1) /
      MAX_ANALYZED_AUDIO_FRAMES
  );
  uint16_t analyzedFrames = 0;

  for (uint32_t frame = 0; frame < frameCount; frame += stride) {
    const int32_t left = samples[frame * 2];
    const int32_t right = samples[frame * 2 + 1];
    const int32_t mono = (left + right) / 2;

    // Cascading both stages produces an approximately 12 dB/octave rolloff.
    // Only bass below 300 Hz drives the visualizer.
    bassLowPass1 += int32_t(
      (int64_t(mono - bassLowPass1) * bassAlpha) >> 15
    );
    bassLowPass2 += int32_t(
      (int64_t(bassLowPass1 - bassLowPass2) * bassAlpha) >> 15
    );

    bassSum += bassLowPass2 < 0 ? uint32_t(-int64_t(bassLowPass2))
                               : uint32_t(bassLowPass2);
    ++analyzedFrames;
  }

  const uint16_t bass = bassSum / analyzedFrames;

  portENTER_CRITICAL(&audioLevelMux);
  bassEnergyRaw = bass;
  lastAudioAt = millis();
  portEXIT_CRITICAL(&audioLevelMux);
}

void resetMusicLevels() {
  portENTER_CRITICAL(&audioLevelMux);
  bassEnergyRaw = 0;
  lastAudioAt = 0;
  portEXIT_CRITICAL(&audioLevelMux);

  music.bass = 0;
  music.bassAverage = 0;
  music.beatPulse = 0;
  music.bassCeiling = 80;
}

void bluetoothA2dpEvent(
  esp_a2d_cb_event_t event,
  esp_a2d_cb_param_t *parameter
) {
  if (parameter == nullptr) {
    return;
  }

  if (event == ESP_A2D_PROF_STATE_EVT) {
    TaskHandle_t startupTask;
    portENTER_CRITICAL(&a2dpProfileMux);
    a2dpProfileState = parameter->a2d_prof_stat.init_state;
    ++a2dpProfileRevision;
    portEXIT_CRITICAL(&a2dpProfileMux);

    portENTER_CRITICAL(&musicRequestMux);
    startupTask = bluetoothStartTaskHandle;
    portEXIT_CRITICAL(&musicRequestMux);
    if (startupTask != nullptr) {
      xTaskNotifyGive(startupTask);
    }
    return;
  }

  if (event == ESP_A2D_AUDIO_CFG_EVT &&
      parameter->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
    const uint8_t sampleFrequency =
      parameter->audio_cfg.mcc.cie.sbc_info.samp_freq;
    uint32_t sampleRate = 44100;
    uint16_t bassAlpha = 1371;

    if (sampleFrequency & ESP_A2D_SBC_CIE_SF_16K) {
      sampleRate = 16000;
      bassAlpha = 3642;
    } else if (sampleFrequency & ESP_A2D_SBC_CIE_SF_32K) {
      sampleRate = 32000;
      bassAlpha = 1874;
    } else if (sampleFrequency & ESP_A2D_SBC_CIE_SF_48K) {
      sampleRate = 48000;
      bassAlpha = 1262;
    }

    portENTER_CRITICAL(&audioLevelMux);
    audioSampleRate = sampleRate;
    bassFilterAlphaQ15 = bassAlpha;
    portEXIT_CRITICAL(&audioLevelMux);
    return;
  }

  if (event != ESP_A2D_CONNECTION_STATE_EVT) {
    return;
  }

  const esp_a2d_connection_state_t state = parameter->conn_stat.state;
  portENTER_CRITICAL(&musicRequestMux);
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    memcpy(
      bluetoothPeerAddress,
      parameter->conn_stat.remote_bda,
      sizeof(bluetoothPeerAddress)
    );
    music.bluetoothConnected = true;
    music.advertiseRequested = false;
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    memset(bluetoothPeerAddress, 0, sizeof(bluetoothPeerAddress));
    music.bluetoothConnected = false;
    // Defer Bluetooth API calls to loop(). This callback runs in Bluedroid.
    music.advertiseRequested = true;
  }
  portEXIT_CRITICAL(&musicRequestMux);
}

void bluetoothGapEvent(
  esp_bt_gap_cb_event_t event,
  esp_bt_gap_cb_param_t *parameter
) {
  if (parameter == nullptr) {
    return;
  }

  if (event == ESP_BT_GAP_CFM_REQ_EVT) {
    // The device is headless, so Secure Simple Pairing uses Just Works.
    esp_bt_gap_ssp_confirm_reply(parameter->cfm_req.bda, true);
  } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_pin_reply(parameter->pin_req.bda, true, 4, pin);
  }
}

void logBluetoothMemory(const char *label) {
  const uint32_t capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Serial.printf(
    "%s: internal heap=%u, minimum-ever=%u, largest block=%u, live tasks=%u\n",
    label,
    heap_caps_get_free_size(capabilities),
    heap_caps_get_minimum_free_size(capabilities),
    heap_caps_get_largest_free_block(capabilities),
    unsigned(uxTaskGetNumberOfTasks())
  );
}

void logTaskStack(const char *taskName) {
  TaskHandle_t task = xTaskGetHandle(taskName);
  if (task == nullptr) {
    Serial.printf("Task '%s' is not running.\n", taskName);
    return;
  }
  Serial.printf(
    "Task '%s' minimum stack headroom=%u bytes\n",
    taskName,
    unsigned(uxTaskGetStackHighWaterMark(task))
  );
}

void logRuntimeDiagnostics() {
  logBluetoothMemory("Runtime memory");
  logTaskStack("loopTask");
  logTaskStack("CHIP");
  logTaskStack("BTC_TASK");
  logTaskStack("BTU_TASK");

  bool bluetoothConnected;
  portENTER_CRITICAL(&musicRequestMux);
  bluetoothConnected = music.bluetoothConnected;
  portEXIT_CRITICAL(&musicRequestMux);

  uint16_t rawBass;
  uint32_t audioAt;
  uint32_t sampleRate;
  portENTER_CRITICAL(&audioLevelMux);
  rawBass = bassEnergyRaw;
  audioAt = lastAudioAt;
  sampleRate = audioSampleRate;
  portEXIT_CRITICAL(&audioLevelMux);

  Serial.printf(
    "Music visualizer: active=%s, connected=%s, rate=%u Hz, bass=%u, audio age=%u ms\n",
    musicSyncActive ? "yes" : "no",
    bluetoothConnected ? "yes" : "no",
    unsigned(sampleRate),
    unsigned(rawBass),
    audioAt == 0 ? 0U : unsigned(millis() - audioAt)
  );
}

bool bluetoothCallSucceeded(const char *operation, esp_err_t result) {
  if (result == ESP_OK) {
    return true;
  }

  Serial.printf(
    "Bluetooth startup stopped at %s (error %d).\n",
    operation,
    int(result)
  );
  return false;
}

uint32_t prepareForA2dpProfileEvent() {
  // Remove a stale notification from the preceding profile operation before
  // recording its revision. The callback can then wake this startup task as
  // soon as the asynchronous init/deinit result arrives.
  ulTaskNotifyTake(pdTRUE, 0);
  portENTER_CRITICAL(&a2dpProfileMux);
  const uint32_t revision = a2dpProfileRevision;
  portEXIT_CRITICAL(&a2dpProfileMux);
  return revision;
}

bool waitForA2dpProfileState(
  esp_a2d_init_state_t expectedState,
  uint32_t initialRevision
) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < A2DP_PROFILE_TIMEOUT_MS) {
    esp_a2d_init_state_t state;
    uint32_t revision;
    portENTER_CRITICAL(&a2dpProfileMux);
    state = a2dpProfileState;
    revision = a2dpProfileRevision;
    portEXIT_CRITICAL(&a2dpProfileMux);

    if (revision != initialRevision && state == expectedState) {
      return true;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  }
  return false;
}

bool cleanupBluetoothStartup(bool a2dpInitializationRequested) {
  bool clean = true;

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    const esp_err_t visibilityResult = esp_bt_gap_set_scan_mode(
      ESP_BT_NON_CONNECTABLE,
      ESP_BT_NON_DISCOVERABLE
    );
    if (visibilityResult != ESP_OK) {
      Serial.printf(
        "Bluetooth cleanup could not hide the device (error %d).\n",
        int(visibilityResult)
      );
      clean = false;
    }

    if (a2dpInitializationRequested) {
      const uint32_t profileRevision = prepareForA2dpProfileEvent();
      const esp_err_t a2dpResult = esp_a2d_sink_deinit();
      if (a2dpResult != ESP_OK) {
        Serial.printf(
          "Bluetooth cleanup could not request A2DP deinitialization (error %d).\n",
          int(a2dpResult)
        );
        clean = false;
      } else if (!waitForA2dpProfileState(
                   ESP_A2D_DEINIT_SUCCESS,
                   profileRevision
                 )) {
        Serial.println("Bluetooth cleanup timed out waiting for A2DP deinitialization.");
        clean = false;
      }
    }

    if (esp_bluedroid_disable() != ESP_OK) {
      Serial.println("Bluetooth cleanup could not disable Bluedroid.");
      clean = false;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED &&
      esp_bluedroid_deinit() != ESP_OK) {
    Serial.println("Bluetooth cleanup could not deinitialize Bluedroid.");
    clean = false;
  }

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      esp_bt_controller_disable() != ESP_OK) {
    Serial.println("Bluetooth cleanup could not disable the controller.");
    clean = false;
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      esp_bt_controller_deinit() != ESP_OK) {
    Serial.println("Bluetooth cleanup could not deinitialize the controller.");
    clean = false;
  }

  logBluetoothMemory(clean ? "After Bluetooth rollback" : "After incomplete Bluetooth rollback");
  return clean;
}

bool startClassicBluetoothController() {
  esp_bt_controller_status_t status = esp_bt_controller_get_status();
  Serial.printf("Bluetooth controller initial status: %d\n", int(status));

  // Matter needs BLE only for commissioning, but it initially owns a BTDM
  // controller and Bluedroid host. Keeping that dual-mode instance alive while
  // A2DP starts leaves too little heap for Matter packet buffers. Once Matter
  // has declared Classic Bluetooth safe, stop the host and controller, release
  // the now-unused BLE memory permanently, and restart in Classic-only mode.
  esp_bluedroid_status_t hostStatus = esp_bluedroid_get_status();
  if (hostStatus == ESP_BLUEDROID_STATUS_ENABLED &&
      !bluetoothCallSucceeded("Bluedroid disable", esp_bluedroid_disable())) {
    return false;
  }
  hostStatus = esp_bluedroid_get_status();
  if (hostStatus == ESP_BLUEDROID_STATUS_INITIALIZED &&
      !bluetoothCallSucceeded("Bluedroid deinitialization", esp_bluedroid_deinit())) {
    return false;
  }

  if (status == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      !bluetoothCallSucceeded("controller disable", esp_bt_controller_disable())) {
    return false;
  }
  status = esp_bt_controller_get_status();
  if (status == ESP_BT_CONTROLLER_STATUS_INITED &&
      !bluetoothCallSucceeded("controller deinitialization", esp_bt_controller_deinit())) {
    return false;
  }
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
    Serial.println("Bluetooth controller did not stop before the Classic-only transition.");
    return false;
  }

  const esp_err_t releaseResult = esp_bt_mem_release(ESP_BT_MODE_BLE);
  if (releaseResult != ESP_OK && releaseResult != ESP_ERR_NOT_FOUND) {
    Serial.printf("Bluetooth startup stopped at BLE memory release (error %d).\n", int(releaseResult));
    return false;
  }
  logBluetoothMemory("After releasing BLE memory");

  esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  config.mode = ESP_BT_MODE_CLASSIC_BT;
  Serial.println("Initializing the Classic-only Bluetooth controller...");
  if (!bluetoothCallSucceeded(
        "controller initialization",
        esp_bt_controller_init(&config)
      )) {
    return false;
  }

  const uint32_t waitStartedAt = millis();
  while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
         millis() - waitStartedAt < BLUETOOTH_START_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  status = esp_bt_controller_get_status();
  if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
    Serial.println("Enabling the Classic-only Bluetooth controller...");
    if (!bluetoothCallSucceeded(
          "controller enable",
          esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)
        )) {
      return false;
    }
  }

  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}

bool initializeMinimalBluetoothSink() {
  logBluetoothMemory("Before Bluetooth startup");

  if (!startClassicBluetoothController()) {
    Serial.println("Classic Bluetooth controller did not become ready.");
    cleanupBluetoothStartup(false);
    return false;
  }
  logBluetoothMemory("After controller startup");

  esp_bluedroid_status_t hostStatus = esp_bluedroid_get_status();
  if (hostStatus == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_bluedroid_config_t config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (!bluetoothCallSucceeded(
        "Bluedroid initialization",
        esp_bluedroid_init_with_cfg(&config)
      )) {
      cleanupBluetoothStartup(false);
      return false;
    }
    hostStatus = esp_bluedroid_get_status();
  }

  if (hostStatus == ESP_BLUEDROID_STATUS_INITIALIZED) {
    if (!bluetoothCallSucceeded("Bluedroid enable", esp_bluedroid_enable())) {
      cleanupBluetoothStartup(false);
      return false;
    }
  }

  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    Serial.println("Bluedroid did not become ready.");
    cleanupBluetoothStartup(false);
    return false;
  }

  if (!bluetoothCallSucceeded(
        "GAP callback registration",
        esp_bt_gap_register_callback(bluetoothGapEvent)
      )) {
    cleanupBluetoothStartup(false);
    return false;
  }

  esp_bt_io_cap_t inputOutputCapability = ESP_BT_IO_CAP_NONE;
  if (!bluetoothCallSucceeded(
        "pairing security setup",
        esp_bt_gap_set_security_param(
          ESP_BT_SP_IOCAP_MODE,
          &inputOutputCapability,
          sizeof(inputOutputCapability)
        )
      )) {
    cleanupBluetoothStartup(false);
    return false;
  }

  esp_bt_pin_code_t unusedPin = {0};
  if (!bluetoothCallSucceeded(
        "legacy pairing setup",
        esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unusedPin)
      ) ||
      !bluetoothCallSucceeded(
        "Bluetooth device name",
        esp_bt_gap_set_device_name(BLUETOOTH_NAME)
      ) ||
      !bluetoothCallSucceeded(
        "A2DP callback registration",
        esp_a2d_register_callback(bluetoothA2dpEvent)
      ) ||
      !bluetoothCallSucceeded(
        "PCM callback registration",
        esp_a2d_sink_register_data_callback(receiveBluetoothAudio)
      )) {
    cleanupBluetoothStartup(false);
    return false;
  }

  const uint32_t profileRevision = prepareForA2dpProfileEvent();
  if (!bluetoothCallSucceeded("A2DP sink initialization", esp_a2d_sink_init())) {
    cleanupBluetoothStartup(false);
    return false;
  }
  if (!waitForA2dpProfileState(ESP_A2D_INIT_SUCCESS, profileRevision)) {
    Serial.println("Bluetooth startup stopped while waiting for A2DP initialization.");
    cleanupBluetoothStartup(true);
    return false;
  }

  logBluetoothMemory("After Bluetooth startup");
  return true;
}

void bluetoothStartTask(void *parameter) {
  (void)parameter;
  bool initialized = initializeMinimalBluetoothSink();
  const bool sinkInitialized = initialized;

  bool shouldAdvertise = false;
  portENTER_CRITICAL(&musicRequestMux);
  shouldAdvertise = requestedMusicSync;
  portEXIT_CRITICAL(&musicRequestMux);

  if (initialized) {
    initialized = bluetoothCallSucceeded(
      "Bluetooth visibility",
      esp_bt_gap_set_scan_mode(
        shouldAdvertise ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        shouldAdvertise ? ESP_BT_GENERAL_DISCOVERABLE
                        : ESP_BT_NON_DISCOVERABLE
      )
    );
  }
  if (!initialized && sinkInitialized) {
    cleanupBluetoothStartup(true);
  }

  portENTER_CRITICAL(&musicRequestMux);
  if (!initialized) {
    bluetoothState = BluetoothRuntimeState::Failed;
    bluetoothFailurePending = true;
    musicSyncActive = false;
    music.bluetoothConnected = false;
    music.advertiseRequested = false;
  } else {
    bluetoothState = BluetoothRuntimeState::Ready;
    musicSyncActive = shouldAdvertise;
  }
  bluetoothStartupDeadlineReported = false;
  portEXIT_CRITICAL(&musicRequestMux);

  if (initialized) {
    Serial.printf(
      "Minimal Bluetooth audio receiver ready and %s; AVRCP and I2S are disabled.\n",
      shouldAdvertise ? "discoverable" : "hidden"
    );
    logTaskStack("BTC_TASK");
    logTaskStack("BTU_TASK");
  }

  Serial.printf(
    "Bluetooth startup task minimum stack headroom=%u bytes\n",
    unsigned(uxTaskGetStackHighWaterMark(nullptr))
  );

  // The loop task reaps this task only after framework calls and rollback have
  // completed. Suspending here avoids both a dangling task handle and deleting
  // a task while it is blocked inside Bluetooth code.
  portENTER_CRITICAL(&musicRequestMux);
  bluetoothStartTaskFinished = true;
  portEXIT_CRITICAL(&musicRequestMux);
  vTaskSuspend(nullptr);
}

void reapBluetoothStartTask() {
  TaskHandle_t completedTask = nullptr;
  portENTER_CRITICAL(&musicRequestMux);
  if (bluetoothStartTaskFinished) {
    completedTask = bluetoothStartTaskHandle;
    bluetoothStartTaskHandle = nullptr;
    bluetoothStartTaskFinished = false;
  }
  portEXIT_CRITICAL(&musicRequestMux);

  if (completedTask != nullptr) {
    vTaskDelete(completedTask);
  }
}

void applyBluetoothAdvertisingRequest() {
  bool advertiseRequested;

  portENTER_CRITICAL(&musicRequestMux);
  advertiseRequested = music.advertiseRequested;
  music.advertiseRequested = false;
  portEXIT_CRITICAL(&musicRequestMux);

  BluetoothRuntimeState state;
  portENTER_CRITICAL(&musicRequestMux);
  state = bluetoothState;
  portEXIT_CRITICAL(&musicRequestMux);

  if (!advertiseRequested || !musicSyncActive ||
      state != BluetoothRuntimeState::Ready) {
    return;
  }

  resetMusicLevels();
  esp_bt_gap_set_scan_mode(
    ESP_BT_CONNECTABLE,
    ESP_BT_GENERAL_DISCOVERABLE
  );
  Serial.printf(
    "Bluetooth audio disconnected; '%s' is advertising again.\n",
    BLUETOOTH_NAME
  );
}

bool requestMusicSyncState(bool state) {
  portENTER_CRITICAL(&musicRequestMux);
  requestedMusicSync = state;
  ++musicRequestRevision;
  portEXIT_CRITICAL(&musicRequestMux);

  Serial.printf("Matter Music Sync: %s\n", state ? "ON" : "OFF");
  return true;
}

void applySerialDiagnosticCommand() {
  while (Serial.available() > 0) {
    const char command = char(Serial.read());
    if (command == 'h' || command == 'H') {
      logRuntimeDiagnostics();
      continue;
    }
    if (command != 'm' && command != 'M') {
      continue;
    }

    bool nextState;
    portENTER_CRITICAL(&musicRequestMux);
    nextState = !requestedMusicSync;
    portEXIT_CRITICAL(&musicRequestMux);

    Serial.printf(
      "USB diagnostic: Music Sync %s\n",
      nextState ? "ON" : "OFF"
    );
    if (!matterMusicSync.setOnOff(nextState)) {
      Serial.println("USB diagnostic could not update the Matter Music Sync switch.");
      continue;
    }

    // MatterOnOffPlugin::setOnOff() updates the local Matter attribute but
    // deliberately does not invoke onChange(). Mirror an external Matter
    // write through the application's request path so this diagnostic command
    // actually starts or stops Bluetooth.
    requestMusicSyncState(nextState);
  }
}

void applyMusicSyncRequest() {
  bool requestedState;
  uint32_t revision;

  portENTER_CRITICAL(&musicRequestMux);
  requestedState = requestedMusicSync;
  revision = musicRequestRevision;
  portEXIT_CRITICAL(&musicRequestMux);

  if (revision == handledMusicRevision) {
    return;
  }
  handledMusicRevision = revision;

  if (requestedState == musicSyncActive) {
    return;
  }

  if (requestedState) {
    bool classicAvailable;
    portENTER_CRITICAL(&musicRequestMux);
    classicAvailable = classicBluetoothAvailable;
    portEXIT_CRITICAL(&musicRequestMux);
    if (!classicAvailable) {
      Serial.println(
        "Music Sync is waiting for Matter commissioning to complete."
      );
      return;
    }
  }

  resetMusicLevels();
  if (requestedState) {
    BluetoothRuntimeState state;
    portENTER_CRITICAL(&musicRequestMux);
    music.bluetoothConnected = false;
    state = bluetoothState;
    portEXIT_CRITICAL(&musicRequestMux);

    Serial.printf(
      "Music Sync enabled. Pair your audio device with Bluetooth device '%s'.\n",
      BLUETOOTH_NAME
    );

    if (state == BluetoothRuntimeState::Ready) {
      if (!bluetoothCallSucceeded(
            "Bluetooth visibility",
            esp_bt_gap_set_scan_mode(
              ESP_BT_CONNECTABLE,
              ESP_BT_GENERAL_DISCOVERABLE
            )
          )) {
        bluetoothFailurePending = true;
        return;
      }
      musicSyncActive = true;
      return;
    }

    if (state == BluetoothRuntimeState::Starting) {
      musicSyncActive = true;
      return;
    }

    portENTER_CRITICAL(&musicRequestMux);
    bluetoothState = BluetoothRuntimeState::Starting;
    bluetoothModeStartedAt = millis();
    bluetoothStartupDeadlineReported = false;
    musicSyncActive = true;
    bluetoothFailurePending = false;
    portEXIT_CRITICAL(&musicRequestMux);

    TaskHandle_t startupTask = nullptr;
    if (xTaskCreate(
          bluetoothStartTask,
          "bt-start",
          BLUETOOTH_START_TASK_STACK,
          nullptr,
          1,
          &startupTask
        ) != pdPASS) {
      Serial.println(
        "Music Sync could not allocate its temporary startup task."
      );
      portENTER_CRITICAL(&musicRequestMux);
      bluetoothState = BluetoothRuntimeState::Failed;
      bluetoothFailurePending = true;
      musicSyncActive = false;
      portEXIT_CRITICAL(&musicRequestMux);
    } else {
      portENTER_CRITICAL(&musicRequestMux);
      bluetoothStartTaskHandle = startupTask;
      portEXIT_CRITICAL(&musicRequestMux);
    }
  } else {
    Serial.println("Music Sync disabled; hiding Bluetooth audio.");
    musicSyncActive = false;
    BluetoothRuntimeState state;
    bool wasConnected;
    esp_bd_addr_t peerAddress;
    portENTER_CRITICAL(&musicRequestMux);
    state = bluetoothState;
    wasConnected = music.bluetoothConnected;
    memcpy(peerAddress, bluetoothPeerAddress, sizeof(peerAddress));
    music.bluetoothConnected = false;
    music.advertiseRequested = false;
    portEXIT_CRITICAL(&musicRequestMux);

    if (state == BluetoothRuntimeState::Ready) {
      esp_bt_gap_set_scan_mode(
        ESP_BT_NON_CONNECTABLE,
        ESP_BT_NON_DISCOVERABLE
      );
      if (wasConnected) {
        esp_a2d_sink_disconnect(peerAddress);
      }
    }
    renderLight(light.currentBrightness);
  }
}

void applyBluetoothStartupDeadline() {
  bool timedOut = false;

  portENTER_CRITICAL(&musicRequestMux);
  if (bluetoothState == BluetoothRuntimeState::Starting &&
      !bluetoothStartupDeadlineReported &&
      millis() - bluetoothModeStartedAt >= BLUETOOTH_MODE_DEADLINE_MS) {
    // Keep the state single-flight until the existing task exits. Marking it
    // Failed here would allow a retry to allocate another 4 KiB task while the
    // first remained blocked inside the Bluetooth framework.
    bluetoothStartupDeadlineReported = true;
    bluetoothFailurePending = true;
    musicSyncActive = false;
    music.bluetoothConnected = false;
    music.advertiseRequested = false;
    timedOut = true;
  }
  portEXIT_CRITICAL(&musicRequestMux);

  if (timedOut) {
    Serial.println(
      "Bluetooth startup exceeded 8 seconds; Matter was kept responsive."
    );
  }
}

void applyBluetoothFailure() {
  bool failurePending;
  portENTER_CRITICAL(&musicRequestMux);
  failurePending = bluetoothFailurePending;
  bluetoothFailurePending = false;
  portEXIT_CRITICAL(&musicRequestMux);

  if (!failurePending) {
    return;
  }

  Serial.println(
    "Music Sync could not start, but Matter remains online. Turning its switch off."
  );
  if (!matterMusicSync.setOnOff(false)) {
    Serial.println("Matter Music Sync switch could not be updated after Bluetooth failure.");
  }
  // The Arduino wrapper does not invoke onChange() for a local setOnOff().
  // Keep the application request synchronized with the reported attribute.
  requestMusicSyncState(false);
  renderLight(light.currentBrightness);
}

void updateMusicVisualization() {
  if (!musicSyncActive) {
    return;
  }

  const uint32_t now = millis();
  if (now - light.lastFrameAt < FRAME_INTERVAL_MS) {
    return;
  }
  light.lastFrameAt = now;

  bool bluetoothConnected;
  portENTER_CRITICAL(&musicRequestMux);
  bluetoothConnected = music.bluetoothConnected;
  portEXIT_CRITICAL(&musicRequestMux);

  if (!bluetoothConnected) {
    // A calm blue breathing pulse indicates that the sink is ready to pair.
    const uint8_t pulsePhase = uint8_t((now * 256UL) / 1800UL);
    const uint8_t pulseBrightness = 18 + scale8(sin8(pulsePhase), 112);
    fill_solid(leds, NUM_LEDS, CHSV(160, 255, pulseBrightness));
    FastLED.show();
    return;
  }

  uint16_t rawBass;
  uint32_t audioAt;
  portENTER_CRITICAL(&audioLevelMux);
  rawBass = bassEnergyRaw;
  audioAt = lastAudioAt;
  portEXIT_CRITICAL(&audioLevelMux);

  if (audioAt == 0 || now - audioAt > AUDIO_TIMEOUT_MS) {
    rawBass = 0;
  }

  // Fast, low-floor AGC keeps the bass visible on quieter tracks.
  // A tiny gate avoids amplifying codec noise during digital silence.
  const uint16_t decayedBassCeiling = max<uint16_t>(
    24,
    uint16_t((uint32_t(music.bassCeiling) * 252U) >> 8)
  );
  music.bassCeiling = max(rawBass, decayedBassCeiling);

  const uint8_t normalizedBass = normalizeAudioLevel(
    rawBass <= 2 ? 0 : rawBass,
    music.bassCeiling
  );
  music.bass = approach8(
    music.bass,
    normalizedBass,
    normalizedBass > music.bass ? 176 : 45
  );

  music.bassAverage = approach8(music.bassAverage, music.bass, 9);
  if (music.bass > 48 &&
      uint16_t(music.bass) * 100U > uint16_t(music.bassAverage) * 120U &&
      now - music.lastBeatAt >= BEAT_COOLDOWN_MS) {
    music.beatPulse = 255;
    music.lastBeatAt = now;
  }
  music.beatPulse = scale8(music.beatPulse, 220);

  // Move a narrow pastel rainbow band around the ring. Showing the whole color
  // wheel at once blends toward white through a diffuser, while this adjacent-
  // color wave remains distinct. Bass drives every LED's speed and brightness.
  music.rainbowPhase +=
    141U +
    (uint16_t(music.bass) * 1100U) / 255U;
  const uint8_t huePhase = uint8_t(music.rainbowPhase >> 8);
  const uint16_t lightLevel =
    64U +
    (uint16_t(music.bass) * 120U) / 255U +
    (uint16_t(music.beatPulse) * 45U) / 255U;
  const uint8_t brightness = min<uint16_t>(lightLevel, 255);

  for (uint16_t index = 0; index < NUM_LEDS; ++index) {
    const uint8_t circlePhase = (uint16_t(index) * 256U) / NUM_LEDS;
    const uint8_t waveHueOffset = scale8(sin8(circlePhase), 64);
    const CRGB targetColor = CHSV(
      huePhase + waveHueOffset,
      MUSIC_PASTEL_SATURATION,
      brightness
    );
    leds[index] = blend(
      leds[index],
      targetColor,
      MUSIC_COLOR_BLEND_AMOUNT
    );
  }
  FastLED.show();
}

void updateFade() {
  const uint32_t now = millis();
  uint32_t revision;
  uint32_t request;

  portENTER_CRITICAL(&light.requestMux);
  revision = light.requestRevision;
  request = light.requested;
  portEXIT_CRITICAL(&light.requestMux);

  if (revision != light.handledRevision) {
    light.handledRevision = revision;
    const espHsvColor_t requestedColor = unpackColor(request);
    const uint8_t requestedBrightness =
      (request & (1UL << 24)) ? requestedColor.v : 0;

    // A color change while already off is invisible, so adopt it immediately.
    // The next fade-on will then start with the newly selected color.
    if (light.currentBrightness == 0 && requestedBrightness == 0) {
      light.activeColor = requestedColor;
      light.fadeActive = false;
      return;
    }

    light.fadeStartColor = light.activeColor;
    light.fadeTargetColor = requestedColor;
    light.fadeStartBrightness = light.currentBrightness;
    light.fadeTargetBrightness = requestedBrightness;
    light.fadeStartedAt = now;
    light.fadeActive = true;
  }

  if (!light.fadeActive ||
      now - light.lastFrameAt < FRAME_INTERVAL_MS) {
    return;
  }
  light.lastFrameAt = now;

  const uint32_t elapsed = now - light.fadeStartedAt;
  if (elapsed >= FADE_DURATION_MS) {
    light.activeColor = light.fadeTargetColor;
    light.currentBrightness = light.fadeTargetBrightness;
    light.fadeActive = false;
  } else {
    const uint8_t progress = (elapsed * 255UL) / FADE_DURATION_MS;
    const uint8_t easedProgress = ease8InOutCubic(progress);

    // Travel through the shortest side of FastLED's circular hue wheel.
    int16_t hueChange =
      int16_t(light.fadeTargetColor.h) - light.fadeStartColor.h;
    if (hueChange > 127) {
      hueChange -= 256;
    } else if (hueChange < -128) {
      hueChange += 256;
    }
    light.activeColor.h = uint8_t(
      int16_t(light.fadeStartColor.h) +
      (int32_t(hueChange) * easedProgress) / 255
    );

    const int16_t saturationChange =
      int16_t(light.fadeTargetColor.s) - light.fadeStartColor.s;
    light.activeColor.s = light.fadeStartColor.s +
      (int32_t(saturationChange) * easedProgress) / 255;

    const int16_t brightnessChange =
      int16_t(light.fadeTargetBrightness) - light.fadeStartBrightness;
    light.currentBrightness = light.fadeStartBrightness +
      (int32_t(brightnessChange) * easedProgress) / 255;
  }

  if (!musicSyncActive) {
    renderLight(light.currentBrightness);
  }
}

bool requestLightState(bool state, espHsvColor_t color) {
  const uint32_t packedRequest = packColor(color) |
    (state ? (1UL << 24) : 0);

  portENTER_CRITICAL(&light.requestMux);
  light.requested = packedRequest;
  ++light.requestRevision;
  portEXIT_CRITICAL(&light.requestMux);

  // Preserve the selected color, but intentionally do not preserve power state.
  preferences.putUInt(COLOR_KEY, packColor(color));
  Serial.printf(
    "Matter light: %s, HSV=(%u,%u,%u)\n",
    state ? "ON" : "OFF",
    color.h,
    color.s,
    color.v
  );
  return true;
}

bool prepareWiFiForMatterStart() {
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.setAutoReconnect(false);

  // Arduino must create the shared station netif before Matter starts, or
  // both frameworks can attempt to create it. Do not let Matter see the
  // driver credentials yet, though: its connectivity task can otherwise
  // associate while the root endpoint is still being enabled and try to
  // update Network Commissioning cluster data that is not registered yet.
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("Matter setup stopped: failed to prepare the Wi-Fi interface.");
    return false;
  }

  // Clear only the driver's RAM copy. The NVS copy remains intact, avoiding
  // flash writes on every boot; WiFi.begin() restores the configured product
  // credentials after Matter.begin() has synchronously enabled all endpoints.
  esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) {
    Serial.printf(
      "Matter setup stopped: failed to select volatile Wi-Fi storage (%s).\n",
      esp_err_to_name(err)
    );
    return false;
  }

  wifi_config_t emptyConfig = {};
  err = esp_wifi_set_config(WIFI_IF_STA, &emptyConfig);
  if (err != ESP_OK) {
    Serial.printf(
      "Matter setup stopped: failed to defer the Wi-Fi connection (%s).\n",
      esp_err_to_name(err)
    );
    return false;
  }

  renderBootPattern(millis());
  return true;
}

void startWiFiConnection() {
  WiFi.setAutoReconnect(true);
  WiFi.enableIPv6(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool setMatterWiFiStationMode(
  chip::DeviceLayer::ConnectivityManager::WiFiStationMode mode
) {
  const esp_matter::lock::status_t lockStatus =
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
  if (lockStatus == esp_matter::lock::FAILED) {
    Serial.println("Matter setup stopped: failed to lock the Matter stack.");
    return false;
  }

  const CHIP_ERROR err =
    chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(mode);
  if (lockStatus == esp_matter::lock::SUCCESS) {
    esp_matter::lock::chip_stack_unlock();
  }

  if (err != CHIP_NO_ERROR) {
    Serial.printf(
      "Matter setup stopped: failed to change Wi-Fi ownership (%lu).\n",
      static_cast<unsigned long>(err.AsInteger())
    );
    return false;
  }
  return true;
}

bool waitForWiFiConnection() {
  uint32_t statusStartedAt = millis();
  uint32_t lastBootFrameAt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    const uint32_t now = millis();
    if (now - lastBootFrameAt >= FRAME_INTERVAL_MS) {
      lastBootFrameAt = now;
      renderBootPattern(now);
    }

    if (now - statusStartedAt >= WIFI_TIMEOUT_MS) {
      Serial.println("Still waiting for Wi-Fi; retrying connection.");
      WiFi.reconnect();
      statusStartedAt = now;
    }
    delay(5);
  }

  Serial.print("Wi-Fi connected. IP address: ");
  Serial.println(WiFi.localIP());
  fadeBootPatternOut();

  // Matter communicates over IPv6. Wait for it before restoring a Matter
  // session so the device reconnects correctly after a power cycle.
  const uint32_t ipv6StartedAt = millis();
  while (WiFi.linkLocalIPv6() == IN6ADDR_ANY &&
         millis() - ipv6StartedAt < IPV6_TIMEOUT_MS) {
    delay(10);
  }

  if (WiFi.linkLocalIPv6() == IN6ADDR_ANY) {
    Serial.println("IPv6 setup timed out; Matter network services are unavailable.");
    return false;
  }

  Serial.print("IPv6 link-local address: ");
  Serial.println(WiFi.linkLocalIPv6());
  return true;
}

void allowClassicBluetooth() {
  portENTER_CRITICAL(&musicRequestMux);
  if (!classicBluetoothAvailable && requestedMusicSync) {
    // Retry a request that arrived while commissioning still owned BLE.
    ++musicRequestRevision;
  }
  classicBluetoothAvailable = true;
  portEXIT_CRITICAL(&musicRequestMux);
}

void handleMatterEvent(
  matterEvent_t event,
  const chip::DeviceLayer::ChipDeviceEvent *eventData
) {
  (void)eventData;
  if (event != MATTER_BLE_DEINITIALIZED) {
    return;
  }

  allowClassicBluetooth();
  Serial.println(
    "Matter stopped BLE commissioning; Classic Bluetooth may now start."
  );
  logBluetoothMemory("After Matter BLE commissioning");
}

void setup() {
  // HardwareSerial defaults to a 256-byte RX ring. A one-character diagnostic
  // protocol only needs the ESP32 UART driver's minimum FIFO-plus-one buffer.
  Serial.setRxBufferSize(129);
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255);
  FastLED.clear(true);

  preferences.begin(PREFERENCES_NAMESPACE, false);
  const espHsvColor_t savedColor = unpackColor(
    preferences.getUInt(COLOR_KEY, packColor(DEFAULT_COLOR))
  );

  if (!credentialsConfigured()) {
    Serial.println(
      "Matter setup paused: set WIFI_SSID and WIFI_PASSWORD in include/secrets.h."
    );
    return;
  }

  if (!prepareWiFiForMatterStart()) {
    return;
  }

  // Always initialize off, even if power was lost while the light was on.
  if (!matterLight.begin(false, savedColor)) {
    Serial.println("Matter setup stopped: failed to create the color-light endpoint.");
    return;
  }
  matterLight.onChange(requestLightState);
  if (!matterMusicSync.begin(false)) {
    Serial.println("Matter setup stopped: failed to create the Music Sync endpoint.");
    return;
  }
  matterMusicSync.onChange(requestMusicSyncState);

  // ESP-Matter's Bluedroid backend requests BLE when it starts. This build
  // also needs Classic Bluetooth later for A2DP, so initialize and enable the
  // controller as BTDM first. Matter then reuses the already-enabled
  // controller instead of initializing it as BTDM and enabling it as BLE,
  // which ESP-IDF rejects because those modes do not match.
  if (!btStartMode(BT_MODE_BTDM)) {
    Serial.println("Matter setup stopped: dual-mode Bluetooth failed to start.");
    return;
  }

  Matter.onEvent(handleMatterEvent);
  Matter.begin();
  if (!esp_matter::is_started()) {
    Serial.println("Matter setup stopped: the Matter server failed to start.");
    return;
  }
  matterStarted = true;

  // esp_matter::start(), used by Matter.begin(), does not return until the
  // server has enabled every endpoint. Temporarily make station association
  // application-controlled so Matter and Arduino cannot both call connect.
  if (!setMatterWiFiStationMode(
        chip::DeviceLayer::ConnectivityManager::
          kWiFiStationMode_ApplicationControlled)) {
    return;
  }
  startWiFiConnection();
  if (!waitForWiFiConnection()) {
    return;
  }
  if (!setMatterWiFiStationMode(
        chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Enabled)) {
    return;
  }

  if (Matter.isDeviceCommissioned()) {
    commissioningHandled = true;
    allowClassicBluetooth();
    matterLight.updateAccessory();
    matterMusicSync.updateAccessory();
    Serial.println("Matter color light and Music Sync switch are commissioned and ready.");
    Serial.println(
      "Bluetooth audio is hidden until Music Sync is enabled in Matter or with the USB 'm' command."
    );
  } else {
    Serial.println("Add a new Matter device in Google Home using:");
    Serial.printf(
      "Manual pairing code: %s\n",
      Matter.getManualPairingCode().c_str()
    );
    Serial.printf(
      "QR code URL: %s\n",
      Matter.getOnboardingQRCodeUrl().c_str()
    );
  }
}

void loop() {
  reapBluetoothStartTask();
  applySerialDiagnosticCommand();
  applyMusicSyncRequest();
  applyBluetoothStartupDeadline();
  applyBluetoothFailure();
  applyBluetoothAdvertisingRequest();
  if (musicSyncActive) {
    updateMusicVisualization();
  } else {
    updateFade();
  }

  if (matterStarted && !commissioningHandled &&
      Matter.isDeviceCommissioned()) {
    commissioningHandled = true;
    allowClassicBluetooth();
    matterLight.updateAccessory();
    matterMusicSync.updateAccessory();
    Serial.println("Matter commissioning complete; light and Music Sync are ready.");
  }

  delay(10);
}
