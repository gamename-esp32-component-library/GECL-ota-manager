#include "gecl-ota-manager.h"

#include <inttypes.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// External Certificate Authority (CA) certificate for HTTPS connection to AWS S3
extern const uint8_t server_cert_pem_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_AmazonRootCA1_pem_end");

// Logging tag for OTA messages
static const char *TAG = "OTA";

// Global variables
static bool ota_in_progress = false;
static SemaphoreHandle_t ota_mutex = NULL;

/**
 * Retrieves the current local timestamp and formats it as a string.
 * This is used for logging and storing the OTA update time.
 */
void get_current_timestamp(char *buffer, size_t max_len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);                              // Convert to local time
    strftime(buffer, max_len, "%Y-%m-%d_%H-%M-%S", &timeinfo); // Format the timestamp
}

/**
 * Writes the OTA update timestamp to NVS (Non-Volatile Storage).
 * This allows the device to track when the last successful OTA update occurred.
 */
esp_err_t write_ota_timestamp_to_nvs(const char *timestamp) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_flash_init(); // Initialize NVS (if not already initialized)
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open("storage", NVS_READWRITE, &nvs_handle); // Open the NVS handle for writing
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, "ota_timestamp", timestamp); // Write the OTA timestamp to NVS
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write timestamp to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle); // Commit the changes to NVS
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle); // Close the NVS handle
    return err;
}

/**
 * Initialize the OTA handler. Ensures that the OTA system starts in a clean state.
 */
void init_ota_handler() {
    ESP_LOGI(TAG, "Initializing OTA handler...");
    ota_in_progress = false;

    // Initialize the mutex for OTA state protection
    if (ota_mutex == NULL) {
        ota_mutex = xSemaphoreCreateMutex();
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &ota_event_handler, NULL));
    ESP_LOGI(TAG, "OTA event handler registered.");
}

/* Event handler for OTA events */
static void ota_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == ESP_HTTPS_OTA_EVENT) {
        switch (event_id) {
        case ESP_HTTPS_OTA_START:
            ESP_LOGI(TAG, "OTA started");
            break;
        case ESP_HTTPS_OTA_CONNECTED:
            ESP_LOGI(TAG, "Connected to server");
            break;
        case ESP_HTTPS_OTA_FINISH:
            ESP_LOGI(TAG, "OTA finished successfully.");
            break;
        case ESP_HTTPS_OTA_ABORT:
            ESP_LOGE(TAG, "OTA aborted.");
            break;
        default:
            ESP_LOGW(TAG, "Unhandled OTA event: %d", event_id);
            break;
        }
    }
}

/**
 * Start the OTA process. Ensures only one OTA process is running at a time.
 */
void ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting OTA Task...");

    // Check if another OTA process is already running
    if (xSemaphoreTake(ota_mutex, portMAX_DELAY) == pdTRUE) {
        if (ota_in_progress) {
            ESP_LOGW(TAG, "OTA process already in progress. Aborting new task.");
            xSemaphoreGive(ota_mutex);
            vTaskDelete(NULL);
            return;
        }
        ota_in_progress = true; // Set the flag to indicate an OTA is in progress
        xSemaphoreGive(ota_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to take OTA mutex. Aborting task.");
        vTaskDelete(NULL);
        return;
    }

    const ota_config_t *ota = (const ota_config_t *)pvParameter;
    ESP_LOGI(TAG, "Using URL: %s", ota->url);

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = ESP_OK;

    // Configure OTA client
    esp_http_client_config_t http_config = {
        .url = ota->url,
        .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = true,
        .max_http_request_size = 4096,
    };

    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA Begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow other tasks to run
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle)) {
        err = esp_https_ota_finish(https_ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA update successful. Writing timestamp to NVS...");

            char timestamp[20];
            get_current_timestamp(timestamp, sizeof(timestamp));
            if (write_ota_timestamp_to_nvs(timestamp) == ESP_OK) {
                ESP_LOGI(TAG, "OTA timestamp written to NVS: %s", timestamp);
            }
            ESP_LOGI(TAG, "Rebooting...");
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA Finish failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Incomplete data received during OTA.");
    }

cleanup:
    esp_https_ota_abort(https_ota_handle);
    if (xSemaphoreTake(ota_mutex, portMAX_DELAY) == pdTRUE) {
        ota_in_progress = false; // Reset the flag
        xSemaphoreGive(ota_mutex);
    }
    ESP_LOGI(TAG, "OTA process ended.");
    vTaskDelete(NULL);
}
