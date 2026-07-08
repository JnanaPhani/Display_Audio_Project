#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "provisioning.h"

#define WIFI_CONNECTED_EVENT_TIMEOUT (3000/portTICK_PERIOD_MS)
extern const int WIFI_CONNECTED_EVENT;
extern EventGroupHandle_t wifi_event_group;

void wifi_start(void);
void wifi_wait_for_ip(void);
void connect_wifi(void);
void wifi_init_sta(void);
void wifi_init(void);