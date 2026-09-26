#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "drv8912.h"

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

// Alternating blink of LED_IO_0 and LED_IO_1, in its own task.
static void blink_task(void *arg)
{
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_IO_0) | (1ULL << LED_IO_1),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_config);

    bool led_state = false;
    while (1) {
        led_state = !led_state;
        gpio_set_level(LED_IO_0, led_state);
        gpio_set_level(LED_IO_1, !led_state);   // opposite phase
        vTaskDelay(pdMS_TO_TICKS(500));
    }
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

    int fault_level;
    ESP_ERROR_CHECK(drv8912_get_fault_pin(drv, &fault_level));
    ESP_LOGI(TAG, "nFAULT = %d (1 = not asserted)", fault_level);

    uint8_t addr[N_DRV]   = { DRV8912_REG_IC_STAT, DRV8912_REG_IC_STAT, DRV8912_REG_IC_STAT };
    uint8_t status[N_DRV] = {0};
    uint8_t report[N_DRV] = {0};
    const char *names[N_DRV] = { "U2(pos1)", "U1(pos2)", "U3(pos3)" };

    ESP_ERROR_CHECK(drv8912_read_registers(drv, addr, report, status));

    for (int i = 0; i < N_DRV; i++) {
        bool ok = (status[i] & 0xC0) == 0xC0;   // top two bits hardwired to 1,1
        ESP_LOGI(TAG, "%s status=0x%02X %s [OTW/SD=%d OLD=%d OCP=%d UVLO=%d OVP=%d NPOR=%d] report=0x%02X",
                 names[i], status[i], ok ? "PRESENT" : "NO RESPONSE",
                 (status[i]>>5)&1,(status[i]>>4)&1,(status[i]>>3)&1,
                 (status[i]>>2)&1,(status[i]>>1)&1,(status[i]>>0)&1, report[i]);
    }

    ESP_ERROR_CHECK(drv8912_clear_faults(drv));
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_ERROR_CHECK(drv8912_read_registers(drv, addr, report, status));
    ESP_LOGI(TAG, "after CLR_FLT: U2=0x%02X U1=0x%02X U3=0x%02X (NPOR->1 if VM present)",
             status[0], status[1], status[2]);

    // Start the alternating LED blink as its own task; app_main can return.
    xTaskCreate(blink_task, "blink", 2048, NULL, 5, NULL);
}