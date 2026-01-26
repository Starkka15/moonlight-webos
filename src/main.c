/*
 * Moonlight webOS - Game Streaming Client for HP TouchPad
 * Main entry point with GUI menu system
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#include "SDL.h"
#include "PDL.h"

#include "Limelight.h"
#include "client.h"
#include "errors.h"

#include "webos_platform.h"

static bool running = true;
static int appState = GUI_STATE_MAIN;
static char server_address[256] = "";
static SERVER_DATA server;
static STREAM_CONFIGURATION config;
static PAPP_LIST appList = NULL;
static bool streamActive = false;

// Exit gesture tracking
static int exitGestureStartY = -1;
static Uint32 exitGestureStartTime = 0;

// Key directory for certificates
#define KEY_DIR "/media/internal/.moonlight"

static void signal_handler(int sig) {
    printf("Signal %d received, shutting down...\n", sig);
    running = false;
}

static int pair_with_server(void) {
    int ret;
    char pin[5];

    // Generate 4-digit PIN
    snprintf(pin, sizeof(pin), "%04d", rand() % 10000);

    // Show PIN on screen
    char msg[128];
    snprintf(msg, sizeof(msg), "Enter PIN on your PC:  %s", pin);
    webos_gui_set_status(msg);
    printf("Please enter PIN %s on your PC\n", pin);

    // Render the pairing screen so user sees the PIN
    webos_gui_render(NULL);
    SDL_GL_SwapBuffers();

    // Now attempt pairing (this blocks until user enters PIN or timeout)
    ret = gs_pair(&server, pin);
    if (ret != GS_OK) {
        printf("Pairing failed: %d\n", ret);
        webos_gui_set_status("Pairing failed! Tap CONNECT to retry.");
        return -1;
    }

    printf("Pairing successful!\n");
    webos_gui_set_status("Paired successfully!");
    return 0;
}

static int connect_to_server(void) {
    int ret;

    printf("Connecting to %s...\n", server_address);
    webos_gui_set_status(server_address);

    // Ensure key directory exists
    mkdir(KEY_DIR, 0755);

    memset(&server, 0, sizeof(server));

    ret = gs_init(&server, server_address, 47989, KEY_DIR, 2, true);
    if (ret != GS_OK) {
        printf("Failed to connect to server: %d\n", ret);
        return -1;
    }

    printf("Connected to server running GFE %s\n",
           server.serverInfo.serverInfoGfeVersion ? server.serverInfo.serverInfoGfeVersion : "unknown");

    // Check if we need to pair
    if (!server.paired) {
        printf("Server not paired. Starting pairing process...\n");
        if (pair_with_server() != 0) {
            return -1;
        }
    }

    return 0;
}

static int fetch_app_list(void) {
    int ret = gs_applist(&server, &appList);
    if (ret != GS_OK || !appList) {
        printf("Failed to get app list: %d\n", ret);
        return -1;
    }

    // Debug: print apps with full details
    PAPP_LIST app = appList;
    int count = 0;
    printf("=== Available apps ===\n");
    while (app) {
        printf("  [%d] %s (id: %d)\n", count, app->name, app->id);
        count++;
        app = app->next;
    }
    printf("Total: %d apps\n", count);
    fflush(stdout);

    return 0;
}

static void setup_stream_config(void) {
    memset(&config, 0, sizeof(config));
    config.width = webos_config_get_width();
    config.height = webos_config_get_height();
    config.fps = webos_config_get_fps();
    config.bitrate = webos_config_get_bitrate();
    config.packetSize = 1024;
    config.streamingRemotely = 0;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;

    printf("Stream config: %dx%d @ %d fps, %d kbps\n",
           config.width, config.height, config.fps, config.bitrate);
}

static int start_streaming(int appId) {
    int ret;

    setup_stream_config();

    printf("=== Starting stream ===\n");
    printf("App ID: %d\n", appId);
    printf("Config: %dx%d @ %d fps, %d kbps\n",
           config.width, config.height, config.fps, config.bitrate);
    printf("Server: %s\n", server_address);
    fflush(stdout);

    // Start the stream on server
    printf("Calling gs_start_app...\n");
    fflush(stdout);
    ret = gs_start_app(&server, &config, appId, false, false, 0);
    if (ret != GS_OK) {
        printf("Failed to start stream: %d\n", ret);
        return -1;
    }
    printf("gs_start_app succeeded\n");
    fflush(stdout);

    // Initialize video for streaming (sets up GL context)
    if (webos_video_init() != 0) {
        printf("Video initialization failed\n");
        gs_quit_app(&server);
        return -1;
    }

    // Initialize audio
    if (webos_audio_init() != 0) {
        printf("Audio initialization failed\n");
        webos_video_cleanup();
        gs_quit_app(&server);
        return -1;
    }

    // Initialize gamepad
    webos_gamepad_init(1024, 768);

    // Connect to stream
    ret = LiStartConnection(&server.serverInfo, &config,
                            &webos_connection_callbacks,
                            &webos_video_callbacks,
                            &webos_audio_callbacks,
                            NULL, 0, NULL, 0);

    if (ret != 0) {
        printf("Failed to start connection: %d\n", ret);
        webos_audio_cleanup();
        webos_video_cleanup();
        gs_quit_app(&server);
        return -1;
    }

    printf("Stream started successfully!\n");
    streamActive = true;
    return 0;
}

static void stop_streaming(void) {
    if (streamActive) {
        printf("Stopping stream...\n");
        LiStopConnection();
        gs_quit_app(&server);
        webos_audio_cleanup();
        webos_video_cleanup();
        streamActive = false;
        // GL context remains valid, GUI will use it
    }
}

// Check for exit gesture (swipe down from top)
static int check_exit_gesture(SDL_Event *event) {
    if (event->type == SDL_MOUSEBUTTONDOWN) {
        if (event->button.y < 50) {
            exitGestureStartY = event->button.y;
            exitGestureStartTime = SDL_GetTicks();
        }
    }
    else if (event->type == SDL_MOUSEMOTION) {
        if (exitGestureStartY >= 0) {
            int dy = event->motion.y - exitGestureStartY;
            if (dy > 150) {
                exitGestureStartY = -1;
                return 1; // Exit gesture detected
            }
        }
    }
    else if (event->type == SDL_MOUSEBUTTONUP) {
        exitGestureStartY = -1;
    }

    return 0;
}

static void main_loop(void) {
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        if (appState == GUI_STATE_STREAMING) {
                            stop_streaming();
                            appState = GUI_STATE_MAIN;
                        } else if (appState == GUI_STATE_APPS) {
                            appState = GUI_STATE_MAIN;
                            webos_gui_set_state(GUI_STATE_MAIN);
                        } else {
                            running = false;
                        }
                    }
                    else if (event.key.keysym.sym == SDLK_g && appState == GUI_STATE_STREAMING) {
                        webos_gamepad_toggle();
                    }
                    break;

                case SDL_MOUSEMOTION:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    if (appState == GUI_STATE_STREAMING) {
                        // Check for exit gesture
                        if (check_exit_gesture(&event)) {
                            stop_streaming();
                            appState = GUI_STATE_MAIN;
                            webos_gui_set_state(GUI_STATE_MAIN);
                            break;
                        }
                        // Normal touch handling for streaming
                        webos_handle_touch(&event);
                    }
                    else {
                        // Menu touch handling
                        int result = webos_gui_handle_event(&event);
                        if (result == 2) {
                            // Connect button pressed
                            appState = GUI_STATE_CONNECTING;
                            webos_gui_set_state(GUI_STATE_CONNECTING);
                        }
                        else if (result == 3) {
                            // App selected, start streaming
                            int appId = webos_gui_get_selected_app();
                            if (start_streaming(appId) == 0) {
                                appState = GUI_STATE_STREAMING;
                            } else {
                                webos_gui_set_status("Failed to start stream");
                                appState = GUI_STATE_APPS;
                                webos_gui_set_state(GUI_STATE_APPS);
                            }
                        }
                    }
                    break;

                default:
                    break;
            }
        }

        // State-specific processing
        switch (appState) {
            case GUI_STATE_MAIN:
            case GUI_STATE_APPS:
                webos_gui_render(NULL);  // GL rendering, no surface needed
                SDL_GL_SwapBuffers();
                SDL_Delay(16); // ~60fps
                break;

            case GUI_STATE_CONNECTING:
                // Show connecting message and attempt connection
                webos_gui_render(NULL);
                SDL_GL_SwapBuffers();

                // Get server address from GUI
                strncpy(server_address, webos_gui_get_server(), sizeof(server_address) - 1);

                if (connect_to_server() == 0) {
                    if (fetch_app_list() == 0) {
                        webos_gui_set_apps(appList);
                        appState = GUI_STATE_APPS;
                        webos_gui_set_state(GUI_STATE_APPS);
                    } else {
                        webos_gui_set_status("Failed to get app list");
                        appState = GUI_STATE_MAIN;
                        webos_gui_set_state(GUI_STATE_MAIN);
                    }
                } else {
                    webos_gui_set_status("Connection failed");
                    appState = GUI_STATE_MAIN;
                    webos_gui_set_state(GUI_STATE_MAIN);
                }
                break;

            case GUI_STATE_STREAMING:
                webos_video_render();
                SDL_Delay(1);
                break;
        }
    }
}

static void cleanup(void) {
    printf("Cleaning up...\n");

    if (streamActive) {
        LiStopConnection();
        gs_quit_app(&server);
        webos_video_cleanup();
        webos_audio_cleanup();
    }

    webos_gui_cleanup();
    PDL_Quit();
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    // Load config file
    webos_config_load();

    // Seed random for PIN generation
    srand((unsigned int)time(NULL));

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Initialize PDL
    if (PDL_Init(0) != PDL_NOERROR) {
        printf("PDL_Init failed\n");
        SDL_Quit();
        return 1;
    }

    // Enable multi-touch support
    PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES);

    // Initialize OpenGL context early (before GUI)
    // This must be done first so we can use GL for both menu and streaming
    if (webos_video_early_init() != 0) {
        printf("OpenGL initialization failed\n");
        PDL_Quit();
        SDL_Quit();
        return 1;
    }

    // Initialize GUI (will use OpenGL for rendering)
    if (webos_gui_init() != 0) {
        printf("GUI initialization failed\n");
        PDL_Quit();
        SDL_Quit();
        return 1;
    }

    // Check if server was passed as argument (for backward compatibility)
    if (argc >= 2) {
        webos_config_set_server(argv[1]);
        webos_config_save();
    }

    printf("Moonlight webOS started - showing menu\n");

    // Main event loop
    main_loop();

    // Cleanup
    cleanup();

    printf("Goodbye!\n");
    return 0;
}
