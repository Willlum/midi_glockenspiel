#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#define DRV8912_MAX_DEVICES 63
#define DRV8912_CHANNELS_PER_DEVICE 12

typedef struct drv8912_context *drv8912_handle_t;

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t cs_gpio;
    gpio_num_t sleep_gpio;
    gpio_num_t fault_gpio;
    uint8_t device_count;
    int clock_speed_hz;
} drv8912_config_t;

typedef enum {
    DRV8912_REG_IC_STAT = 0x00,
    DRV8912_REG_OP_CTRL_1 = 0x08,
    DRV8912_REG_OP_CTRL_2 = 0x09,
    DRV8912_REG_OP_CTRL_3 = 0x0A,
} drv8912_register_t;

esp_err_t drv8912_init(const drv8912_config_t *config, drv8912_handle_t *out_handle);
esp_err_t drv8912_deinit(drv8912_handle_t handle);

uint8_t drv8912_get_device_count(drv8912_handle_t handle);
esp_err_t drv8912_get_fault_pin(drv8912_handle_t handle, int *level);

esp_err_t drv8912_read_registers(drv8912_handle_t handle,
                                 const uint8_t *addresses,
                                 uint8_t *values,
                                 uint8_t *status);
esp_err_t drv8912_write_registers(drv8912_handle_t handle,
                                  const uint8_t *addresses,
                                  const uint8_t *values);
esp_err_t drv8912_read_register(drv8912_handle_t handle,
                                uint8_t device,
                                uint8_t address,
                                uint8_t *value,
                                uint8_t *status);
esp_err_t drv8912_write_register(drv8912_handle_t handle,
                                 uint8_t device,
                                 uint8_t address,
                                 uint8_t value);
esp_err_t drv8912_clear_faults(drv8912_handle_t handle);

// Channels are numbered 1..12; enabling a side disables the opposite side.
esp_err_t drv8912_set_half_bridge(drv8912_handle_t handle,
                                  uint8_t device,
                                  uint8_t channel,
                                  bool high_side,
                                  bool enabled);