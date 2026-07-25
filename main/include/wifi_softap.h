#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

void wifi_init_manager(void);

// Return provisioning token when in AP provisioning mode, or NULL otherwise
const char* wifi_get_provision_token(void);

// Provision device with SSID/password and token obtained from AP
esp_err_t wifi_provision(const char *ssid, const char *pass, const char *token);

#ifdef __cplusplus
}
#endif
