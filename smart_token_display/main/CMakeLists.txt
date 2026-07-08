idf_component_register(
    SRCS "src/main.c" "src/wifi.c" "src/supabase.c"
         "src/provisioning.c" "src/token_display.c" "src/ota.c"
         "src/espnow.c" "src/ota_selftest.c" "src/crash_report.c"
    INCLUDE_DIRS "inc"
    REQUIRES nvs_flash esp_wifi esp_http_client esp_websocket_client
             esp_https_ota json esp-tls app_update esp_netif esp_event
             esp_timer spi_flash wifi_provisioning usb usb_host_hid
             barcode_scanner p10display espcoredump)
