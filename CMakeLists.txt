cmake_minimum_required(VERSION 3.5)

idf_component_register(
    SRCS 
        "gecl-ota-manager.c" 
        "AmazonRootCA1_pem.c"
    INCLUDE_DIRS 
        "include" 
    PRIV_REQUIRES 
        main
        esp_event 
        esp_system
        esp_http_client 
        esp_https_ota 
        esp_partition 
        nvs_flash 
        esp_netif 
        esp_wifi 
        app_update 
        mqtt 
        json 
        gecl-mqtt-manager
        gecl-time-sync-manager
        lwip
    )