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

// RSSI Threshold: if median RSSI is lower (more negative) than this, driver is AWAY (> 5m).
// Adjust this value after testing if needed (e.g., -70 for closer, -80 for farther).
static const int RSSI_THRESHOLD = -50; 

static const int WIN_SIZE = 6;
static const uint32_t BEACON_TIMEOUT_MS = 5000; // 5 seconds without seeing beacon = away

// --- State Variables ---
static int rssi_buf[WIN_SIZE];
static int rssi_count = 0;
static int rssi_idx = 0;
static TickType_t last_seen_tick = 0;

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
                    
                    rssi_buf[rssi_idx] = rssi;
                    rssi_idx = (rssi_idx + 1) % WIN_SIZE;
                    if (rssi_count < WIN_SIZE) {
                        rssi_count++;
                    }
                    last_seen_tick = xTaskGetTickCount();
                }
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

void beacon_tracker_init(void) {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Initialize bt controller failed");
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Enable bt controller failed");
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Init bluedroid failed");
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Enable bluedroid failed");
        return;
    }

    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(TAG, "Gap register failed");
        return;
    }

    static esp_ble_scan_params_t ble_scan_params = {
        .scan_type              = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval          = 0x50,
        .scan_window            = 0x30,
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
    };

    esp_ble_gap_set_scan_params(&ble_scan_params);
    ESP_LOGI(TAG, "Beacon tracker initialized.");
}

bool beacon_tracker_driver_near(void) {
    // If we haven't seen the beacon in 5 seconds, assume driver walked away
    if (xTaskGetTickCount() - last_seen_tick > pdMS_TO_TICKS(BEACON_TIMEOUT_MS)) {
        return false; 
    }

    // Require at least a few samples before making a judgment
    if (rssi_count < (WIN_SIZE / 2)) {
        return false; 
    }

    int med_rssi = median_rssi();
    
    // Returns true if RSSI is >= -75 dBm (strong/near), false if weaker than -75 dBm (away)
    return med_rssi >= RSSI_THRESHOLD;
}