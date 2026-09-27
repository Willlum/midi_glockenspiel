#pragma once

#define MOUNT_POINT "/sdcard"

void mount_sd(void);

void unmount_sd(void);

void list_sd_contents(const char *path);

