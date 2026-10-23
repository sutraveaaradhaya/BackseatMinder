#include "usb_camera_stream.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "usb_telemetry.h"

namespace {

constexpr int64_t FRAME_INTERVAL_US = 1000000 / 3; // Target three preview FPS.
const char *TAG = "usb_camera";
int64_t s_last_frame_sent = 0;

} // namespace

void usb_camera_stream_send(const camera_fb_t *frame)
{
    const int64_t now = esp_timer_get_time();
    if (!frame || !usb_telemetry_host_connected() ||
        now - s_last_frame_sent < FRAME_INTERVAL_US) {
        return;
    }

    // Rate-limit attempts too: an unplugged development cable should
    // not make JPEG conversion run for every captured camera frame.
    s_last_frame_sent = now;
    uint8_t *jpeg = nullptr;
    size_t jpeg_length = 0;
    if (frame2jpg(const_cast<camera_fb_t *>(frame), 50, &jpeg, &jpeg_length)) {
        usb_telemetry_send_camera_frame(jpeg, jpeg_length);
        free(jpeg);
    } else {
        ESP_LOGW(TAG, "JPEG conversion failed");
    }
}
