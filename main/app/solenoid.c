#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "solenoid.h"

static const char *TAG2 = "SOLENOID";

static uint8_t _solenoidPins[MAX_SOLENOIDS];
static uint8_t _enablePin;
static esp_timer_handle_t offTimers[MAX_SOLENOIDS];

volatile bool playbackAbortFlag = false;

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

    _enablePin = enablePin;
    gpio_set_direction(_enablePin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(_enablePin, GPIO_PULLDOWN_ONLY);
    gpio_install_isr_service(0);
    gpio_set_intr_type(_enablePin, GPIO_INTR_ANYEDGE); 
    gpio_isr_handler_add(_enablePin, enable_isr_handler, NULL);

    ESP_LOGD(TAG2, "Initialized %d solenoids with %dms pulse.", MAX_SOLENOIDS, SOLENOID_PULSE_MS);
    return ESP_OK;
}

void solenoid_strike(uint8_t midiNote, uint8_t velocity) {
    //if (velocity == 0) return;
    int index = midiNote - MIDI_BASE_NOTE;

    if (index >= 0 && index < MAX_SOLENOIDS) {
        // Stop the timer if it was already running to reset
        esp_timer_stop(offTimers[index]);

        gpio_set_level(_solenoidPins[index], 1);

        // Set timers LOW after set time (converted to uS)
        esp_timer_start_once(offTimers[index], SOLENOID_PULSE_MS * 1000);
        
        ESP_LOGI(TAG2, "Strike Note %d (Pin %d)", midiNote, _solenoidPins[index]);
    }
}

void solenoid_stop(void) {
    for (int i = 0; i < MAX_SOLENOIDS; i++) {
        esp_timer_stop(offTimers[i]);
        gpio_set_level(_solenoidPins[i], 0);
    }
    ESP_LOGI(TAG2, "Stop: All solenoids OFF");
}

void solenoid_test(void) {
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