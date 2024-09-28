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
#include "freertos/timers.h"
#include "gecl-mqtt-manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

// External Certificate Authority (CA) certificate for HTTPS connection to AWS S3
extern const uint8_t AmazonRootCA1_pem[];

// Logging tag for OTA messages
static const char *TAG = "OTA";

// Event group for OTA task synchronization
static EventGroupHandle_t ota_event_group = NULL;
const int OTA_COMPLETE_BIT = BIT0; // OTA complete event bit
const int OTA_FAILED_BIT = BIT1;   // OTA failure event bit

// Maximum retries for OTA process
#define MAX_RETRIES 5
#define LOG_PROGRESS_INTERVAL 100
#define MAX_URL_LENGTH 512
#define OTA_PROGRESS_MESSAGE_LENGTH 128

// Macro to handle OTA failure, sets failure event bit, deletes watchdog task, and terminates OTA task
#define OTA_FAIL_EXIT()                                      \
    do                                                       \
    {                                                        \
        xEventGroupSetBits(ota_event_group, OTA_FAILED_BIT); \
        esp_task_wdt_delete(NULL);                           \
        vTaskDelete(NULL);                                   \
    } while (0)

// Macro to handle OTA success, sets complete event bit, deletes watchdog task, and terminates OTA task
#define OTA_COMPLETE_EXIT()                                    \
    do                                                         \
    {                                                          \
        xEventGroupSetBits(ota_event_group, OTA_COMPLETE_BIT); \
        esp_task_wdt_delete(NULL);                             \
        vTaskDelete(NULL);                                     \
    } while (0)

// Timer handles for scheduling reboot and OTA timeout
static TimerHandle_t reboot_timer = NULL;
static TimerHandle_t ota_timeout_timer = NULL;

// Timeout period for OTA task (defined in configuration, in milliseconds)
#define OTA_TIMEOUT_PERIOD (CONFIG_GECL_OTA_TIMEOUT_MINUTES * 60 * 1000)

/**
 * Retrieves the MAC address burned into the ESP32 and formats it as a string.
 * This MAC address is used as a unique identifier during the OTA process.
 */
void get_burned_in_mac_address(char *mac_str)
{
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read the station MAC address
    if (ret == ESP_OK)
    {
        snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    else
    {
        snprintf(mac_str, 18, "ERROR");
    }
}

/**
 * Callback function to reboot the ESP32 device after a successful OTA update.
 * The device is restarted using the `esp_restart()` function.
 */
void reboot_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Rebooting system...");
    esp_restart(); // Reboot the system
}

/**
 * Callback function to handle OTA timeout.
 * If the OTA process exceeds the defined timeout period, the task is aborted.
 */
void ota_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGE(TAG, "OTA task timed out. Aborting...");
    OTA_FAIL_EXIT(); // Abort the OTA task
}

/**
 * Schedules a system reboot with a delay (in milliseconds).
 * This function creates and starts a one-shot FreeRTOS timer.
 */
void schedule_reboot(int delay_ms)
{
    if (reboot_timer == NULL)
    {
        // Create a one-shot timer for reboot
        reboot_timer = xTimerCreate("reboot_timer", pdMS_TO_TICKS(delay_ms), pdFALSE, (void *)0, reboot_callback);
    }
    // Start the timer
    xTimerStart(reboot_timer, 0);
}

/**
 * Schedules a timeout for the OTA process.
 * If the OTA process does not complete within the specified time, the OTA task will be aborted.
 */
void schedule_ota_timeout(int timeout_ms)
{
    if (ota_timeout_timer == NULL)
    {
        // Create a one-shot timer for OTA timeout
        ota_timeout_timer = xTimerCreate("ota_timeout_timer", pdMS_TO_TICKS(timeout_ms), pdFALSE, (void *)0, ota_timeout_callback);
    }
    // Start the timer
    xTimerStart(ota_timeout_timer, 0);
}

/**
 * Retrieves the current local timestamp and formats it as a string.
 * This is used for logging and storing the OTA update time.
 */
void get_current_timestamp(char *buffer, size_t max_len)
{
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
esp_err_t write_ota_timestamp_to_nvs(const char *timestamp)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Initialize NVS (if not already initialized)
    err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Open the NVS handle for writing
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Write the OTA timestamp to NVS
    err = nvs_set_str(nvs_handle, "ota_timestamp", timestamp);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write timestamp to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit the changes to NVS
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit to NVS: %s", esp_err_to_name(err));
    }

    // Close the NVS handle
    nvs_close(nvs_handle);
    return err;
}

/**
 * The main OTA handler task. This task is responsible for initiating and managing the OTA update process.
 * It handles MQTT message parsing, downloading the firmware, and updating the device.
 */
void ota_handler_task(void *pvParameter)
{
    esp_log_level_set("*", ESP_LOG_INFO);   // Set global logging level to Info
    esp_log_level_set("OTA", ESP_LOG_INFO); // Set logging level for the "OTA" tag

    ESP_LOGI(TAG, "Starting OTA handler task");

    // Extract MQTT event and client from parameter
    esp_mqtt_event_handle_t mqtt_event = (esp_mqtt_event_handle_t)pvParameter;
    esp_mqtt_client_handle_t my_mqtt_client = mqtt_event->client;

    if (mqtt_event->data_len == 0)
    {
        ESP_LOGE(TAG, "Empty payload received! Aborting OTA...");
        vTaskDelete(NULL); // Terminate the task if the payload is empty
    }

    // Copy the payload data from the MQTT event
    char *payload_data = malloc(mqtt_event->data_len + 1);
    strncpy(payload_data, mqtt_event->data, mqtt_event->data_len);
    payload_data[mqtt_event->data_len] = '\0';

    ESP_LOGI(TAG, "Payload data: %s", payload_data);

    // Parse the JSON payload to extract the firmware URL
    cJSON *json = cJSON_Parse(payload_data);
    if (!json)
    {
        ESP_LOGE(TAG, "Failed to parse JSON string");
        vTaskDelete(NULL); // Terminate the task if JSON parsing fails
    }

    free(payload_data);

    // Retrieve the device's MAC address
    char mac_address[18];
    get_burned_in_mac_address(mac_address);
    ESP_LOGI(TAG, "Burned-In MAC Address: %s\n", mac_address);

    // Extract the firmware URL from the JSON object
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

    // Configure the HTTPS client for OTA download
    esp_http_client_config_t config = {
        .url = url_buffer,
        .cert_pem = (char *)AmazonRootCA1_pem, // Use the predefined Amazon Root CA certificate
        .timeout_ms = 30000,                   // Set a 30-second timeout for the HTTPS client
    };

    cJSON_Delete(json);

    ESP_LOGI(TAG, "Using URL: %s", config.url);

    // Prepare the OTA update configuration
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    // Get the OTA update partition
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition)
    {
        ESP_LOGE(TAG, "Failed to find update partition");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Writing to partition: %s", update_partition->label);

    // Ensure the event group is created for synchronization
    if (ota_event_group == NULL)
    {
        ota_event_group = xEventGroupCreate();
        if (ota_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create event group");
            vTaskDelete(NULL);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Event NOT NULL - already created?");
    }

    // Initialize the OTA process
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start OTA: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    else
    {
        ESP_LOGI(TAG, "OTA started successfully");
    }

    // Start the OTA task to handle the actual firmware download
    xTaskCreate(&ota_task, "ota_task", 8192, ota_handle, 10, NULL);

    // Retry logic for the OTA process
    const int max_retries = 3;
    int attempt = 0;
    while (attempt < max_retries)
    {
        // Wait for the OTA completion or failure event
        EventBits_t bits = xEventGroupWaitBits(ota_event_group, OTA_COMPLETE_BIT | OTA_FAILED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & OTA_COMPLETE_BIT)
        {
            ESP_LOGW(TAG, "OTA handler reports firmware copy successful");
            break;
        }
        else if (bits & OTA_FAILED_BIT)
        {
            // Retry in case of OTA failure
            ESP_LOGE(TAG, "OTA attempt %d failed", attempt + 1);
            attempt++;
            if (attempt < max_retries)
            {
                ESP_LOGI(TAG, "Retrying OTA...");
            }
            else
            {
                ESP_LOGE(TAG, "Max OTA attempts reached. OTA FAILED");
                OTA_FAIL_EXIT(); // Exit if the maximum number of retries is reached
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay to avoid busy-waiting
    }

    // Stop the MQTT client and disconnect WiFi
    esp_mqtt_client_stop(my_mqtt_client);
    esp_wifi_disconnect();

    // Schedule a system reboot after OTA completion
    schedule_reboot(10000); // Schedule a reboot with a 10-second delay

    // Delete the OTA handler task
    vTaskDelete(NULL);
}

/**
 * The OTA task performs the actual firmware download and installation.
 * It monitors the OTA progress, handles timeouts, and ensures successful installation.
 */
void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting");

    esp_https_ota_handle_t ota_handle = (esp_https_ota_handle_t)pvParameter;

    // Add the OTA task to the watchdog timer
    esp_task_wdt_add(NULL);

    // Schedule the OTA timeout in case the process hangs
    schedule_ota_timeout(OTA_TIMEOUT_PERIOD);

    while (1)
    {
        // Perform the OTA operation (firmware download)
        esp_err_t err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            // OTA update is in progress, continue
            vTaskDelay(pdMS_TO_TICKS(100)); // Delay to avoid busy-waiting
            // Reset the watchdog timer
            esp_task_wdt_reset();
            continue;
        }
        else if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "perform error: %s", esp_err_to_name(err));
            OTA_FAIL_EXIT(); // Exit on OTA perform error
        }
        else
        {
            ESP_LOGI(TAG, "perform successful");
            break; // OTA operation successful, exit the loop
        }
    }

    // Verify that all data was received and the OTA update is complete
    if (esp_https_ota_is_complete_data_received(ota_handle))
    {
        // Finish the OTA process and apply the update
        esp_err_t ota_finish_err = esp_https_ota_finish(ota_handle);
        if (ota_finish_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Success! Update complete!");

            // Get the current timestamp
            char timestamp[20];
            get_current_timestamp(timestamp, sizeof(timestamp));

            // Write the OTA timestamp to NVS for tracking
            esp_err_t nvs_err = write_ota_timestamp_to_nvs(timestamp);
            if (nvs_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to write OTA timestamp to NVS: %s", esp_err_to_name(nvs_err));
                OTA_FAIL_EXIT();
            }
            ESP_LOGI(TAG, "timestamp written to NVS: %s", timestamp);

            OTA_COMPLETE_EXIT(); // OTA process completed successfully
        }
        else
        {
            ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(ota_finish_err));
            OTA_FAIL_EXIT(); // Exit on OTA finish error
        }
    }
    else
    {
        ESP_LOGE(TAG, "Complete data was not received.");
        OTA_FAIL_EXIT(); // Exit if the complete firmware data was not received
    }

    // Remove the OTA task from the watchdog timer
    ESP_LOGI(TAG, "Removing OTA task from watchdog timer");
    esp_task_wdt_delete(NULL);
}
