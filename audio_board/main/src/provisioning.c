#include <provisioning.h>
#include "wifi_provisioning/scheme_softap.h"
#include "wifi_provisioning/manager.h"
#include "main.h"  
#include "wifi.h"
#include "espnow.h"
#include "nvs_flash.h"

// LED status handle from main
// extern led_status_t led_mode;
// extern char *TAG;
extern const int WIFI_CONNECTED_EVENT;
extern EventGroupHandle_t wifi_event_group;
// extern unsigned char trigger_reprovision;
unsigned char trigger_reprovision = 0;
const char *TAG_PROV = "Provisioning";

#include "esp_timer.h"

static esp_timer_handle_t s_reconnect_timer = NULL;

static void reconnect_timer_cb(void *arg) {
    ESP_LOGI(TAG_PROV, "Timer triggered: attempting to reconnect to AP...");
    esp_wifi_connect();
}

static void start_reconnect_timer(void) {
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &reconnect_timer_cb,
            .name = "wifi_reconnect"
        };
        esp_timer_create(&timer_args, &s_reconnect_timer);
    } else {
        esp_timer_stop(s_reconnect_timer);
    }
    esp_timer_start_once(s_reconnect_timer, 30000000); // 30 seconds
}

/* This salt,verifier has been generated for username = "wifiprov" and password = "abcd1234"
 * IMPORTANT NOTE: For production cases, this must be unique to every device
 * and should come from device manufacturing partition.*/
static const char sec2_salt[] = {
    0x03, 0x6e, 0xe0, 0xc7, 0xbc, 0xb9, 0xed, 0xa8, 0x4c, 0x9e, 0xac, 0x97, 0xd9, 0x3d, 0xec, 0xf4
};

static const char sec2_verifier[] = {
    0x7c, 0x7c, 0x85, 0x47, 0x65, 0x08, 0x94, 0x6d, 0xd6, 0x36, 0xaf, 0x37, 0xd7, 0xe8, 0x91, 0x43,
    0x78, 0xcf, 0xfd, 0x61, 0x6c, 0x59, 0xd2, 0xf8, 0x39, 0x08, 0x12, 0x72, 0x38, 0xde, 0x9e, 0x24,
    0xa4, 0x70, 0x26, 0x1c, 0xdf, 0xa9, 0x03, 0xc2, 0xb2, 0x70, 0xe7, 0xb1, 0x32, 0x24, 0xda, 0x11,
    0x1d, 0x97, 0x18, 0xdc, 0x60, 0x72, 0x08, 0xcc, 0x9a, 0xc9, 0x0c, 0x48, 0x27, 0xe2, 0xae, 0x89,
    0xaa, 0x16, 0x25, 0xb8, 0x04, 0xd2, 0x1a, 0x9b, 0x3a, 0x8f, 0x37, 0xf6, 0xe4, 0x3a, 0x71, 0x2e,
    0xe1, 0x27, 0x86, 0x6e, 0xad, 0xce, 0x28, 0xff, 0x54, 0x46, 0x60, 0x1f, 0xb9, 0x96, 0x87, 0xdc,
    0x57, 0x40, 0xa7, 0xd4, 0x6c, 0xc9, 0x77, 0x54, 0xdc, 0x16, 0x82, 0xf0, 0xed, 0x35, 0x6a, 0xc4,
    0x70, 0xad, 0x3d, 0x90, 0xb5, 0x81, 0x94, 0x70, 0xd7, 0xbc, 0x65, 0xb2, 0xd5, 0x18, 0xe0, 0x2e,
    0xc3, 0xa5, 0xf9, 0x68, 0xdd, 0x64, 0x7b, 0xb8, 0xb7, 0x3c, 0x9c, 0xfc, 0x00, 0xd8, 0x71, 0x7e,
    0xb7, 0x9a, 0x7c, 0xb1, 0xb7, 0xc2, 0xc3, 0x18, 0x34, 0x29, 0x32, 0x43, 0x3e, 0x00, 0x99, 0xe9,
    0x82, 0x94, 0xe3, 0xd8, 0x2a, 0xb0, 0x96, 0x29, 0xb7, 0xdf, 0x0e, 0x5f, 0x08, 0x33, 0x40, 0x76,
    0x52, 0x91, 0x32, 0x00, 0x9f, 0x97, 0x2c, 0x89, 0x6c, 0x39, 0x1e, 0xc8, 0x28, 0x05, 0x44, 0x17,
    0x3f, 0x68, 0x02, 0x8a, 0x9f, 0x44, 0x61, 0xd1, 0xf5, 0xa1, 0x7e, 0x5a, 0x70, 0xd2, 0xc7, 0x23,
    0x81, 0xcb, 0x38, 0x68, 0xe4, 0x2c, 0x20, 0xbc, 0x40, 0x57, 0x76, 0x17, 0xbd, 0x08, 0xb8, 0x96,
    0xbc, 0x26, 0xeb, 0x32, 0x46, 0x69, 0x35, 0x05, 0x8c, 0x15, 0x70, 0xd9, 0x1b, 0xe9, 0xbe, 0xcc,
    0xa9, 0x38, 0xa6, 0x67, 0xf0, 0xad, 0x50, 0x13, 0x19, 0x72, 0x64, 0xbf, 0x52, 0xc2, 0x34, 0xe2,
    0x1b, 0x11, 0x79, 0x74, 0x72, 0xbd, 0x34, 0x5b, 0xb1, 0xe2, 0xfd, 0x66, 0x73, 0xfe, 0x71, 0x64,
    0x74, 0xd0, 0x4e, 0xbc, 0x51, 0x24, 0x19, 0x40, 0x87, 0x0e, 0x92, 0x40, 0xe6, 0x21, 0xe7, 0x2d,
    0x4e, 0x37, 0x76, 0x2f, 0x2e, 0xe2, 0x68, 0xc7, 0x89, 0xe8, 0x32, 0x13, 0x42, 0x06, 0x84, 0x84,
    0x53, 0x4a, 0xb3, 0x0c, 0x1b, 0x4c, 0x8d, 0x1c, 0x51, 0x97, 0x19, 0xab, 0xae, 0x77, 0xff, 0xdb,
    0xec, 0xf0, 0x10, 0x95, 0x34, 0x33, 0x6b, 0xcb, 0x3e, 0x84, 0x0f, 0xb9, 0xd8, 0x5f, 0xb8, 0xa0,
    0xb8, 0x55, 0x53, 0x3e, 0x70, 0xf7, 0x18, 0xf5, 0xce, 0x7b, 0x4e, 0xbf, 0x27, 0xce, 0xce, 0xa8,
    0xb3, 0xbe, 0x40, 0xc5, 0xc5, 0x32, 0x29, 0x3e, 0x71, 0x64, 0x9e, 0xde, 0x8c, 0xf6, 0x75, 0xa1,
    0xe6, 0xf6, 0x53, 0xc8, 0x31, 0xa8, 0x78, 0xde, 0x50, 0x40, 0xf7, 0x62, 0xde, 0x36, 0xb2, 0xba
};

wifi_sta_config_t *wifi_sta_cfg;

static esp_err_t example_get_sec2_salt(const char **salt, uint16_t *salt_len) {
    ESP_LOGI(TAG_PROV, "Development mode: using hard coded salt");
    *salt = sec2_salt;
    *salt_len = sizeof(sec2_salt);
    return ESP_OK;
}

static esp_err_t example_get_sec2_verifier(const char **verifier, uint16_t *verifier_len) {
    ESP_LOGI(TAG_PROV, "Development mode: using hard coded verifier");
    *verifier = sec2_verifier;
    *verifier_len = sizeof(sec2_verifier);
    return ESP_OK;
}


/* Event handler for catching system events */
void prov_event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    static int retries;
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG_PROV, "Provisioning started");
                // led_mode = LED_BLINK_FAST;
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                /* SECURITY: never log the Wi-Fi password (plaintext credential
                 * leak over UART). Log the SSID only. */
                ESP_LOGI(TAG_PROV, "Received Wi-Fi credentials for SSID: %s",
                         (const char *) wifi_sta_cfg->ssid);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG_PROV, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                // led_mode = LED_WIFI_FAIL;
                retries++;
                if (retries >= PROV_MGR_MAX_RETRY_CNT) {
                    ESP_LOGI(TAG_PROV, "Failed to connect with provisioned AP, reseting provisioned credentials");
                    wifi_prov_mgr_reset_sm_state_on_failure();
                    retries = 0;
                }
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG_PROV, "Provisioning successful");
                set_provisioning_flag(); 
                retries = 0;
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG_PROV, "Disconnected. Scheduling reconnect attempt...");
                // led_mode = LED_ERROR;
                /* Router may be down: re-pin the radio to the fixed ESP-NOW
                 * channel so scan announcements keep reaching the gateway even
                 * while the STA wanders channels trying to reconnect. */
                espnow_repin_channel();
                if(trigger_reprovision==1) {
                    ESP_LOGI(TAG_PROV,"RESETTING PROVISIONING");
                    wifi_prov_mgr_reset_provisioning();
                    esp_restart();
                }
                start_reconnect_timer();
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG_PROV, "SoftAP transport: Connected!");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG_PROV, "SoftAP transport: Disconnected!");
                // ESP_LOGW(TAG, "Restarting in 5 seconds...");
                // vTaskDelay(pdMS_TO_TICKS(5000));

                // esp_restart();
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
        }
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

        ESP_LOGI(TAG_PROV, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        // led_mode = LED_ON;
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (event_id) {
            case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
                ESP_LOGI(TAG_PROV, "Secured session established!");
                break;
            case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
                ESP_LOGE(TAG_PROV, "Received invalid security parameters for establishing secure session!");
                break;
            case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
                ESP_LOGE(TAG_PROV, "Received incorrect username and/or PoP for establishing secure session!");
                break;
            default:
                break;
        }
    }
}

void set_provisioning_flag(void) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "new_prov", 1); // Set flag: Just provisioned!
        nvs_commit(handle);
        nvs_close(handle);
    }
}


void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    
    // Format device type in uppercase
    char dev_type_caps[32];
    strncpy(dev_type_caps, DEVICE_TYPE, sizeof(dev_type_caps) - 1);
    dev_type_caps[sizeof(dev_type_caps) - 1] = '\0';
    for (int i = 0; dev_type_caps[i]; i++) {
        if (dev_type_caps[i] >= 'a' && dev_type_caps[i] <= 'z') {
            dev_type_caps[i] -= 32;
        }
    }

    snprintf(service_name, max, "ZT_%s_%02X%02X%02X%02X%02X%02X", dev_type_caps,
             eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
}


/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf) {
        ESP_LOGI(TAG_PROV, "Received data: %.*s", inlen, (char *)inbuf);
    }
    char response[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL) {
        ESP_LOGE(TAG_PROV, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

    return ESP_OK;
}

void wifi_prov_print_qr(const char *name, const char *username, const char *pop, const char *transport)
{
    if (!name || !transport) {
        ESP_LOGW(TAG_PROV, "Cannot generate QR code payload. Data missing.");
        return;
    }
    char payload[150] = {0};
    if (pop) {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"username\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, username, pop, transport);
    } else {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, transport);
    }
// #ifdef CONFIG_EXAMPLE_PROV_SHOW_QR
    // ESP_LOGI(TAG_PROV, "Scan this QR code from the provisioning application for Provisioning.");
    // esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    // esp_qrcode_generate(&cfg, payload);
// #endif /* CONFIG_APP_WIFI_PROV_SHOW_QR */
    ESP_LOGI(TAG_PROV, "If QR code is not visible, copy paste the below URL in a browser.\n%s?data=%s", QRCODE_BASE_URL, payload);
}

bool provisioning_init(void)
{
    static bool provisioned = false;
    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
        // .scheme_event_handler = wifi_prov_scheme_softap_event_handler

    };
    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    return provisioned;
}

void do_provisioning(void)
{
    ESP_LOGI(TAG_PROV, "Starting provisioning");

    /* What is the Device Service Name that we want
        * This translates to :
        *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
        *     - device name when scheme is wifi_prov_scheme_ble
        */
    char service_name[32];
    get_device_service_name(service_name, sizeof(service_name));

    wifi_prov_security_t security = WIFI_PROV_SECURITY_2;
    /* The username must be the same one, which has been used in the generation of salt and verifier */

    /* This pop field represents the password that will be used to generate salt and verifier.
        * The field is present here in order to generate the QR code containing password.
        * In production this password field shall not be stored on the device */
    const char *username  = EXAMPLE_PROV_SEC2_USERNAME;
    const char *pop = EXAMPLE_PROV_SEC2_PWD;
    /* This is the structure for passing security parameters
        * for the protocomm security 2.
        * If dynamically allocated, sec2_params pointer and its content
        * must be valid till WIFI_PROV_END event is triggered.
        */
    wifi_prov_security2_params_t sec2_params = {};

    ESP_ERROR_CHECK(example_get_sec2_salt(&sec2_params.salt, &sec2_params.salt_len));
    ESP_ERROR_CHECK(example_get_sec2_verifier(&sec2_params.verifier, &sec2_params.verifier_len));

    wifi_prov_security2_params_t *sec_params = &sec2_params;

    /* What is the service key (could be NULL)
        * This translates to :
        *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
        *          (Minimum expected length: 8, maximum 64 for WPA2-PSK)
        *     - simply ignored when scheme is wifi_prov_scheme_ble
        */
    const char *service_key = NULL;
    /* An optional endpoint that applications can create if they expect to
        * get some additional custom data during provisioning workflow.
        * The endpoint name can be anything of your choice.
        * This call must be made before starting the provisioning.
        */
    wifi_prov_mgr_endpoint_create("custom-data");

    /* Start provisioning service */
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, (const void *) sec_params, service_name, service_key));

    /* The handler for the optional endpoint created above.
        * This call must be made after starting the provisioning, and only if the endpoint
        * has already been created above.
        */
    wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

    /* Uncomment the following to wait for the provisioning to finish and then release
        * the resources of the manager. Since in this case de-initialization is triggered
        * by the default event loop handler, we don't need to call the following */
    // wifi_prov_mgr_wait();
    // wifi_prov_mgr_deinit();
    /* Print QR code for provisioning */
    wifi_prov_print_qr(service_name, username, pop, PROV_TRANSPORT_SOFTAP);
}
