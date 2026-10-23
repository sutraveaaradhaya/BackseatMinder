#pragma once

#include "esp_camera.h"

// Runs a two-stage ESP-WHO face detector, tuned to favor recall over the
// stock example's false-positive rate, on one RGB565 camera frame. Detected
// faces are boxed on the frame. Call from one task only.
bool tuned_face_detect(camera_fb_t *frame);

// Returns whether a face was detected since the previous call.
bool get_tuned_face_detected();
