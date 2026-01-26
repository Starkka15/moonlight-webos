/*
 * Moonlight webOS - Virtual Gamepad Overlay
 *
 * Draws on-screen touch controls and sends gamepad input to the stream.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "SDL.h"
#include <GLES2/gl2.h>

#include "Limelight.h"
#include "webos_platform.h"

// Button IDs matching Limelight's controller buttons
#define BTN_A           0x1000
#define BTN_B           0x2000
#define BTN_X           0x4000
#define BTN_Y           0x8000
#define BTN_UP          0x0001
#define BTN_DOWN        0x0002
#define BTN_LEFT        0x0004
#define BTN_RIGHT       0x0008
#define BTN_START       0x0010
#define BTN_SELECT      0x0020
#define BTN_LB          0x0100
#define BTN_RB          0x0200
#define BTN_LS          0x0040  // Left stick click
#define BTN_RS          0x0080  // Right stick click

// Touch region structure
typedef struct {
    int x, y, w, h;     // Screen position and size
    int button;         // Button flag
    int active;         // Currently pressed
    int fingerId;       // Which finger is touching this
} TouchRegion;

// Virtual gamepad state
static int gamepadEnabled = 1;  // Enabled by default

// Double-tap detection for toggle
static int lastTapX = 0, lastTapY = 0;
static Uint32 lastTapTime = 0;
static int screenWidth = 1024;
static int screenHeight = 768;

// Current button state
static short currentButtons = 0;
static short leftStickX = 0, leftStickY = 0;
static short rightStickX = 0, rightStickY = 0;
static unsigned char leftTrigger = 0, rightTrigger = 0;

// Touch regions for buttons
#define MAX_REGIONS 16
static TouchRegion regions[MAX_REGIONS];
static int numRegions = 0;

// Analog stick touch tracking
static int leftStickFinger = -1;
static int leftStickCenterX, leftStickCenterY;
static int rightStickFinger = -1;
static int rightStickCenterX, rightStickCenterY;

// Add a button region
static void add_region(int x, int y, int w, int h, int button) {
    if (numRegions >= MAX_REGIONS) return;

    TouchRegion *r = &regions[numRegions++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->button = button;
    r->active = 0;
    r->fingerId = -1;
}

// Initialize virtual gamepad layout
void webos_gamepad_init(int width, int height) {
    screenWidth = width;
    screenHeight = height;
    numRegions = 0;

    // Button sizes
    int btnSize = 60;      // ABXY button size
    int dpadSize = 65;     // D-pad button size (bigger)
    int margin = 20;       // Edge margin
    int spacing = 8;       // Button spacing

    // Left side - D-pad (bottom left, bigger touch area)
    int dpadX = margin + dpadSize + 20;
    int dpadY = height - margin - dpadSize - 60;

    add_region(dpadX - dpadSize/2, dpadY - dpadSize - spacing, dpadSize, dpadSize, BTN_UP);
    add_region(dpadX - dpadSize/2, dpadY + spacing, dpadSize, dpadSize, BTN_DOWN);
    add_region(dpadX - dpadSize - spacing - dpadSize/2, dpadY - dpadSize/2, dpadSize, dpadSize, BTN_LEFT);
    add_region(dpadX + spacing + dpadSize/2, dpadY - dpadSize/2, dpadSize, dpadSize, BTN_RIGHT);

    // Right side - ABXY (bottom right) - Xbox diamond pattern
    int abxyX = width - margin - btnSize - 80;
    int abxyY = height - margin - btnSize - 60;

    add_region(abxyX, abxyY + btnSize + spacing, btnSize, btnSize, BTN_A);      // A - bottom
    add_region(abxyX + btnSize + spacing, abxyY, btnSize, btnSize, BTN_B);      // B - right
    add_region(abxyX - btnSize - spacing, abxyY, btnSize, btnSize, BTN_X);      // X - left
    add_region(abxyX, abxyY - btnSize - spacing, btnSize, btnSize, BTN_Y);      // Y - top

    // Shoulder buttons - ABOVE the d-pad and ABXY areas
    int shoulderW = 100;
    int shoulderH = 45;
    int shoulderY = dpadY - dpadSize - spacing - shoulderH - 30;  // Above d-pad
    add_region(margin, shoulderY, shoulderW, shoulderH, BTN_LB);
    add_region(width - margin - shoulderW, shoulderY, shoulderW, shoulderH, BTN_RB);

    // Start/Select (center bottom)
    int startW = 70;
    int startH = 35;
    add_region(width/2 - startW - 15, height - margin - startH - 15, startW, startH, BTN_SELECT);
    add_region(width/2 + 15, height - margin - startH - 15, startW, startH, BTN_START);

    // Left analog stick area (middle left)
    leftStickCenterX = margin + 90;
    leftStickCenterY = height / 2 - 50;

    // Right analog stick area (middle right)
    rightStickCenterX = width - margin - 90;
    rightStickCenterY = height / 2 - 50;

    printf("Virtual gamepad initialized: %d regions, screen %dx%d\n", numRegions, width, height);
}

// Check if point is in region
static int point_in_region(int x, int y, TouchRegion *r) {
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

// Check if point is in analog stick area
static int point_in_stick(int x, int y, int centerX, int centerY, int radius) {
    int dx = x - centerX;
    int dy = y - centerY;
    return (dx*dx + dy*dy) <= (radius * radius);
}

// Send controller state to stream
static void send_controller_state(void) {
    LiSendControllerEvent(currentButtons, leftTrigger, rightTrigger,
                          leftStickX, leftStickY, rightStickX, rightStickY);
}

// Handle touch event - returns 1 if consumed by gamepad
// SDL 1.2 only has mouse events for touch, so we use those
int webos_gamepad_touch(int type, int fingerId, int x, int y) {
    // Check for double-tap in top-right corner to toggle gamepad
    if (type == SDL_MOUSEBUTTONDOWN) {
        Uint32 now = SDL_GetTicks();
        // Top-right corner (within 100px of corner)
        if (x > screenWidth - 100 && y < 100) {
            if (now - lastTapTime < 400 &&
                abs(x - lastTapX) < 50 && abs(y - lastTapY) < 50) {
                // Double tap detected - toggle gamepad
                webos_gamepad_toggle();
                lastTapTime = 0;  // Reset to prevent triple-tap
                return 1;
            }
            lastTapX = x;
            lastTapY = y;
            lastTapTime = now;
        }
    }

    if (!gamepadEnabled) return 0;

    int stateChanged = 0;
    int stickRadius = 60;  // Smaller stick area
    int consumed = 0;

    // Check analog sticks first
    if (type == SDL_MOUSEBUTTONDOWN) {
        // Left stick
        if (leftStickFinger < 0 && point_in_stick(x, y, leftStickCenterX, leftStickCenterY, stickRadius)) {
            leftStickFinger = fingerId;
            stateChanged = 1;
            consumed = 1;
        }
        // Right stick
        else if (rightStickFinger < 0 && point_in_stick(x, y, rightStickCenterX, rightStickCenterY, stickRadius)) {
            rightStickFinger = fingerId;
            stateChanged = 1;
            consumed = 1;
        }
    }

    // Update analog stick position
    // Note: Screen Y increases downward, but controller Y should increase upward
    // So we negate dy for both sticks
    if (type == SDL_MOUSEMOTION || type == SDL_MOUSEBUTTONDOWN) {
        if (fingerId == leftStickFinger) {
            int dx = x - leftStickCenterX;
            int dy = y - leftStickCenterY;
            // Clamp to radius
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist > stickRadius) {
                dx = (int)(dx * stickRadius / dist);
                dy = (int)(dy * stickRadius / dist);
            }
            // Convert to -32768 to 32767, negate Y for proper up/down
            leftStickX = (short)(dx * 32767 / stickRadius);
            leftStickY = (short)(-dy * 32767 / stickRadius);
            stateChanged = 1;
            consumed = 1;
        }
        if (fingerId == rightStickFinger) {
            int dx = x - rightStickCenterX;
            int dy = y - rightStickCenterY;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist > stickRadius) {
                dx = (int)(dx * stickRadius / dist);
                dy = (int)(dy * stickRadius / dist);
            }
            rightStickX = (short)(dx * 32767 / stickRadius);
            rightStickY = (short)(-dy * 32767 / stickRadius);
            stateChanged = 1;
            consumed = 1;
        }
    }

    // Release analog stick
    if (type == SDL_MOUSEBUTTONUP) {
        if (fingerId == leftStickFinger) {
            leftStickFinger = -1;
            leftStickX = 0;
            leftStickY = 0;
            stateChanged = 1;
            consumed = 1;
        }
        if (fingerId == rightStickFinger) {
            rightStickFinger = -1;
            rightStickX = 0;
            rightStickY = 0;
            stateChanged = 1;
            consumed = 1;
        }
    }

    // Check button regions
    for (int i = 0; i < numRegions; i++) {
        TouchRegion *r = &regions[i];

        if (type == SDL_MOUSEBUTTONDOWN) {
            if (!r->active && point_in_region(x, y, r)) {
                r->active = 1;
                r->fingerId = fingerId;
                currentButtons |= r->button;
                stateChanged = 1;
                consumed = 1;
            }
        }
        else if (type == SDL_MOUSEBUTTONUP) {
            if (r->active && r->fingerId == fingerId) {
                r->active = 0;
                r->fingerId = -1;
                currentButtons &= ~r->button;
                stateChanged = 1;
                consumed = 1;
            }
        }
        else if (type == SDL_MOUSEMOTION) {
            // Handle finger sliding off button
            if (r->active && r->fingerId == fingerId && !point_in_region(x, y, r)) {
                r->active = 0;
                r->fingerId = -1;
                currentButtons &= ~r->button;
                stateChanged = 1;
            }
        }
    }

    if (stateChanged) {
        send_controller_state();
    }

    return consumed;
}

// Toggle gamepad on/off
void webos_gamepad_toggle(void) {
    gamepadEnabled = !gamepadEnabled;
    printf("Virtual gamepad %s\n", gamepadEnabled ? "enabled" : "disabled");

    // Reset state when disabled
    if (!gamepadEnabled) {
        currentButtons = 0;
        leftStickX = leftStickY = 0;
        rightStickX = rightStickY = 0;
        leftTrigger = rightTrigger = 0;
        send_controller_state();
    }
}

int webos_gamepad_enabled(void) {
    return gamepadEnabled;
}

// Get button regions for overlay rendering
int webos_gamepad_get_regions(int *outX, int *outY, int *outW, int *outH, int *outBtn, int maxRegions) {
    int count = numRegions < maxRegions ? numRegions : maxRegions;
    for (int i = 0; i < count; i++) {
        outX[i] = regions[i].x;
        outY[i] = regions[i].y;
        outW[i] = regions[i].w;
        outH[i] = regions[i].h;
        outBtn[i] = regions[i].button;
    }
    return count;
}

// Get analog stick positions for overlay
void webos_gamepad_get_sticks(int *lx, int *ly, int *rx, int *ry, int *radius) {
    *lx = leftStickCenterX;
    *ly = leftStickCenterY;
    *rx = rightStickCenterX;
    *ry = rightStickCenterY;
    *radius = 60;  // Smaller radius
}
