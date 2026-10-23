#include "camera_sensor.h"

#include <atomic>

#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tuned_face_detection.h"
#include "usb_camera_stream.h"
#include "who_camera.h" // CAMERA_PIN_* from the ESP-WHO camera configuration.

namespace {

const char *TAG = "camera_sensor";

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_stopped = nullptr;
std::atomic<bool> s_should_run{false};
bool s_running = false; // Only touched by the caller of start/stop.

bool init_camera()
{
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = CAMERA_PIN_SIOD;
    config.pin_sccb_scl = CAMERA_PIN_SIOC;
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Same per-sensor orientation fixes as ESP-WHO's register_camera().
    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID || s->id.PID == OV2640_PID) {
        s->set_vflip(s, 1);
    } else if (s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);
    } else if (s->id.PID == GC032A_PID) {
        s->set_vflip(s, 1);
    }
    if (s->id.PID == OV3660_PID) {
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }
    return true;
}

// Capture -> face detection -> optional USB preview, one frame at a time, so
// stopping never leaves a camera buffer in flight in another task.
void camera_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_should_run.load()) {
            camera_fb_t *frame = esp_camera_fb_get();
            if (!frame) {
                continue;
            }
            tuned_face_detect(frame);
            usb_camera_stream_send(frame);
            esp_camera_fb_return(frame);
        }
        xSemaphoreGive(s_stopped);
    }
}

} // namespace

bool camera_sensor_start()
{
    if (s_running) {
        return true;
    }
    if (!s_task) {
        s_stopped = xSemaphoreCreateBinary();
        if (!s_stopped ||
            xTaskCreatePinnedToCore(camera_task, "camera", 10 * 1024, nullptr, 5,
                                    &s_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "Could not create camera task");
            return false;
        }
    }
    if (!init_camera()) {
        return false;
    }
    get_tuned_face_detected(); // Drop any result from an earlier session.
    s_should_run.store(true);
    xTaskNotifyGive(s_task);
    s_running = true;
    ESP_LOGI(TAG, "Camera and face detection started");
    return true;
}

void camera_sensor_stop()
{
    if (!s_running) {
        return;
    }
    s_should_run.store(false);
    // Bounded: esp_camera_fb_get() times out if frames stop arriving.
    xSemaphoreTake(s_stopped, portMAX_DELAY);
    esp_camera_deinit();
    get_tuned_face_detected();
    s_running = false;
    ESP_LOGI(TAG, "Camera and face detection stopped");
}
