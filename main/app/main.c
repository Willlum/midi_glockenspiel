#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "drv8912";

#define PIN_MOSI   23   // U5 IO23 -> U2 SDI  (chain input)
#define PIN_MISO   19   // U3 SDO  -> U5 IO19  (chain output)
#define PIN_SCLK   18
#define PIN_CS     5
#define PIN_NSLEEP 22   // drive HIGH to wake
#define PIN_NFAULT 21   // active-low, pulled up by R1

#define LED_IO_0   32   // D2 (LED1)
#define LED_IO_1   33   // D1 (LED2)

#define DRV_HOST   SPI3_HOST        // VSPI default pins = 18/19/23/5
#define N_DRV      3
#define FRAME_LEN  (2 + 2*N_DRV)    // 2 header + N addr + N data = 8 bytes

// Chain positions (MOSI -> U2 -> U1 -> U3 -> MISO):
//   pos 0 = U2 (first), pos 1 = U1, pos 2 = U3 (last)
#define REG_IC_STAT 0x00

static spi_device_handle_t drv;

// One daisy-chain frame 
static esp_err_t drv_xfer( const uint8_t addr[N_DRV], const uint8_t data[N_DRV], bool is_read, 
    bool clr_fault, uint8_t status[N_DRV], uint8_t report[N_DRV], uint8_t rx_raw[FRAME_LEN]) {

        uint8_t tx[FRAME_LEN] = {0};
        uint8_t rx[FRAME_LEN] = {0};

    tx[0] = 0x80 | (N_DRV & 0x3F);          // HEADER 1: 1 0 N5..N0
    tx[1] = 0x80 | (clr_fault ? 0x20 : 0);  // HEADER 2: 1 0 CLR x x x x x

    // Sent order is A3,A2,A1 then D3,D2,D1
    for (int pos = 0; pos < N_DRV; pos++) {
        uint8_t a = ((is_read ? 1 : 0) << 6) | (addr[pos] & 0x3F);
        tx[2 + (N_DRV - 1 - pos)] = a;         
        tx[2 + N_DRV + (N_DRV - 1 - pos)] = data[pos]; 
    }

    spi_transaction_t t = { .length = FRAME_LEN * 8, .tx_buffer = tx, .rx_buffer = rx };
    esp_err_t err = spi_device_transmit(drv, &t);
    if (err != ESP_OK) return err;
    if (rx_raw) memcpy(rx_raw, rx, FRAME_LEN);

    // RX order: S3 S2 S1 HDR1 HDR2 R3 R2 R1
    for (int pos = 0; pos < N_DRV; pos++) {
        status[pos] = rx[N_DRV - 1 - pos];
        report[pos] = rx[2 + N_DRV + (N_DRV - 1 - pos)];
    }
    return ESP_OK;
}

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
    gpio_config_t in  = { 
        .pin_bit_mask = 1ULL << PIN_NFAULT, 
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE 
    };
    gpio_config(&in);
    
    gpio_config_t out = { 
        .pin_bit_mask = 1ULL << PIN_NSLEEP,
        .mode = GPIO_MODE_OUTPUT 
    };
    gpio_config(&out);
    gpio_set_level(PIN_NSLEEP, 1);          // wake the DRVs
    vTaskDelay(pdMS_TO_TICKS(5));           // tREADY <= 1ms; 5ms is safe

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DRV_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 5000000,          // max 5 MHz; drop to 1 MHz while debugging joints
        .mode = 1,                          // CPOL=0, CPHA=1 : SDI sampled on falling edge
        .spics_io_num = PIN_CS,
        .queue_size = 1,
        .cs_ena_pretrans = 2, .cs_ena_posttrans = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(DRV_HOST, &dev, &drv));

    ESP_LOGI(TAG, "nFAULT = %d (1 = not asserted)", gpio_get_level(PIN_NFAULT));

    uint8_t addr[N_DRV]   = { REG_IC_STAT, REG_IC_STAT, REG_IC_STAT };
    uint8_t data[N_DRV]   = { 0, 0, 0 };
    uint8_t status[N_DRV] = {0};
    uint8_t report[N_DRV] = {0};
    uint8_t raw[FRAME_LEN] = {0};
    const char *names[N_DRV] = { "U2(pos1)", "U1(pos2)", "U3(pos3)" };

    ESP_ERROR_CHECK(drv_xfer(addr, data, true, false, status, report, raw));

    ESP_LOGI(TAG, "raw rx: %02X %02X %02X %02X %02X %02X %02X %02X",
             raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7]);

    bool hdr_ok = (raw[3] == (0x80 | N_DRV)) && (raw[4] == 0x80);
    ESP_LOGI(TAG, "header echo %s (got %02X %02X, want %02X %02X)",
             hdr_ok ? "OK" : "MISMATCH", raw[3], raw[4], 0x80|N_DRV, 0x80);

    for (int i = 0; i < N_DRV; i++) {
        bool ok = (status[i] & 0xC0) == 0xC0;   // top two bits hardwired to 1,1
        ESP_LOGI(TAG, "%s status=0x%02X %s [OTW/SD=%d OLD=%d OCP=%d UVLO=%d OVP=%d NPOR=%d] report=0x%02X",
                 names[i], status[i], ok ? "PRESENT" : "NO RESPONSE",
                 (status[i]>>5)&1,(status[i]>>4)&1,(status[i]>>3)&1,
                 (status[i]>>2)&1,(status[i]>>1)&1,(status[i]>>0)&1, report[i]);
    }

    // Clear the power-on latches (NPOR/UVLO), then re-read to show status change.
    ESP_ERROR_CHECK(drv_xfer(addr, data, true, true,  status, report, raw)); // CLR=1
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_ERROR_CHECK(drv_xfer(addr, data, true, false, status, report, raw));
    ESP_LOGI(TAG, "after CLR_FLT: U2=0x%02X U1=0x%02X U3=0x%02X (NPOR->1 if VM present)",
             status[0], status[1], status[2]);

    // Start the alternating LED blink as its own task; app_main can return.
    xTaskCreate(blink_task, "blink", 2048, NULL, 5, NULL);
}