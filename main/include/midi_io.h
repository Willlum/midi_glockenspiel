#pragma once

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  Mount the SD card
 */
void mount_sd(void);

/**
 * @brief  Unmount the SD card
 */
void unmount_sd(void);

void list_sd_contents(const char *path);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
