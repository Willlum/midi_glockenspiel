#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "drv8912.h"
#include "esp_log.h"

#define DRV8912_HEADER_BYTES 2
#define DRV8912_FRAME_SIZE(count) (DRV8912_HEADER_BYTES + 2 * (count))
#define DRV8912_READ_BIT 0x40
#define DRV8912_CLEAR_FAULT_BIT 0x20

static const char *TAG = "drv8912";

struct drv8912_context {
    spi_device_handle_t spi_device;
    spi_host_device_t spi_host;
    gpio_num_t sleep_gpio;
    gpio_num_t fault_gpio;
    uint8_t device_count;
    SemaphoreHandle_t mutex;
};

static esp_err_t drv8912_transfer(drv8912_handle_t handle,
                                  const uint8_t *addresses,
                                  const uint8_t *values,
                                  bool is_read,
                                  bool clear_faults,
                                  uint8_t *read_values,
                                  uint8_t *status)
{
    uint8_t tx[DRV8912_FRAME_SIZE(DRV8912_MAX_DEVICES)] = {0};
    uint8_t rx[DRV8912_FRAME_SIZE(DRV8912_MAX_DEVICES)] = {0};
    const size_t frame_size = DRV8912_FRAME_SIZE(handle->device_count);

    tx[0] = 0x80 | handle->device_count;
    tx[1] = 0x80 | (clear_faults ? DRV8912_CLEAR_FAULT_BIT : 0);

    for (uint8_t device = 0; device < handle->device_count; device++) {
        const size_t chain_index = handle->device_count - 1 - device;
        tx[DRV8912_HEADER_BYTES + chain_index] =
            (is_read ? DRV8912_READ_BIT : 0) | (addresses[device] & 0x3F);
        tx[DRV8912_HEADER_BYTES + handle->device_count + chain_index] = values[device];
    }

    spi_transaction_t transaction = {
        .length = frame_size * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(handle->spi_device, &transaction);
    if (err != ESP_OK) {
        return err;
    }

    const bool header_matches = rx[handle->device_count] == tx[0] &&
                                rx[handle->device_count + 1] == tx[1];

    for (uint8_t device = 0; device < handle->device_count; device++) {
        const size_t chain_index = handle->device_count - 1 - device;
        if (status != NULL) {
            status[device] = rx[chain_index];
        }
        if (read_values != NULL) {
            read_values[device] = rx[DRV8912_HEADER_BYTES + handle->device_count + chain_index];
        }
    }

    if (!header_matches) {
        ESP_LOGE(TAG, "SPI header echo mismatch: got %02X %02X, expected %02X %02X",
                 rx[handle->device_count], rx[handle->device_count + 1], tx[0], tx[1]);
        ESP_LOGE(TAG, "SPI RX frame:");
        for (size_t i = 0; i < frame_size; i++) {
            ESP_LOGI(TAG, "rx[%u] = 0x%02X", (unsigned)i, rx[i]);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t drv8912_validate_arrays(drv8912_handle_t handle,
                                         const uint8_t *addresses,
                                         const uint8_t *values)
{
    if (handle == NULL || addresses == NULL || values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t device = 0; device < handle->device_count; device++) {
        if (addresses[device] > 0x3F) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t drv8912_init(const drv8912_config_t *config, drv8912_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->device_count == 0 ||
        config->device_count > DRV8912_MAX_DEVICES || config->clock_speed_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    drv8912_handle_t handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->spi_host = config->spi_host;
    handle->sleep_gpio = config->sleep_gpio;
    handle->fault_gpio = config->fault_gpio;
    handle->device_count = config->device_count;

    esp_err_t err;
    if (handle->sleep_gpio != GPIO_NUM_NC) {
        gpio_config_t sleep_config = {
            .pin_bit_mask = 1ULL << handle->sleep_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&sleep_config);
        if (err != ESP_OK) {
            free(handle);
            return err;
        }
        gpio_set_level(handle->sleep_gpio, 0);
    }

    if (handle->fault_gpio != GPIO_NUM_NC) {
        gpio_config_t fault_config = {
            .pin_bit_mask = 1ULL << handle->fault_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&fault_config);
        if (err != ESP_OK) {
            free(handle);
            return err;
        }
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DRV8912_FRAME_SIZE(config->device_count),
    };
    err = spi_bus_initialize(config->spi_host, &bus_config, 0);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->clock_speed_hz,
        .mode = 1,
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
    };
    err = spi_bus_add_device(config->spi_host, &device_config, &handle->spi_device);
    if (err != ESP_OK) {
        spi_bus_free(config->spi_host);
        free(handle);
        return err;
    }

    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL) {
        spi_bus_remove_device(handle->spi_device);
        spi_bus_free(config->spi_host);
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    if (handle->sleep_gpio != GPIO_NUM_NC) {
        gpio_set_level(handle->sleep_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t config_addresses[DRV8912_MAX_DEVICES];
    uint8_t config_values[DRV8912_MAX_DEVICES] = {0};
    for (uint8_t device = 0; device < handle->device_count; device++) {
        config_addresses[device] = DRV8912_REG_CONFIG_CTRL;
        config_values[device] = 1U << 1; // EXT_OVP: 33 V overvoltage threshold
    }
    err = drv8912_write_registers(handle, config_addresses, config_values);
    if (err != ESP_OK) {
        vSemaphoreDelete(handle->mutex);
        spi_bus_remove_device(handle->spi_device);
        spi_bus_free(config->spi_host);
        free(handle);
        return err;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t drv8912_deinit(drv8912_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->sleep_gpio != GPIO_NUM_NC) {
        gpio_set_level(handle->sleep_gpio, 0);
    }
    vSemaphoreDelete(handle->mutex);
    esp_err_t err = spi_bus_remove_device(handle->spi_device);
    if (err != ESP_OK) {
        return err;
    }
    err = spi_bus_free(handle->spi_host);
    if (err != ESP_OK) {
        return err;
    }
    free(handle);
    return ESP_OK;
}

uint8_t drv8912_get_device_count(drv8912_handle_t handle)
{
    return handle == NULL ? 0 : handle->device_count;
}

esp_err_t drv8912_get_fault_pin(drv8912_handle_t handle, int *level)
{
    if (handle == NULL || level == NULL || handle->fault_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }
    *level = gpio_get_level(handle->fault_gpio);
    return ESP_OK;
}

esp_err_t drv8912_read_registers(drv8912_handle_t handle,
                                 const uint8_t *addresses,
                                 uint8_t *values,
                                 uint8_t *status)
{
    if (values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = drv8912_validate_arrays(handle, addresses, values);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t tx_values[DRV8912_MAX_DEVICES] = {0};
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    err = drv8912_transfer(handle, addresses, tx_values, true, false, values, status);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t drv8912_write_registers(drv8912_handle_t handle,
                                  const uint8_t *addresses,
                                  const uint8_t *values)
{
    esp_err_t err = drv8912_validate_arrays(handle, addresses, values);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    err = drv8912_transfer(handle, addresses, values, false, false, NULL, NULL);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t drv8912_read_register(drv8912_handle_t handle,
                                uint8_t device,
                                uint8_t address,
                                uint8_t *value,
                                uint8_t *status)
{
    if (handle == NULL || value == NULL || device >= handle->device_count || address > 0x3F) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addresses[DRV8912_MAX_DEVICES];
    uint8_t values[DRV8912_MAX_DEVICES] = {0};
    uint8_t statuses[DRV8912_MAX_DEVICES];
    for (uint8_t i = 0; i < handle->device_count; i++) {
        addresses[i] = DRV8912_REG_IC_STAT;
    }
    addresses[device] = address;

    esp_err_t err = drv8912_read_registers(handle, addresses, values, statuses);
    if (err == ESP_OK) {
        *value = values[device];
        if (status != NULL) {
            *status = statuses[device];
        }
    }
    return err;
}

esp_err_t drv8912_write_register(drv8912_handle_t handle,
                                 uint8_t device,
                                 uint8_t address,
                                 uint8_t value)
{
    if (handle == NULL || device >= handle->device_count || address > 0x3F) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addresses[DRV8912_MAX_DEVICES];
    uint8_t values[DRV8912_MAX_DEVICES] = {0};
    for (uint8_t i = 0; i < handle->device_count; i++) {
        addresses[i] = DRV8912_REG_IC_STAT;
    }
    addresses[device] = address;
    values[device] = value;
    return drv8912_write_registers(handle, addresses, values);
}

esp_err_t drv8912_clear_faults(drv8912_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addresses[DRV8912_MAX_DEVICES];
    uint8_t values[DRV8912_MAX_DEVICES] = {0};
    for (uint8_t i = 0; i < handle->device_count; i++) {
        addresses[i] = DRV8912_REG_IC_STAT;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    esp_err_t err = drv8912_transfer(handle, addresses, values, true, true, NULL, NULL);
    xSemaphoreGive(handle->mutex);
    return err;
}

// Enable or disable one half-bridge (high-side or low-side) on one device in the chain.
esp_err_t drv8912_set_half_bridge(drv8912_handle_t handle,
                                  uint8_t device,
                                  uint8_t channel,      // 1..12 (HB1..HB12)
                                  bool high_side,
                                  bool enabled)
{
    if (handle == NULL || device >= handle->device_count || channel == 0 ||
        channel > DRV8912_CHANNELS_PER_DEVICE) {
        return ESP_ERR_INVALID_ARG;
    }

    // --- Locate the register and bit for this half-bridge ---
    // Table 17: the 12 half-bridges are split across three OP_CTRL registers
    //   OP_CTRL_1 (0x08) -> HB1..HB4,  
    //   OP_CTRL_2 (0x09) -> HB5..HB8,
    //   OP_CTRL_3 (0x0A) -> HB9..HB12.
    const uint8_t address = DRV8912_REG_OP_CTRL_1 + (channel - 1) / 4;

    // Within each OP_CTRL register, every HB occupies a 2-bit field laid out as
    // (Table 17, e.g. OP_CTRL_1):
    //   bit7 HB4_HS_EN, bit6 HB4_LS_EN, ... bit1 HB1_HS_EN, bit0 HB1_LS_EN
    // So for the HB's index-within-register (0..3): LS = even bit (2*idx),
    // HS = odd bit (2*idx + 1).
    const uint8_t bit = 2 * ((channel - 1) % 4) + (high_side ? 1 : 0);

    uint8_t addresses[DRV8912_MAX_DEVICES];
    uint8_t values[DRV8912_MAX_DEVICES]   = {0};
    uint8_t statuses[DRV8912_MAX_DEVICES];

    // Daisy chain (sect. 8.5.3.1): one address+data byte is shifted per device
    // in a single frame, so every device in the chain must be given a command.
    // For the devices we don't want to disturb, target IC_STAT (0x00): it is a
    // read-only status register (Table 17), so a write to it is a harmless no-op.
    for (uint8_t i = 0; i < handle->device_count; i++) {
        addresses[i] = DRV8912_REG_IC_STAT;
    }
    addresses[device] = address;

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    // --- Read-modify-write ---
    // Read the current OP_CTRL value first so the other three HBs sharing this
    // register are preserved. Per sect. 8.5.2, a read (R/W = 1) returns the
    // register's current contents in the report byte (Table 14 / Fig. 72).
    esp_err_t err = drv8912_transfer(handle, addresses, values, true, false, values, statuses);
    if (err == ESP_OK) {
        if (enabled) {
            // Set the requested side, and clear its partner within the same pair.
            // Note (sect. 8.3.1.1.1 NOTE / Table 2): if both HS_EN and LS_EN of a
            // half-bridge are set, that HB is forced Hi-Z (shoot-through guard),
            // so we explicitly clear the opposite bit. (bit ^ 1 = partner bit.)
            values[device] |=  1U << bit;
            values[device] &= ~(1U << (bit ^ 1U));
        } else {
            // Disable: clear only this side. With both sides 0 the HB coasts
            // (Hi-Z / Motor Coast, Table 2).
            values[device] &= ~(1U << bit);
        }

        // Rebuild the frame for the write pass (R/W = 0). Re-assert the target
        // address and hold every other device at IC_STAT (read-only no-op) with
        // data 0, so only the intended device's OP_CTRL register is modified.
        addresses[device] = address;
        for (uint8_t i = 0; i < handle->device_count; i++) {
            if (i != device) {
                addresses[i] = DRV8912_REG_IC_STAT;
                values[i]    = 0;
            }
        }
        err = drv8912_transfer(handle, addresses, values, false, false, NULL, NULL);
    }

    xSemaphoreGive(handle->mutex);
    return err;
}