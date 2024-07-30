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

void ota_task(void *pvParameter);
void ota_handler_task(void *pvParameter);
bool was_booted_after_ota_update(void);
#endif  // OTA_UPDATE_H
