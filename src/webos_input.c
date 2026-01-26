/*
 * Moonlight webOS - Input Handling
 *
 * Maps TouchPad touch events to mouse input for game streaming.
 */

#include <stdio.h>
#include <stdbool.h>

#include "SDL.h"
#include "Limelight.h"
#include "webos_platform.h"

// Screen dimensions for coordinate mapping
static int screenWidth = 1024;
static int screenHeight = 768;

// Track touch state for relative mouse movement
static bool touchActive = false;
static int lastTouchX = 0;
static int lastTouchY = 0;

void webos_handle_touch(SDL_Event *event) {
    short deltaX, deltaY;
    int x, y;
    int fingerId;

    switch (event->type) {
        case SDL_MOUSEMOTION:
            x = event->motion.x;
            y = event->motion.y;
            // PDL uses 'which' field for finger ID when multi-touch is enabled
            fingerId = event->motion.which;

            // When gamepad is enabled, ALL touch goes to gamepad (no mouse)
            if (webos_gamepad_enabled()) {
                webos_gamepad_touch(SDL_MOUSEMOTION, fingerId, x, y);
                return;
            }

            // Only track first finger for mouse mode
            if (fingerId == 0 && touchActive) {
                // Calculate relative movement
                deltaX = (short)(x - lastTouchX);
                deltaY = (short)(y - lastTouchY);

                // Send relative mouse movement
                if (deltaX != 0 || deltaY != 0) {
                    LiSendMouseMoveEvent(deltaX, deltaY);
                }
                lastTouchX = x;
                lastTouchY = y;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            x = event->button.x;
            y = event->button.y;
            // PDL uses 'which' field for finger ID when multi-touch is enabled
            fingerId = event->button.which;

            // When gamepad is enabled, ALL touch goes to gamepad (no mouse)
            if (webos_gamepad_enabled()) {
                webos_gamepad_touch(SDL_MOUSEBUTTONDOWN, fingerId, x, y);
                return;
            }

            // Only track first finger for mouse mode
            if (fingerId == 0) {
                touchActive = true;
                lastTouchX = x;
                lastTouchY = y;
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            x = event->button.x;
            y = event->button.y;
            // PDL uses 'which' field for finger ID when multi-touch is enabled
            fingerId = event->button.which;

            // When gamepad is enabled, ALL touch goes to gamepad (no mouse)
            if (webos_gamepad_enabled()) {
                webos_gamepad_touch(SDL_MOUSEBUTTONUP, fingerId, x, y);
                return;
            }

            // Only track first finger for mouse mode
            if (fingerId == 0) {
                touchActive = false;
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            break;

        default:
            break;
    }
}

// Connection callbacks
static void connection_started(void) {
    printf("Connection started\n");
}

static void connection_terminated(int errorCode) {
    printf("Connection terminated: %d\n", errorCode);
}

static void display_message(const char *message) {
    printf("Message: %s\n", message);
}

static void display_transient_message(const char *message) {
    printf("Transient: %s\n", message);
}

static void log_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

static void rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
    // TouchPad doesn't have rumble, ignore
}

static void rumble_triggers(unsigned short controllerNumber, unsigned short leftTrigger, unsigned short rightTrigger) {
    // TouchPad doesn't have triggers, ignore
}

static void set_hdr_mode(bool enabled) {
    // Not supported
}

static void set_controller_led(unsigned short controllerNumber, unsigned char r, unsigned char g, unsigned char b) {
    // Not supported
}

CONNECTION_LISTENER_CALLBACKS webos_connection_callbacks = {
    .stageStarting = NULL,
    .stageComplete = NULL,
    .stageFailed = NULL,
    .connectionStarted = connection_started,
    .connectionTerminated = connection_terminated,
    .logMessage = log_message,
    .rumble = rumble,
    .connectionStatusUpdate = NULL,
    .setHdrMode = set_hdr_mode,
    .rumbleTriggers = rumble_triggers,
    .setControllerLED = set_controller_led,
};
