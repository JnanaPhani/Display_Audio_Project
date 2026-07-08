#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <sys/time.h>

// Define a local TAG for this file for logging
static const char *TAG_WIFI = "WIFI_MANAGER";

// These are now internal to this file
const int WIFI_CONNECTED_EVENT = BIT0;
EventGroupHandle_t wifi_event_group;

void wifi_start(void)
{
    bool provision_status = false;
    /* NVS is already initialized in app_main(), no need to re-init here */
    wifi_init();

    provision_status = provisioning_init();

    /* If device is not yet provisioned start provisioning service */
    if (!provision_status) {
        do_provisioning();
    } else {
        ESP_LOGI(TAG_WIFI, "Already provisioned, starting Wi-Fi STA");
        /* We don't need the manager as device is already provisioned,
         * so let's release its resources */
        wifi_prov_mgr_deinit();
        /* Start Wi-Fi station */
        wifi_init_sta();
    }
}

void wifi_wait_for_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    while (netif == NULL || !esp_netif_is_netif_up(netif)) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    ESP_LOGI(TAG_WIFI, "Waiting for IP address from DHCP...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);
    ESP_LOGI(TAG_WIFI, "IP address acquired! Proceeding with application setup.");
}

void connect_wifi(void)
{
    wifi_start();
    wifi_wait_for_ip();
}


void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_init(void)
{
    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &prov_event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}