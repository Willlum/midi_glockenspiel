#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "note_map.h"
#include "solenoid.h"

static const char *TAG2 = "SOLENOID";

#define DRV_SOLENOID_PULSE_MS 10
#define DRV_NOTE_QUEUE_LENGTH 32

typedef enum {
    DRV_NOTE_COMMAND_STRIKE,
    DRV_NOTE_COMMAND_STOP,
} drv_note_command_type_t;

typedef struct {
    drv_note_command_type_t type;
    uint8_t midi_note;
    uint8_t velocity;
} drv_note_command_t;

static uint8_t _solenoidPins[MAX_SOLENOIDS];
static uint8_t _enablePin;
static esp_timer_handle_t offTimers[MAX_SOLENOIDS];
static bool gpio_solenoids_initialized;
static drv8912_handle_t drv_handle;
static QueueHandle_t drv_note_queue;
static SemaphoreHandle_t drv_stop_ack;

volatile bool playbackAbortFlag = false;

static esp_err_t set_drv_note_output(uint8_t midi_note, bool enabled)
{
    const note_map_entry_t *output = note_map_get(midi_note);
    if (output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return drv8912_set_half_bridge(drv_handle, output->device_index, output->channel,
                                   output->high_side, enabled);
}

static void drv_note_task(void *arg)
{
    bool active[NOTE_MAP_COUNT] = {false};
    int64_t off_deadline_us[NOTE_MAP_COUNT] = {0};

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        int64_t next_deadline_us = INT64_MAX;
        for (size_t i = 0; i < NOTE_MAP_COUNT; i++) {
            if (active[i] && off_deadline_us[i] <= now_us) {
                const uint8_t midi_note = NOTE_MAP_MIN_MIDI_NOTE + i;
                esp_err_t err = set_drv_note_output(midi_note, false);
                if (err == ESP_OK) {
                    active[i] = false;
                } else {
                    ESP_LOGE(TAG2, "Failed to turn off MIDI note %u: %s",
                             midi_note, esp_err_to_name(err));
                    off_deadline_us[i] = now_us + 1000;
                }
            }
            if (active[i] && off_deadline_us[i] < next_deadline_us) {
                next_deadline_us = off_deadline_us[i];
            }
        }

        TickType_t wait_ticks = portMAX_DELAY;
        if (next_deadline_us != INT64_MAX) {
            const int64_t remaining_us = next_deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                wait_ticks = 0;
            } else {
                const uint64_t remaining_ms = (remaining_us + 999) / 1000;
                wait_ticks = pdMS_TO_TICKS(remaining_ms);
                if (wait_ticks == 0) {
                    wait_ticks = 1;
                }
            }
        }

        drv_note_command_t command;
        if (xQueueReceive(drv_note_queue, &command, wait_ticks) != pdTRUE) {
            continue;
        }

        if (command.type == DRV_NOTE_COMMAND_STOP) {
            for (size_t i = 0; i < NOTE_MAP_COUNT; i++) {
                if (active[i]) {
                    const uint8_t midi_note = NOTE_MAP_MIN_MIDI_NOTE + i;
                    esp_err_t err = set_drv_note_output(midi_note, false);
                    if (err == ESP_OK) {
                        active[i] = false;
                    } else {
                        ESP_LOGE(TAG2, "Failed to stop MIDI note %u: %s",
                                 midi_note, esp_err_to_name(err));
                        off_deadline_us[i] = esp_timer_get_time() + 1000;
                    }
                }
            }
            xSemaphoreGive(drv_stop_ack);
            continue;
        }

        if (command.velocity == 0) {
            continue;
        }

        const note_map_entry_t *output = note_map_get(command.midi_note);
        if (output == NULL) {
            ESP_LOGD(TAG2, "Ignoring MIDI note %u outside the glockenspiel range",
                     command.midi_note);
            continue;
        }

        const size_t index = command.midi_note - NOTE_MAP_MIN_MIDI_NOTE;
        if (active[index]) {
            esp_err_t err = set_drv_note_output(command.midi_note, false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG2, "Failed to retrigger MIDI note %u: %s",
                         command.midi_note, esp_err_to_name(err));
                off_deadline_us[index] = esp_timer_get_time() + 1000;
                continue;
            }
            active[index] = false;
        }

        esp_err_t err = set_drv_note_output(command.midi_note, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG2, "Failed to strike MIDI note %u: %s",
                     command.midi_note, esp_err_to_name(err));
            continue;
        }

        active[index] = true;
        off_deadline_us[index] = esp_timer_get_time() + DRV_SOLENOID_PULSE_MS * 1000;
    }
}

static void IRAM_ATTR enable_isr_handler(void* arg) {
    if (gpio_get_level(_enablePin) == 1){playbackAbortFlag = true;} 
    else {playbackAbortFlag = false;} 
}

// Callback: Runs in the esp_timer task 
static void solenoid_off_callback(void* arg) {
    int index = (int)arg;
    if (index < MAX_SOLENOIDS) {
        gpio_set_level(_solenoidPins[index], 0);
    }
}

esp_err_t solenoid_init(const uint8_t* solenoidPins, const uint8_t enablePin) {
    if (solenoidPins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (drv_note_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < MAX_SOLENOIDS; i++) {
        _solenoidPins[i] = solenoidPins[i];

        const esp_timer_create_args_t timerArgs = {
            .callback = &solenoid_off_callback,
            .arg = (void*)i,
            .name = "solenoid_off"
        };
        
        esp_err_t err = esp_timer_create(&timerArgs, &offTimers[i]);
        if (err != ESP_OK) return err;

        gpio_reset_pin(_solenoidPins[i]);
        gpio_set_direction(_solenoidPins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(_solenoidPins[i], 0);
    }

    gpio_solenoids_initialized = true;
    _enablePin = enablePin;
    gpio_set_direction(_enablePin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(_enablePin, GPIO_PULLDOWN_ONLY);
    gpio_install_isr_service(0);
    gpio_set_intr_type(_enablePin, GPIO_INTR_ANYEDGE); 
    gpio_isr_handler_add(_enablePin, enable_isr_handler, NULL);

    ESP_LOGD(TAG2, "Initialized %d solenoids with %dms pulse.", MAX_SOLENOIDS, SOLENOID_PULSE_MS);
    return ESP_OK;
}

esp_err_t solenoid_init_drv(drv8912_handle_t driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (drv_note_queue != NULL) {
        return drv_handle == driver ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t queue = xQueueCreate(DRV_NOTE_QUEUE_LENGTH, sizeof(drv_note_command_t));
    SemaphoreHandle_t stop_ack = xSemaphoreCreateBinary();
    if (queue == NULL || stop_ack == NULL) {
        if (queue != NULL) {
            vQueueDelete(queue);
        }
        if (stop_ack != NULL) {
            vSemaphoreDelete(stop_ack);
        }
        return ESP_ERR_NO_MEM;
    }

    drv_handle = driver;
    drv_note_queue = queue;
    drv_stop_ack = stop_ack;
    BaseType_t task_created = xTaskCreate(drv_note_task, "drv_note_out", 4096,
                                          NULL, 6, NULL);
    if (task_created != pdPASS) {
        drv_note_queue = NULL;
        drv_stop_ack = NULL;
        drv_handle = NULL;
        vQueueDelete(queue);
        vSemaphoreDelete(stop_ack);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t solenoid_strike(uint8_t midiNote, uint8_t velocity) {
    if (velocity == 0) {
        return ESP_OK;
    }

    if (drv_note_queue != NULL) {
        if (note_map_get(midiNote) == NULL) {
            ESP_LOGD(TAG2, "Ignoring MIDI note %u outside the glockenspiel range", midiNote);
            return ESP_OK;
        }

        const drv_note_command_t command = {
            .type = DRV_NOTE_COMMAND_STRIKE,
            .midi_note = midiNote,
            .velocity = velocity,
        };
        return xQueueSend(drv_note_queue, &command, pdMS_TO_TICKS(100)) == pdTRUE
                   ? ESP_OK
                   : ESP_ERR_TIMEOUT;
    }

    int index = midiNote - MIDI_BASE_NOTE;

    if (index >= 0 && index < MAX_SOLENOIDS) {
        if (!gpio_solenoids_initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        // Stop the timer if it was already running to reset
        esp_timer_stop(offTimers[index]);

        gpio_set_level(_solenoidPins[index], 1);

        // Set timers LOW after set time (converted to uS)
        esp_timer_start_once(offTimers[index], SOLENOID_PULSE_MS * 1000);
        
        ESP_LOGI(TAG2, "Strike Note %d (Pin %d)", midiNote, _solenoidPins[index]);
    }
    return ESP_OK;
}

void solenoid_stop(void) {
    if (drv_note_queue != NULL) {
        const drv_note_command_t command = {
            .type = DRV_NOTE_COMMAND_STOP,
        };
        if (xQueueSend(drv_note_queue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG2, "Failed to queue stop for DRV solenoids");
            return;
        }
        if (xSemaphoreTake(drv_stop_ack, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG2, "Timed out waiting for DRV solenoids to stop");
        }
        return;
    }

    if (!gpio_solenoids_initialized) {
        return;
    }

    for (int i = 0; i < MAX_SOLENOIDS; i++) {
        esp_timer_stop(offTimers[i]);
        gpio_set_level(_solenoidPins[i], 0);
    }
    ESP_LOGI(TAG2, "Stop: All solenoids OFF");
}

void solenoid_test(void) {
    if (!gpio_solenoids_initialized) {
        ESP_LOGE(TAG2, "GPIO solenoids are not initialized");
        return;
    }

    const TickType_t delay = 400 / portTICK_PERIOD_MS;
    
    while(!playbackAbortFlag){
        for(int i = 0; i< MAX_SOLENOIDS; i++){
            if(playbackAbortFlag) break;
            
            gpio_set_level(_solenoidPins[i],1);
            esp_timer_start_once(offTimers[i], SOLENOID_PULSE_MS * 1000);
            ESP_LOGI(TAG2, "Firing pin %d", _solenoidPins[i]);
            vTaskDelay(delay);
        }
    } 
    
    solenoid_stop();
}