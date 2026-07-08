#ifndef PROVISIONING_H__
#define PROVISIONING_H__

#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#define EXAMPLE_PROV_SEC2_USERNAME          "wifiprov"
#define EXAMPLE_PROV_SEC2_PWD               "abcd1234"
#define PROV_MGR_MAX_RETRY_CNT              (5)
#define PROV_QR_VERSION                     "v1"
#define PROV_TRANSPORT_SOFTAP               "softap"
#define QRCODE_BASE_URL                     "https://espressif.github.io/esp-jumpstart/qrcode.html"

// unsigned char trigger_reprovision = 0;
// char *TAG = "Provisioning";  // or whatever tag you use

extern unsigned char trigger_reprovision;  // declaration only



void prov_event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data);
void set_provisioning_flag(void);
void get_device_service_name(char *service_name, size_t max);
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen, void *priv_data);
void wifi_prov_print_qr(const char *name, const char *username, const char *pop, const char *transport);
bool provisioning_init(void);
void do_provisioning(void);

#endif /*PROVISIONING_H__*/