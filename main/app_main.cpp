// TO UPLOAD TO ESP32 RUN BELOW COMMANDS IN IDF POWERSHELL:

// 1. idf.py build
// 2. idf.py -p COM4 flash monitor


// TO SAVE TO GITHUB RUN COMMANDS IN TERMINAL:
// git add .
// git commit -m "working version"
// git push

// TO REVERT TO OTHER VERSION:
// git log --oneline
// git checkout (COMMIT ID)



#include "driver/gpio.h"
#include "beacon_tracker.h"
#include "boot_button.h"
#include "cellular_link.h"
#include "cellular_diagnostics.h"
#include "cellular_protocol.h"
#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmwave_sensor.h"
#include "presence_sensors.h"
#include "setup_mode.h"
#include "temp_humidity_sensor.h"
#include "thermal_camera.h"
#include "tuned_face_detection.h"
#include "usb_telemetry.h"

#define LED_GPIO        GPIO_NUM_2
// The ESP32-S3 has a WS2812 RGB NeoPixel on GPIO 48.
// We hold it LOW so floating/spurious signals don't light it up.
#define NEOPIXEL_GPIO   GPIO_NUM_48

namespace {

const char *TAG = "app_main";

// The beacon decision must hold this long before sensors are switched, so a
// brief RSSI dip or a missed advertisement does not power-cycle everything.
constexpr TickType_t DRIVER_STATE_SETTLE = pdMS_TO_TICKS(3000);
constexpr TickType_t TELEMETRY_INTERVAL = pdMS_TO_TICKS(3000);

void send_boot_test_sms()
{
    char phone[32];
    if (!setup_mode_get_phone_number(phone, sizeof(phone))) {
        ESP_LOGW(TAG, "No valid phone number configured; save a number in setup mode before sending SMS");
        cellular_diagnostics_record("NO_PHONE");
        return;
    }
    char name[64];
    if (!setup_mode_get_name(name, sizeof(name))) {
        ESP_LOGW(TAG, "Save a valid name in setup before sending the BOOT test");
        cellular_diagnostics_record("NO_NAME");
        return;
    }
    char message[cellular::MAX_MESSAGE + 1];
    const int length = snprintf(message, sizeof(message),
        "Backseat Minder test for %s. This checks delivery of passenger alerts. No action is needed.",
        name);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(message) ||
        !cellular::valid_message(message)) {
        ESP_LOGW(TAG, "Saved name cannot be used in the BOOT test; update it in setup");
        cellular_diagnostics_record("INVALID_NAME");
        return;
    }
    cellular_link_send_sms(phone, message);
}

} // namespace

extern "C" void app_main()
{
    setup_mode_init();  // handles NVS, button GPIO

    // Suppress the onboard NeoPixel (GPIO 48) by driving it low.
    // Without this the camera driver leaves the data line floating,
    // which can latch a bright white colour into the LED.
    gpio_reset_pin(NEOPIXEL_GPIO);
    gpio_set_direction(NEOPIXEL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NEOPIXEL_GPIO, 0);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    // ---- Setup first ----
    // On first boot (nothing saved), when the setup button is held, or when
    // setup was requested at runtime, open the Wi-Fi setup portal now --
    // before Bluetooth, the camera or any sensor exists. enter_setup_mode()
    // saves the settings and restarts; it does not return.
    if (setup_mode_requested_at_boot()) {
        enter_setup_mode();
    }

    boot_button_init();
    const esp_err_t cellular_status = cellular_link_init();
    if (cellular_status != ESP_OK) {
        ESP_LOGW(TAG, "Cellular link unavailable: %s", esp_err_to_name(cellular_status));
        cellular_diagnostics_record("LINK_UNAVAILABLE");
    }
    usb_telemetry_init();

    // ---- Beacon-based driver presence ----
    // Scans for the driver's BLE beacon. Every presence sensor stays off
    // until the beacon reports the driver as away.
    if (!beacon_tracker_init()) {
        ESP_LOGE(TAG, "Bluetooth unavailable: driver will read as away and sensors will run");
    }

    float temperature_f = 0.0f;
    float humidity_percent = 0.0f;
    bool temperature_humidity_valid = false;
    bool face_detected_since_telemetry = false;
    bool mmwave_presence_detected = false;
    bool thermal_heat_detected = false;
    bool human_presence_detected = false;
    TickType_t last_telemetry = xTaskGetTickCount();
    TickType_t last_stack_report = xTaskGetTickCount();
    bool driver_near = true;            // Sensors start off.
    bool candidate_near = true;
    TickType_t candidate_since = xTaskGetTickCount();

    while (true)
    {
        if (boot_button_released()) {
            send_boot_test_sms();
        }

        if (setup_mode_button_pressed()) {
            setup_mode_restart_into_setup();  // does not return
        }

        // ---- Driver-presence gate ----
        const bool beacon_near = beacon_tracker_driver_near();
        if (beacon_near != candidate_near) {
            candidate_near = beacon_near;
            candidate_since = xTaskGetTickCount();
        }
        if (candidate_near != driver_near &&
            xTaskGetTickCount() - candidate_since >= DRIVER_STATE_SETTLE) {
            driver_near = candidate_near;
            ESP_LOGI(TAG, "Driver %s (beacon RSSI %d dBm)",
                     driver_near ? "near" : "away", beacon_tracker_rssi());
            if (driver_near) {
                presence_sensors_stop();
                temperature_humidity_valid = false;
            } else {
                presence_sensors_start();
                // Respect the DHT22's 2 s minimum spacing after start-up.
                last_telemetry = xTaskGetTickCount();
            }
            face_detected_since_telemetry = false;
        }

        if (presence_sensors_active()) {
            // ESP-WHO exposes face detection as a one-shot flag. Preserve any
            // detection until the next telemetry sample instead of losing it
            // between the main loop's polls and telemetry's 3 second polls.
            face_detected_since_telemetry |= get_tuned_face_detected();
            mmwave_presence_detected = mmwave_sensor_presence_detected();
            thermal_heat_detected = thermal_camera_heat_trace_detected();
            human_presence_detected = face_detected_since_telemetry ||
                                      mmwave_presence_detected ||
                                      thermal_heat_detected;
        } else {
            mmwave_presence_detected = false;
            thermal_heat_detected = false;
            human_presence_detected = false;
        }
        gpio_set_level(LED_GPIO, human_presence_detected ? 1 : 0);

        // Telemetry is sent while the driver is near too, so the viewer shows
        // the beacon state; sensor fields are false while sensors are off.
        // AM2302/DHT22 measurements should be spaced by at least two seconds.
        if (xTaskGetTickCount() - last_telemetry >= TELEMETRY_INTERVAL) {
            last_telemetry = xTaskGetTickCount();
            if (presence_sensors_active()) {
                temperature_humidity_valid =
                    temp_humidity_sensor_read(&temperature_f, &humidity_percent);
                if (temperature_humidity_valid) {
                    ESP_LOGI(TAG, "Temperature: %.1f F, Humidity: %.1f %%",
                             temperature_f, humidity_percent);
                }
            }

            usb_telemetry_send(temperature_f, humidity_percent,
                               temperature_humidity_valid,
                               face_detected_since_telemetry,
                               mmwave_presence_detected,
                               thermal_heat_detected,
                               human_presence_detected,
                               driver_near,
                               presence_sensors_active(),
                               beacon_tracker_rssi());

            face_detected_since_telemetry = false;
        }

        if (xTaskGetTickCount() - last_stack_report >= pdMS_TO_TICKS(60000)) {
            // ESP-IDF reports the minimum unused task stack in bytes.
            ESP_LOGI(TAG, "Main task minimum free stack: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            last_stack_report = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // Keep the BOOT button responsive.
    }
}
