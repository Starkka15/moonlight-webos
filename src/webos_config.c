/*
 * Moonlight webOS - Configuration
 *
 * Loads/saves settings from a config file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webos_platform.h"

#define CONFIG_PATH "/media/internal/.moonlight/config.txt"

static char serverAddress[256] = "";
static int streamWidth = 854;
static int streamHeight = 480;
static int streamFps = 30;
static int streamBitrate = 3000;

const char* webos_config_get_server(void) {
    return serverAddress;
}

void webos_config_set_server(const char *addr) {
    strncpy(serverAddress, addr, sizeof(serverAddress) - 1);
    serverAddress[sizeof(serverAddress) - 1] = '\0';
}

int webos_config_get_width(void) { return streamWidth; }
int webos_config_get_height(void) { return streamHeight; }
int webos_config_get_fps(void) { return streamFps; }
int webos_config_get_bitrate(void) { return streamBitrate; }

void webos_config_set_resolution(int w, int h, int fps, int bitrate) {
    streamWidth = w;
    streamHeight = h;
    streamFps = fps;
    streamBitrate = bitrate;
}

int webos_config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        printf("No config file found at %s\n", CONFIG_PATH);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Parse key=value
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        // Trim whitespace
        while (*key == ' ') key++;
        while (*value == ' ') value++;

        if (strcmp(key, "server") == 0) {
            strncpy(serverAddress, value, sizeof(serverAddress) - 1);
        } else if (strcmp(key, "width") == 0) {
            streamWidth = atoi(value);
        } else if (strcmp(key, "height") == 0) {
            streamHeight = atoi(value);
        } else if (strcmp(key, "fps") == 0) {
            streamFps = atoi(value);
        } else if (strcmp(key, "bitrate") == 0) {
            streamBitrate = atoi(value);
        }
    }

    fclose(f);
    printf("Config loaded: server=%s, %dx%d@%d, %d kbps\n",
           serverAddress, streamWidth, streamHeight, streamFps, streamBitrate);
    return 0;
}

int webos_config_save(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        printf("Failed to save config to %s\n", CONFIG_PATH);
        return -1;
    }

    fprintf(f, "server=%s\n", serverAddress);
    fprintf(f, "width=%d\n", streamWidth);
    fprintf(f, "height=%d\n", streamHeight);
    fprintf(f, "fps=%d\n", streamFps);
    fprintf(f, "bitrate=%d\n", streamBitrate);

    fclose(f);
    printf("Config saved to %s\n", CONFIG_PATH);
    return 0;
}
