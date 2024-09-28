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
#include "freertos/timers.h"
#include "gecl-mqtt-manager.h"
#include "nvs.h"
#include "nvs_flash.h"
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
    do                                                       \
    {                                                        \
        xEventGroupSetBits(ota_event_group, OTA_FAILED_BIT); \
        esp_task_wdt_delete(NULL);                           \
        vTaskDelete(NULL);                                   \
    } while (0)

#define OTA_COMPLETE_EXIT()                                    \
    do                                                         \
    {                                                          \
        xEventGroupSetBits(ota_event_group, OTA_COMPLETE_BIT); \
        esp_task_wdt_delete(NULL);                             \
        vTaskDelete(NULL);                                     \
    } while (0)

// Define a timer handle
static TimerHandle_t reboot_timer = NULL;
static TimerHandle_t ota_timeout_timer = NULL;

// Timeout period for OTA task (15 minutes in milliseconds)
#define OTA_TIMEOUT_PERIOD (CONFIG_GECL_OTA_TIMEOUT_MINUTES * 60 * 1000)

void get_burned_in_mac_address(char *mac_str)
{
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret == ESP_OK)
    {
        snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    else
    {
        snprintf(mac_str, 18, "ERROR");
    }
}

// Reboot callback function
void reboot_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Rebooting system...");
    esp_restart();
}

// OTA timeout callback function
void ota_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGE(TAG, "OTA task timed out. Aborting...");
    OTA_FAIL_EXIT();
}

// Function to schedule the reboot
void schedule_reboot(int delay_ms)
{
    if (reboot_timer == NULL)
    {
        // Create a one-shot timer
        reboot_timer = xTimerCreate("reboot_timer", pdMS_TO_TICKS(delay_ms), pdFALSE, (void *)0, reboot_callback);
    }
    // Start the timer
    xTimerStart(reboot_timer, 0);
}

// Function to schedule the OTA timeout
void schedule_ota_timeout(int timeout_ms)
{
    if (ota_timeout_timer == NULL)
    {
        // Create a one-shot timer
        ota_timeout_timer =
            xTimerCreate("ota_timeout_timer", pdMS_TO_TICKS(timeout_ms), pdFALSE, (void *)0, ota_timeout_callback);
    }
    // Start the timer
    xTimerStart(ota_timeout_timer, 0);
}

// Function to get the current local timestamp
void get_current_timestamp(char *buffer, size_t max_len)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buffer, max_len, "%Y-%m-%d_%H-%M-%S", &timeinfo);
}

// Function to write the OTA timestamp to NVS
esp_err_t write_ota_timestamp_to_nvs(const char *timestamp)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Initialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Open NVS handle
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Write the timestamp to NVS
    err = nvs_set_str(nvs_handle, "ota_timestamp", timestamp);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write timestamp to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit the write
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit to NVS: %s", esp_err_to_name(err));
    }

    // Close NVS handle
    nvs_close(nvs_handle);
    return err;
}

void ota_handler_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA handler task");

    esp_mqtt_event_handle_t mqtt_event = (esp_mqtt_event_handle_t)pvParameter;
    esp_mqtt_client_handle_t my_mqtt_client = mqtt_event->client;

    if (mqtt_event->data_len == 0)
    {
        ESP_LOGE(TAG, "Empty payload received! Aborting OTA...");
        vTaskDelete(NULL);
    }

    char *payload_data = malloc(mqtt_event->data_len + 1);
    strncpy(payload_data, mqtt_event->data, mqtt_event->data_len);
    payload_data[mqtt_event->data_len] = '\0';

    ESP_LOGI(TAG, "Lengths - Original: %d Payload: %d", mqtt_event->data_len, strlen(payload_data));

    ESP_LOGI(TAG, "Payload data: %s", payload_data);

    cJSON *json = cJSON_Parse(payload_data);
    if (!json)
    {
        ESP_LOGE(TAG, "Failed to parse JSON string");
        vTaskDelete(NULL);
    }

    free(payload_data);

    char mac_address[18];
    get_burned_in_mac_address(mac_address);
    ESP_LOGI(TAG, "Burned-In MAC Address: %s\n", mac_address);

    cJSON *host_key = cJSON_GetObjectItem(json, mac_address);
    const char *host_key_value = cJSON_GetStringValue(host_key);
    if (!host_key || !host_key_value)
    {
        ESP_LOGE(TAG, "Invalid or missing '%s' key in JSON", mac_address);
        cJSON_Delete(json);
        vTaskDelete(NULL);
    }

    char url_buffer[MAX_URL_LENGTH];
    strncpy(url_buffer, host_key_value, MAX_URL_LENGTH - 1);
    url_buffer[MAX_URL_LENGTH - 1] = '\0';

    ESP_LOGI(TAG, "Host key value: %s", url_buffer);

    esp_http_client_config_t config = {
        .url = url_buffer,
        .cert_pem = (char *)AmazonRootCA1_pem,
        .timeout_ms = 30000,
    };

    cJSON_Delete(json);

    ESP_LOGI(TAG, "Starting OTA with URL: %s", config.url);

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition)
    {
        ESP_LOGE(TAG, "Failed to find update partition");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Writing to partition: %s", update_partition->label);

    // Ensure the event group is created
    if (ota_event_group == NULL)
    {
        ota_event_group = xEventGroupCreate();
        if (ota_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create event group");
            vTaskDelete(NULL);
        }
    }

    // Create the OTA task and pass the ota_handle
    xTaskCreate(&ota_task, "ota_task", 8192, ota_handle, 10, NULL);

    // Start the retry loop
    const int max_retries = 3;
    int attempt = 0;
    while (attempt < max_retries)
    {
        // Wait for OTA completion
        EventBits_t bits = xEventGroupWaitBits(ota_event_group, OTA_COMPLETE_BIT | OTA_FAILED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & OTA_COMPLETE_BIT)
        {
            ESP_LOGW(TAG, "OTA handler shows copy successful");
            break;
        }
        else if (bits & OTA_FAILED_BIT)
        {
            // Handle OTA failure
            ESP_LOGE(TAG, "OTA attempt %d failed", attempt + 1);
            attempt++;
            if (attempt < max_retries)
            {
                ESP_LOGI(TAG, "Retrying OTA...");
            }
            else
            {
                ESP_LOGE(TAG, "Max OTA attempts reached. OTA FAILED");
                break;
            }
        }
    }

    // Stop MQTT client
    esp_mqtt_client_stop(my_mqtt_client);

    // Disconnect WiFi
    esp_wifi_disconnect();

    // Schedule a reboot after OTA attempts
    schedule_reboot(10000); // Schedule a reboot with a 10-second delay

    // Delete the handler task
    vTaskDelete(NULL);
}

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA task");

    esp_https_ota_handle_t ota_handle = (esp_https_ota_handle_t)pvParameter;

    // Add the task to the watchdog
    esp_task_wdt_add(NULL);

    // Schedule the OTA timeout
    schedule_ota_timeout(OTA_TIMEOUT_PERIOD);

    while (1)
    {
        esp_err_t err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            // Continue downloading OTA update
            vTaskDelay(pdMS_TO_TICKS(100)); // Add a delay to avoid busy-waiting
            // Reset the watchdog timer
            esp_task_wdt_reset();
            continue;
        }
        else if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "OTA perform error: %s", esp_err_to_name(err));
            OTA_FAIL_EXIT();
        }
        else
        {
            break;
        }
    }

    if (esp_https_ota_is_complete_data_received(ota_handle))
    {
        esp_err_t ota_finish_err = esp_https_ota_finish(ota_handle);
        if (ota_finish_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Success - OTA update complete!");

            // Get the current timestamp
            char timestamp[20];
            get_current_timestamp(timestamp, sizeof(timestamp));

            // Write the timestamp to NVS
            esp_err_t nvs_err = write_ota_timestamp_to_nvs(timestamp);
            if (nvs_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to write OTA timestamp to NVS: %s", esp_err_to_name(nvs_err));
                OTA_FAIL_EXIT();
            }
            ESP_LOGI(TAG, "OTA timestamp written to NVS: %s", timestamp);

            OTA_COMPLETE_EXIT();
        }
        else
        {
            ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ota_finish_err));
            OTA_FAIL_EXIT();
        }
    }
    else
    {
        ESP_LOGE(TAG, "Complete data was not received.");
        OTA_FAIL_EXIT();
    }

    // Remove the task from the watchdog
    esp_task_wdt_delete(NULL);
}
