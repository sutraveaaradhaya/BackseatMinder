#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * Initializes the BLE controller and starts continuous passive scanning for
 * the driver's beacon. Returns false if Bluetooth could not be started.
 */
bool beacon_tracker_init(void);

/**
 * Returns true while the driver's beacon is considered near.
 *
 * The driver is assumed near for a short grace period after start-up so the
 * scanner can find the beacon. After that, the driver is away when the beacon
 * has not been seen recently or its median RSSI falls below the threshold
 * (with hysteresis so a signal hovering at the threshold does not flap).
 */
bool beacon_tracker_driver_near(void);

/**
 * Latest median RSSI of the beacon in dBm, or 0 if it has not been seen.
 */
int beacon_tracker_rssi(void);

#ifdef __cplusplus
}
#endif
