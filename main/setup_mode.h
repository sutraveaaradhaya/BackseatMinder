#pragma once

#include <stddef.h>

void setup_mode_init();        // Call once in app_main before the loop (NVS + button GPIO)
bool setup_mode_button_pressed(); // Returns true if button is held (debounced)
void enter_setup_mode();       // Blocks until user confirms in portal, then restarts

// Call once at boot, before Bluetooth or any sensor is started. True when the
// device should open setup: nothing valid is saved yet, the setup button is
// held, or setup_mode_restart_into_setup() requested it before restarting.
bool setup_mode_requested_at_boot();

// Restarts the device straight into setup mode, so setup always runs with
// Bluetooth and every sensor off. Does not return.
void setup_mode_restart_into_setup();

// True when a usable name and primary phone number are saved.
bool setup_mode_config_complete();

// Reads only the saved primary number. Removes common display formatting;
// returns false (and clears the output) if missing, malformed or too long.
bool setup_mode_get_phone_number(char *buffer, size_t capacity);
// Reads the saved name; false if missing, empty, or the buffer is too small.
bool setup_mode_get_name(char *buffer, size_t capacity);
