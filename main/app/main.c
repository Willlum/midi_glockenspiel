#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "midi_io.h"
#include "midi_parser.h"
#include "solenoid.h"

#define MOUNT_POINT "/sdcard"

//pins 2,14,15 for sd card
const uint8_t targetPins[MAX_SOLENOIDS] = {
    GPIO_NUM_18, 
    GPIO_NUM_19, 
    GPIO_NUM_21, 
    GPIO_NUM_22, 
    GPIO_NUM_23, 
    GPIO_NUM_25, 
    GPIO_NUM_26, 
    GPIO_NUM_27
};

const uint8_t enablePin = GPIO_NUM_13;

//const char filePath[] = MOUNT_POINT"/AFROCUBA.MID";
const char filePath[] = MOUNT_POINT"/AIR.MID";
//const char filePath[] = MOUNT_POINT"/CT_PEA~1.MID";
//const char filePath[] = MOUNT_POINT"/CT_FOR~1.MID";

void app_main(void)
{    
    solenoid_init(targetPins, enablePin);
    
    //solenoid_test();

    mount_sd();    
    list_sd_contents(MOUNT_POINT);

    read_midi_file(filePath, 1, -1);
    
    unmount_sd();
}