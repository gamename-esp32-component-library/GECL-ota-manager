/*
 * OTA Update Process for ESP32 Device
 * ===================================
 *
 * This document outlines the process of Over-The-Air (OTA) firmware updates for an ESP32 device using
 * ESP-IDF (Espressif IoT Development Framework). The OTA handler uses MQTT for message reception,
 * HTTPS for secure firmware downloads, and NVS (Non-Volatile Storage) for tracking OTA update timestamps.
 *
 * Key Features:
 * 1. OTA updates initiated via MQTT message.
 * 2. HTTPS used for secure firmware download.
 * 3. NVS used for storing update timestamps.
 * 4. Timeouts and retry mechanisms ensure robust error handling.
 * 5. Watchdog timers to prevent task hangs during the OTA process.
 * 6. Automatic reboot after successful update.
 *
 */

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

// External Certificate Authority (CA) certificate for HTTPS connection to AWS S3
extern const uint8_t server_cert_pem_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_AmazonRootCA1_pem_end");

// Logging tag for OTA messages
static const char *TAG = "OTA";

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

    // Initialize NVS (if not already initialized)
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Open the NVS handle for writing
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Write the OTA timestamp to NVS
    err = nvs_set_str(nvs_handle, "ota_timestamp", timestamp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write timestamp to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit the changes to NVS
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit to NVS: %s", esp_err_to_name(err));
    }

    // Close the NVS handle
    nvs_close(nvs_handle);
    return err;
}

/**
 * Retrieves the MAC address burned into the ESP32 and formats it as a string.
 * This MAC address is used as a unique identifier during the OTA process.
 */
void get_burned_in_mac_address(char *mac_str) {
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read the station MAC address
    if (ret == ESP_OK) {
        snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(mac_str, 18, "ERROR");
    }
}

/* Event handler for catching system events */
static void ota_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == ESP_HTTPS_OTA_EVENT) {
        switch (event_id) {
        case ESP_HTTPS_OTA_START:
            ESP_LOGI(TAG, "OTA started");
            break;
        case ESP_HTTPS_OTA_CONNECTED:
            ESP_LOGI(TAG, "Connected to server");
            break;
        case ESP_HTTPS_OTA_GET_IMG_DESC:
            ESP_LOGI(TAG, "Reading Image Description");
            break;
        case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
            ESP_LOGI(TAG, "Verifying chip id of new image: %d", *(esp_chip_id_t *)event_data);
            break;
        case ESP_HTTPS_OTA_DECRYPT_CB:
            ESP_LOGI(TAG, "Callback to decrypt function");
            break;
        case ESP_HTTPS_OTA_WRITE_FLASH:
            ESP_LOGD(TAG, "Writing to flash: %d written", *(int *)event_data);
            break;
        case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
            ESP_LOGI(TAG, "Boot partition updated. Next Partition: %d", *(esp_partition_subtype_t *)event_data);
            break;
        case ESP_HTTPS_OTA_FINISH:
            ESP_LOGI(TAG, "OTA finish");
            break;
        case ESP_HTTPS_OTA_ABORT:
            ESP_LOGI(TAG, "OTA abort");
            break;
        }
    }
}

void init_ota_handler() {
    ESP_LOGI(TAG, "Initializing OTA handler...");

    // Register the OTA event handler
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &ota_event_handler, NULL));

    ESP_LOGI(TAG, "OTA event handler registered.");
}

static esp_err_t _http_client_init_cb(esp_http_client_handle_t http_client) {
    esp_err_t err = ESP_OK;
    /* Uncomment to add custom headers to HTTP request */
    // err = esp_http_client_set_header(http_client, "Custom-Header", "Value");
    return err;
}
static esp_err_t validate_image_header(esp_app_desc_t *new_app_info) {
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Currently running firmware version: %s", running_app_info.version);
    }
#if 0
    if (memcmp(new_app_info->version, running_app_info.version, sizeof(new_app_info->version)) == 0) {
        ESP_LOGW(TAG, "Current running version is the same as new version. Update discontinued.");
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
}
void stop_mqtt(esp_mqtt_client_handle_t mqtt_client) {
    esp_err_t ret;

    ret = esp_mqtt_client_stop(mqtt_client);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client stopped successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to stop MQTT client: %s", esp_err_to_name(ret));
    }

    // Optionally destroy the MQTT client to free up resources
    ret = esp_mqtt_client_destroy(mqtt_client);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client destroyed successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to destroy MQTT client: %s", esp_err_to_name(ret));
    }
}

void stop_wifi() {
    esp_err_t ret;

    // Stop Wi-Fi
    ret = esp_wifi_stop();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi stopped successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(ret));
    }

    // Optionally deinitialize Wi-Fi
    ret = esp_wifi_deinit();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi deinitialized successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize Wi-Fi: %s", esp_err_to_name(ret));
    }
}

void ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting...");

    const ota_config_t *ota = (const ota_config_t *)pvParameter;
    ESP_LOGI(TAG, "Using URL: %s", ota->url);

    ESP_LOGI(TAG, "Disabling Wi-Fi power save mode during OTA...");
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "Turn off MQTT so we can perform the update");
    stop_mqtt(ota->mqtt_client);

    esp_err_t ota_finish_err = ESP_OK;
    esp_http_client_config_t _http_config = {
        .url = ota->url,
        .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &_http_config,
        // Register a callback to be invoked after esp_http_client is initialized
        .http_client_init_cb = _http_client_init_cb,
        .partial_http_download = true,
        .max_http_request_size = 4096,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Begin failed");
        goto ota_end;
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc failed");
        goto ota_end;
    }
    err = validate_image_header(&app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image header verification failed");
        goto ota_end;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        // ESP_LOGI(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay to allow other tasks to run
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGE(TAG, "Complete data was not received.");
        goto ota_end;
    } else {
        ota_finish_err = esp_https_ota_finish(https_ota_handle);
        if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
            ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful! Writing timestamp to NVS...");
            // Get the current timestamp
            char timestamp[20];
            get_current_timestamp(timestamp, sizeof(timestamp));

            // Write the OTA timestamp to NVS for tracking
            esp_err_t nvs_err = write_ota_timestamp_to_nvs(timestamp);
            if (nvs_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA timestamp to NVS: %s", esp_err_to_name(nvs_err));
                goto ota_end;
            } else {
                ESP_LOGI(TAG, "timestamp written to NVS: %s", timestamp);
            }

            vTaskDelay(1000 / portTICK_PERIOD_MS);
            ESP_LOGI(TAG, "Stopping WiFi ...");
            stop_wifi();
            ESP_LOGI(TAG, "Rebooting ...");
            esp_restart();
        } else {
            if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            }
            ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", ota_finish_err);
            goto ota_end;
        }
    }

ota_end:
    esp_https_ota_abort(https_ota_handle);
    ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed");
    stop_wifi();
    esp_restart();
}
