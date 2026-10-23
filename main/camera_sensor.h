#pragma once

// Owns the OV camera, face detection and the USB camera preview. Nothing is
// initialized until camera_sensor_start(); camera_sensor_stop() shuts the
// camera driver down again so it is genuinely off, not merely ignored.

// Powers up the camera and starts capture + face detection. Returns false if
// the camera could not be initialized (the rest of the device keeps running).
bool camera_sensor_start();

// Stops capture and deinitializes the camera driver. Blocks until done.
void camera_sensor_stop();
