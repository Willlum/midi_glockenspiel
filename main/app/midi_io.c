#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h> 
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "midi_io.h"
#include "sdkconfig.h"
#include "esp_log.h"

#define MOUNT_POINT "/sdcard"
static const char *TAG = "MIDI_IO";
static sdmmc_card_t *card;    
static const char mount_point[] = MOUNT_POINT;

#define EXAMPLE_MAX_CHAR_SIZE    64

void mount_sd(void)
{
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 20,
        .allocation_unit_size = 16 * 1024
    };        

    ESP_LOGI(TAG, "Initializing SD card");  

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize card (%s).", esp_err_to_name(ret));
        }
        return;
    }
    
    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);
}

void unmount_sd(void)
{
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
}

/**
 * Iterates through the SD card and prints all files and directories in root.
 */
void list_sd_contents(const char *path) 
{
    ESP_LOGI(TAG, "Listing objects in %s:", path);

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        struct stat entry_stat;
        char full_path[512];
        
        // Construct the full path to get file stats
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (stat(full_path, &entry_stat) == 0) {
            if (S_ISDIR(entry_stat.st_mode)) {
                printf("[DIR ] %s\n", entry->d_name);
            } else {
                printf("[FILE] %-10s  (Size: %ld bytes)\n", entry->d_name, (long)entry_stat.st_size);
            }
        } else {
            // Fallback if stat fails
            printf("[????] %s\n", entry->d_name);
        }
    }

    closedir(dir);
}