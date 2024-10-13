#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdio.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

typedef struct
{
    esp_mqtt_client_handle_t mqtt_client; // MQTT client handle
    char url[512];                        // URL string (512 bytes)
} ota_config_t;

void ota_task(void *pvParameter);
#endif // OTA_UPDATE_H
