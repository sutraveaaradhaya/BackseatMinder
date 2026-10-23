#include "presence_sensors.h"

#include <atomic>

#include "camera_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mmwave_sensor.h"
#include "temp_humidity_sensor.h"
#include "thermal_camera.h"
#include "usb_telemetry.h"

namespace {

const char *TAG = "presence_sensors";
constexpr TickType_t THERMAL_FRAME_INTERVAL = pdMS_TO_TICKS(250); // 4 FPS

TaskHandle_t s_thermal_task = nullptr;
SemaphoreHandle_t s_thermal_stopped = nullptr;
std::atomic<bool> s_thermal_should_run{false};
bool s_active = false;

// Owns the MLX90640 for a sensor session: initializes it, streams frames to
// USB and runs heat-trace detection, then releases the I2C bus on stop.
void thermal_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (thermal_camera_init()) {
            TickType_t next_frame_time = xTaskGetTickCount();
            while (s_thermal_should_run.load()) {
                size_t csv_length = 0;
                const char *csv = thermal_camera_read_csv(&csv_length);
                if (csv) {
                    usb_telemetry_send_thermal_frame(csv, csv_length);
                    uint8_t x = 0;
                    uint8_t y = 0;
                    uint8_t width = 0;
                    uint8_t height = 0;
                    const bool detected = thermal_camera_get_heat_trace_bounds(
                        &x, &y, &width, &height);
                    usb_telemetry_send_heat_trace_bounds(detected, x, y, width, height);
                }
                xTaskDelayUntil(&next_frame_time, THERMAL_FRAME_INTERVAL);
            }
        }
        thermal_camera_deinit();
        xSemaphoreGive(s_thermal_stopped);
    }
}

void thermal_start()
{
    if (!s_thermal_task) {
        s_thermal_stopped = xSemaphoreCreateBinary();
        if (!s_thermal_stopped ||
            xTaskCreatePinnedToCore(thermal_task, "thermal", 8192, nullptr, 3,
                                    &s_thermal_task, 0) != pdPASS) {
            ESP_LOGW(TAG, "Could not start thermal camera task");
            s_thermal_task = nullptr;
            return;
        }
    }
    s_thermal_should_run.store(true);
    xTaskNotifyGive(s_thermal_task);
}

void thermal_stop()
{
    if (!s_thermal_task) {
        return;
    }
    s_thermal_should_run.store(false);
    xSemaphoreTake(s_thermal_stopped, portMAX_DELAY);
}

} // namespace

void presence_sensors_start()
{
    if (s_active) {
        return;
    }
    ESP_LOGI(TAG, "Driver away: starting camera, thermal, mmWave and temp/humidity sensors");
    if (!camera_sensor_start()) {
        ESP_LOGW(TAG, "Camera unavailable; continuing without face detection");
    }
    thermal_start();
    temp_humidity_sensor_init();
    mmwave_sensor_start();
    s_active = true;
}

void presence_sensors_stop()
{
    if (!s_active) {
        return;
    }
    ESP_LOGI(TAG, "Driver near: stopping all presence sensors");
    camera_sensor_stop();
    thermal_stop();
    mmwave_sensor_stop();
    temp_humidity_sensor_deinit();
    s_active = false;
}

bool presence_sensors_active()
{
    return s_active;
}
