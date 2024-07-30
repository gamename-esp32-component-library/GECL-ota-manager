#include "gecl-ota-manager.h"

#include <inttypes.h>
#include <mbedtls/sha256.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "gecl-logger-manager.h"
#include "gecl-misc-util-manager.h"
#include "gecl-mqtt-manager.h"
#include "gecl-rgb-led-manager.h"
#include "sdkconfig.h"

extern const uint8_t AmazonRootCA1_pem[];

static const char *TAG = "OTA";

static EventGroupHandle_t ota_event_group;
const int OTA_COMPLETE_BIT = BIT0;
const int OTA_FAILED_BIT = BIT1;

#define MAX_RETRIES 5
#define LOG_PROGRESS_INTERVAL 100
#define MAX_URL_LENGTH 512
#define OTA_PROGRESS_MESSAGE_LENGTH 128

#define OTA_FAIL_EXIT()                                      \
    do {                                                     \
        xEventGroupSetBits(ota_event_group, OTA_FAILED_BIT); \
        esp_task_wdt_delete(NULL);                           \
        vTaskDelete(NULL);                                   \
    } while (0)

#define OTA_COMPLETE_EXIT()                                    \
    do {                                                       \
        xEventGroupSetBits(ota_event_group, OTA_COMPLETE_BIT); \
        esp_task_wdt_delete(NULL);                             \
        vTaskDelete(NULL);                                     \
    } while (0)

char ota_progress_buffer[OTA_PROGRESS_MESSAGE_LENGTH];

// Helper function to handle NVS errors and close the handle
static esp_err_t nvs_get_u32_safe(nvs_handle_t nvs_handle, const char *key, uint32_t *out_value) {
    esp_err_t err = nvs_get_u32(nvs_handle, key, out_value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to get value for %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

// Check if the system was booted after an OTA update
bool was_booted_after_ota_update(void) {
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason != ESP_RST_SW) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        return false;
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *boot_partition = esp_ota_get_boot_partition();

    if (!running_partition || !boot_partition) {
        ESP_LOGE(TAG, "Failed to get partition information.");
        nvs_close(nvs_handle);
        return false;
    }

    uint32_t saved_boot_part_addr = 0;
    err = nvs_get_u32_safe(nvs_handle, "boot_part", &saved_boot_part_addr);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved boot partition address found. Saving current boot partition.");
        err = nvs_set_u32(nvs_handle, "boot_part", boot_partition->address);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save boot partition address: %s", esp_err_to_name(err));
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        return true;
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }

    bool is_ota_update = (boot_partition->address != saved_boot_part_addr);
    if (is_ota_update) {
        ESP_LOGI(TAG, "OTA update detected.");
        err = nvs_set_u32(nvs_handle, "boot_part", boot_partition->address);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save boot partition address: %s", esp_err_to_name(err));
        }
        nvs_commit(nvs_handle);
    } else {
        ESP_LOGI(TAG, "No OTA update detected.");
    }

    nvs_close(nvs_handle);
    return is_ota_update;
}

// Convert seconds into minutes and seconds
void convert_seconds(int totalSeconds, int *minutes, int *seconds) {
    *minutes = totalSeconds / 60;
    *seconds = totalSeconds % 60;
}

// Main OTA task
void ota_task(void *pvParameter) {
    send_log_message(ESP_LOG_INFO, TAG, "Starting OTA task");

    esp_https_ota_config_t *ota_config = (esp_https_ota_config_t *)pvParameter;

    esp_task_wdt_add(NULL);

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(ota_config, &ota_handle);
    if (err != ESP_OK) {
        send_log_message(ESP_LOG_ERROR, TAG, "Failed to start OTA: %s", esp_err_to_name(err));
        OTA_FAIL_EXIT();
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        send_log_message(ESP_LOG_ERROR, TAG, "Failed to find update partition");
        OTA_FAIL_EXIT();
    }

    send_log_message(ESP_LOG_INFO, TAG, "OTA update partition: %s", update_partition->label);

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            esp_task_wdt_reset();
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        } else if (err != ESP_OK) {
            send_log_message(ESP_LOG_ERROR, TAG, "OTA perform error: %s", esp_err_to_name(err));
            OTA_FAIL_EXIT();
        } else {
            break;
        }
    }

    if (esp_https_ota_is_complete_data_received(ota_handle)) {
        esp_err_t ota_finish_err = esp_https_ota_finish(ota_handle);
        if (ota_finish_err == ESP_OK) {
            send_log_message(ESP_LOG_INFO, TAG, "Success - OTA update complete!");
            OTA_COMPLETE_EXIT();
        } else {
            send_log_message(ESP_LOG_ERROR, TAG, "OTA update failed: %s", esp_err_to_name(ota_finish_err));
            OTA_FAIL_EXIT();
        }
    } else {
        send_log_message(ESP_LOG_ERROR, TAG, "Complete data was not received.");
        OTA_FAIL_EXIT();
    }
}

void ota_handler_task(void *pvParameter) {
    esp_mqtt_event_handle_t mqtt_event = (esp_mqtt_event_handle_t)pvParameter;
    esp_mqtt_client_handle_t my_mqtt_client = mqtt_event->client;

    cJSON *json = cJSON_Parse(mqtt_event->data);
    if (!json) {
        send_log_message(ESP_LOG_ERROR, TAG, "Failed to parse JSON string");
        OTA_FAIL_EXIT();
    }

    char mac_address[18];
    get_burned_in_mac_address(mac_address);
    send_log_message(ESP_LOG_INFO, TAG, "Burned-In MAC Address: %s\n", mac_address);

    cJSON *host_key = cJSON_GetObjectItem(json, mac_address);
    const char *host_key_value = cJSON_GetStringValue(host_key);
    if (!host_key || !host_key_value) {
        send_log_message(ESP_LOG_ERROR, TAG, "Invalid or missing '%s' key in JSON", mac_address);
        cJSON_Delete(json);
        OTA_FAIL_EXIT();
    }

    char url_buffer[MAX_URL_LENGTH];
    strncpy(url_buffer, host_key_value, MAX_URL_LENGTH - 1);
    url_buffer[MAX_URL_LENGTH - 1] = '\0';

    send_log_message(ESP_LOG_INFO, TAG, "Host key value: %s", url_buffer);

    esp_http_client_config_t config = {
        .url = url_buffer,
        .cert_pem = (char *)AmazonRootCA1_pem,
        .timeout_ms = 30000,
    };

    cJSON_Delete(json);

    send_log_message(ESP_LOG_INFO, TAG, "Starting OTA with URL: %s", config.url);

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    // Ensure the event group is created
    if (ota_event_group == NULL) {
        ota_event_group = xEventGroupCreate();
        if (ota_event_group == NULL) {
            send_log_message(ESP_LOG_ERROR, TAG, "Failed to create event group");
            OTA_FAIL_EXIT();
        }
    }

    // Create the OTA task and pass the ota_config
    xTaskCreate(&ota_task, "ota_task", 8192, &ota_config, 5, NULL);

    // Wait for OTA completion
    EventBits_t bits =
        xEventGroupWaitBits(ota_event_group, OTA_COMPLETE_BIT | OTA_FAILED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

    if (bits & OTA_COMPLETE_BIT) {
        // Reboot the system
        esp_restart();
    } else if (bits & OTA_FAILED_BIT) {
        // Handle OTA failure
        send_log_message(ESP_LOG_ERROR, TAG, "OTA FAILED");
    }

    // Delete the handler task
    vTaskDelete(NULL);
}
