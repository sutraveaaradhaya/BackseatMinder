#pragma once

// Groups every back-seat sensor (camera + face detection, MLX90640 thermal
// camera, C4001 mmWave radar and AM2302/DHT22) so they are powered up only
// while the driver is away and shut down again when the driver returns.

// Starts all sensors. A sensor that fails to start is logged and skipped.
// Takes a few seconds (mmWave configuration). Call from the main task only.
void presence_sensors_start();

// Stops and releases all sensors. Blocks until the camera and thermal tasks
// have finished their current frame. Call from the main task only.
void presence_sensors_stop();

// True between presence_sensors_start() and presence_sensors_stop().
bool presence_sensors_active();
