/*
 * Moonlight webOS - Platform Abstraction Header
 */

#ifndef WEBOS_PLATFORM_H
#define WEBOS_PLATFORM_H

#include "SDL.h"
#include "Limelight.h"
#include "client.h"

// GUI states
#define GUI_STATE_MAIN       0
#define GUI_STATE_CONNECTING 1
#define GUI_STATE_APPS       2
#define GUI_STATE_STREAMING  3

// Video subsystem
int webos_video_early_init(void);  // Initialize OpenGL context (call before GUI)
int webos_video_init(void);        // Initialize decoder (call when starting stream)
void webos_video_cleanup(void);
void webos_video_render(void);     // Call from main loop to render frames

// Audio subsystem
int webos_audio_init(void);
void webos_audio_cleanup(void);

// Input handling
void webos_handle_touch(SDL_Event *event);

// Virtual gamepad
void webos_gamepad_init(int width, int height);
int webos_gamepad_touch(int type, int fingerId, int x, int y);
void webos_gamepad_toggle(void);
int webos_gamepad_enabled(void);
int webos_gamepad_get_regions(int *outX, int *outY, int *outW, int *outH, int *outBtn, int maxRegions);
void webos_gamepad_get_sticks(int *lx, int *ly, int *rx, int *ry, int *radius);

// Configuration
int webos_config_load(void);
int webos_config_save(void);
const char* webos_config_get_server(void);
void webos_config_set_server(const char *addr);
int webos_config_get_width(void);
int webos_config_get_height(void);
int webos_config_get_fps(void);
int webos_config_get_bitrate(void);

// GUI
int webos_gui_init(void);
void webos_gui_cleanup(void);
void webos_gui_set_state(int state);
int webos_gui_get_state(void);
void webos_gui_set_status(const char *msg);
void webos_gui_render(SDL_Surface *screen);
int webos_gui_handle_event(SDL_Event *event);
const char* webos_gui_get_server(void);
void webos_gui_set_apps(PAPP_LIST apps);
int webos_gui_get_selected_app(void);

// Callbacks for Limelight
extern CONNECTION_LISTENER_CALLBACKS webos_connection_callbacks;
extern DECODER_RENDERER_CALLBACKS webos_video_callbacks;
extern AUDIO_RENDERER_CALLBACKS webos_audio_callbacks;

#endif /* WEBOS_PLATFORM_H */
