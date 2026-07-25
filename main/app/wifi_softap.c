#include "wifi_softap.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG3 = "WIFI_MANAGER";

static char prov_token[16] = {0};
static bool in_provisioning = false;

// NVS namespace/keys
#define NVS_NAMESPACE "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

static esp_netif_t *sta_netif = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG3, "Disconnected, retrying...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG3, "Got IP: %s", ip4addr_ntoa(&event->ip_info.ip));
        }
    }
}

const char* wifi_get_provision_token(void) {
    return in_provisioning ? prov_token : NULL;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static bool load_wifi_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    if (nvs_get_str(h, NVS_KEY_SSID, ssid_out, &ssid_len) != ESP_OK) { nvs_close(h); return false; }
    if (nvs_get_str(h, NVS_KEY_PASS, pass_out, &pass_len) != ESP_OK) { nvs_close(h); return false; }
    nvs_close(h);
    return true;
}

void wifi_init_manager(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();

    // Try to read stored creds
    char ssid[64] = {0};
    char pass[128] = {0};

    if (load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG3, "Found stored Wi-Fi credentials, attempting STA connection to '%s'", ssid);

        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

        wifi_config_t wifi_cfg = {0};
        strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid)-1);
        strncpy((char*)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password)-1);

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();

        // Wait briefly for connection (event handler will log IP if connected)
        vTaskDelay(pdMS_TO_TICKS(5000));

        // If we have IP, event handler printed it; check ip info
        esp_netif_ip_info_t ip_info;
        sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                ESP_LOGI(TAG3, "Connected with IP: %s", ip4addr_ntoa(&ip_info.ip));
                in_provisioning = false;
                return;
            }
        }

        ESP_LOGW(TAG3, "STA connection failed or no IP; falling back to provisioning AP");
    } else {
        ESP_LOGI(TAG3, "No stored Wi-Fi credentials, starting provisioning AP");
    }

    // Start provisioning AP
    // generate a short token as AP password
    uint32_t r = esp_random();
    uint32_t pin = 100000 + (r % 900000);
    snprintf(prov_token, sizeof(prov_token), "%06u", pin);
    in_provisioning = true;

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {0};
    snprintf((char*)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "Glockenspiel_PROV");
    wifi_config.ap.ssid_len = strlen("Glockenspiel_PROV");
    snprintf((char*)wifi_config.ap.password, sizeof(wifi_config.ap.password), "%s", prov_token);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG3, "Provisioning AP started. SSID=Glockenspiel_PROV password=%s", prov_token);
    ESP_LOGI(TAG3, "Connect to the AP and POST credentials to /api/provision?ssid=...&pass=...&token=...");
}

esp_err_t wifi_provision(const char *ssid, const char *pass, const char *token)
{
    if (!in_provisioning) return ESP_ERR_INVALID_STATE;
    if (!ssid || !token) return ESP_ERR_INVALID_ARG;
    if (strcmp(token, prov_token) != 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG3, "Provisioning with SSID='%s'", ssid);

    if (save_wifi_credentials(ssid, pass) != ESP_OK) {
        ESP_LOGE(TAG3, "Failed to save credentials to NVS");
        return ESP_FAIL;
    }

    // Stop AP and start STA using saved credentials
    esp_wifi_stop();

    // start STA
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_cfg = {0};
    strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid)-1);
    if (pass) strncpy((char*)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password)-1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    in_provisioning = false;
    memset(prov_token, 0, sizeof(prov_token));

    ESP_LOGI(TAG3, "Provisioning complete, attempting STA connect");
    return ESP_OK;
}

static const char *TAG3 = "WIFI_AP";

void wifi_init_softap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {0};
    snprintf((char*)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "Glockenspiel_AP");
    wifi_config.ap.ssid_len = strlen("Glockenspiel_AP");
    snprintf((char*)wifi_config.ap.password, sizeof(wifi_config.ap.password), "midi_pass");
    wifi_config.ap.max_connection = 1;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    esp_log_level_set("esp_netif", ESP_LOG_INFO);
    ESP_LOGI(TAG3, "SoftAP started. SSID=%s password=%s", "Glockenspiel_AP", "midi_pass");
}
