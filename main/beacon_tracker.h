#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * Initializes the BLE controller and starts continuous background scanning.
 */
void beacon_tracker_init(void);

/**
 * Returns true if the beacon is detected within 5 meters.
 * Returns false if the beacon is > 5 meters away or hasn't been seen recently.
 */
bool beacon_tracker_driver_near(void);

#ifdef __cplusplus
}
#endif