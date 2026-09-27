#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "drv8912.h"
#include "midi_io.h"

static const char *TAG = "drv8912";

#define PIN_MOSI   23
#define PIN_MISO   19
#define PIN_SCLK   18
#define PIN_CS     5
#define PIN_NSLEEP 22
#define PIN_NFAULT 21

#define LED_IO_0   32   // D2 (LED1)
#define LED_IO_1   33   // D1 (LED2)
#define N_DRV      3

static void blink_task(void *arg)
{
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_IO_0) | (1ULL << LED_IO_1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_config));

    bool led_state = false;
    while (1) {
        led_state = !led_state;
        gpio_set_level(LED_IO_0, led_state);
        gpio_set_level(LED_IO_1, !led_state);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void drv_output_test_task(void *arg)
{
    drv8912_handle_t drv = (drv8912_handle_t)arg;
    const uint8_t device_count = drv8912_get_device_count(drv);

    for (uint8_t device = 0; device < device_count; device++) {
        for (uint8_t channel = 1; channel <= DRV8912_CHANNELS_PER_DEVICE; channel++) {
            ESP_LOGI(TAG, "Device %u, channel %u high side ON", device, channel);
            ESP_ERROR_CHECK(drv8912_set_half_bridge(drv, device, channel, true, true));
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_ERROR_CHECK(drv8912_set_half_bridge(drv, device, channel, true, false));
        }
    }

    ESP_LOGI(TAG, "High-side output test complete");
    vTaskDelete(NULL);
}

void app_main(void)
{
    const drv8912_config_t config = {
        .spi_host = SPI3_HOST,
        .mosi_gpio = PIN_MOSI,
        .miso_gpio = PIN_MISO,
        .sclk_gpio = PIN_SCLK,
        .cs_gpio = PIN_CS,
        .sleep_gpio = PIN_NSLEEP,
        .fault_gpio = PIN_NFAULT,
        .device_count = N_DRV,
        .clock_speed_hz = 5000000,
    };
    drv8912_handle_t drv;
    ESP_ERROR_CHECK(drv8912_init(&config, &drv));
    
    mount_sd();
    list_sd_contents(MOUNT_POINT);
    
    int fault_level;
    ESP_ERROR_CHECK(drv8912_get_fault_pin(drv, &fault_level));
    ESP_LOGI(TAG, "nFAULT = %d (1 = not asserted)", fault_level);

    uint8_t addr[N_DRV]   = { DRV8912_REG_IC_STAT, DRV8912_REG_IC_STAT, DRV8912_REG_IC_STAT };
    uint8_t status[N_DRV] = {0};
    uint8_t report[N_DRV] = {0};
    const char *names[N_DRV] = { "U2(pos1)", "U1(pos2)", "U3(pos3)" };

    esp_err_t read_err = drv8912_read_registers(drv, addr, report, status);
    if (read_err != ESP_OK) {
        ESP_LOGE(TAG, "Initial DRV status read failed: %s", esp_err_to_name(read_err));
    }

    for (int i = 0; i < N_DRV; i++) {
        bool ok = (status[i] & 0xC0) == 0xC0;   // top two bits hardwired to 1,1
        ESP_LOGI(TAG, "%s status=0x%02X %s [OTW/SD=%d OLD=%d OCP=%d UVLO=%d OVP=%d NPOR=%d] report=0x%02X",
                 names[i], status[i], ok ? "PRESENT" : "NO RESPONSE",
                 (status[i]>>5)&1,(status[i]>>4)&1,(status[i]>>3)&1,
                 (status[i]>>2)&1,(status[i]>>1)&1,(status[i]>>0)&1, report[i]);
    }

    bool chain_verified = read_err == ESP_OK;
    for (int i = 0; i < N_DRV; i++) {
        if ((status[i] & 0xC0) != 0xC0) {
            chain_verified = false;
            ESP_LOGE(TAG, "%s failed DRV status verification (0x%02X)", names[i], status[i]);
        }
    }

    esp_err_t clear_err = drv8912_clear_faults(drv);
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "DRV fault clear failed: %s", esp_err_to_name(clear_err));
        chain_verified = false;
    } else {
        ESP_LOGI(TAG, "DRV fault latches cleared");
    }

    BaseType_t blink_task_created = xTaskCreate(blink_task, "blink", 2048, NULL, 5, NULL);
    ESP_ERROR_CHECK(blink_task_created == pdPASS ? ESP_OK : ESP_FAIL);

    if (chain_verified) {
        BaseType_t test_task_created = xTaskCreate(drv_output_test_task, "drv_test", 3072,
                                                   drv, 5, NULL);
        ESP_ERROR_CHECK(test_task_created == pdPASS ? ESP_OK : ESP_FAIL);
    } else {
        ESP_LOGE(TAG, "Skipping high-side scan because the daisy chain did not verify");
    }


}