#pragma once

#include "esp_camera.h"

// Converts a camera frame to JPEG and sends it over native USB, at most a few
// development preview frames per second. Frames in between are skipped. The
// caller keeps ownership of the frame.
void usb_camera_stream_send(const camera_fb_t *frame);
