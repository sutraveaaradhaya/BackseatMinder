#pragma once

// DFRobot SEN0609/C4001 presence sensor on UART1 (GPIO 41 RX, GPIO 42 TX).

// Installs the UART and starts the sensor. The first start after boot also
// sends the full configuration (takes about 3 seconds).
void mmwave_sensor_start();

// Sends sensorStop and releases the UART. Safe to call when not started.
void mmwave_sensor_stop();

// Processes any pending sensor reports and returns the latest presence state.
// Always false while stopped.
bool mmwave_sensor_presence_detected();
