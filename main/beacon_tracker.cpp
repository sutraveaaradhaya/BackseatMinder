#include "beacon_tracker.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "beacon_tracker";

// --- Settings ---
// Target MAC: "78:9f:38:08:0b:e5"
static esp_bd_addr_t target_mac = {0x78, 0x9f, 0x38, 0x08, 0x0b, 0xe5};

// RSSI threshold in dBm. Median RSSI at or above this means the driver is
// NEAR. -50 is very close (roughly arm's length) and is only for bench testing;
// a real "walked away from the car" radius is more like -70 to -80.
static const int RSSI_THRESHOLD = -50;
// The median must drop this many dB below the threshold before the driver
// counts as AWAY, so a signal hovering at the threshold does not flap.
static const int RSSI_HYSTERESIS = 5;

static const int WIN_SIZE = 6;
static const uint32_t BEACON_TIMEOUT_MS = 5000; // 5 seconds without seeing beacon = away

// --- State Variables ---
// Written by the Bluetooth task, read by the main loop.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int rssi_buf[WIN_SIZE];
static int rssi_count = 0;
static int rssi_idx = 0;
static TickType_t last_seen_tick = 0;
static TickType_t init_tick = 0;
static bool s_driver_near = true;

static int median_rssi() {
    int tmp[WIN_SIZE];
    int n = rssi_count;
    for (int i = 0; i < n; i++) tmp[i] = rssi_buf[i];

    // Simple insertion sort
    for (int i = 1; i < n; i++) {
        int k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    return tmp[n / 2];
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            esp_ble_gap_start_scanning(0);
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
            if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                // Match target MAC address
                if (memcmp(scan_result->scan_rst.bda, target_mac, sizeof(esp_bd_addr_t)) == 0) {
                    int rssi = scan_result->scan_rst.rssi;

                    taskENTER_CRITICAL(&s_lock);
                    rssi_buf[rssi_idx] = rssi;
                    rssi_idx = (rssi_idx + 1) % WIN_SIZE;
                    if (rssi_count < WIN_SIZE) {
                        rssi_count++;
                    }
                    last_seen_tick = xTaskGetTickCount();
                    taskEXIT_CRITICAL(&s_lock);
                }
            } else if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                // Duration 0 should scan forever; restart if the stack stops anyway.
                esp_ble_gap_start_scanning(0);
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            esp_ble_gap_start_scanning(0);
            break;

        default:
            break;
    }
}

bool beacon_tracker_init(void) {
    init_tick = xTaskGetTickCount();

    // The ESP32-S3 has no Classic Bluetooth; give its memory back to the heap.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Initialize bt controller failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Enable bt controller failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Init bluedroid failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Enable bluedroid failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(TAG, "Gap register failed: %s", esp_err_to_name(ret));
        return false;
    }

    static esp_ble_scan_params_t ble_scan_params = {
        .scan_type              = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval          = 0x50,
        .scan_window            = 0x30,
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
    };

    ret = esp_ble_gap_set_scan_params(&ble_scan_params);
    if (ret) {
        ESP_LOGE(TAG, "Set scan params failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Beacon tracker initialized.");
    return true;
}

bool beacon_tracker_driver_near(void) {
    const TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_lock);
    const int count = rssi_count;
    const TickType_t seen = last_seen_tick;
    const int med_rssi = count ? median_rssi() : 0;
    taskEXIT_CRITICAL(&s_lock);

    if (count == 0) {
        // Give the scanner time to find the beacon after boot before
        // deciding that the driver has gone.
        s_driver_near = now - init_tick <= pdMS_TO_TICKS(BEACON_TIMEOUT_MS);
    } else if (now - seen > pdMS_TO_TICKS(BEACON_TIMEOUT_MS)) {
        // Not seen recently: assume the driver walked away (or the beacon died).
        s_driver_near = false;
    } else if (count >= WIN_SIZE / 2) {
        if (med_rssi >= RSSI_THRESHOLD) {
            s_driver_near = true;
        } else if (med_rssi < RSSI_THRESHOLD - RSSI_HYSTERESIS) {
            s_driver_near = false;
        }
        // In between: keep the previous decision.
    }
    return s_driver_near;
}

int beacon_tracker_rssi(void) {
    taskENTER_CRITICAL(&s_lock);
    const int rssi = rssi_count ? median_rssi() : 0;
    taskEXIT_CRITICAL(&s_lock);
    return rssi;
}
