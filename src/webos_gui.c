/*
 * Moonlight webOS - GUI Menu System (OpenGL)
 *
 * Touch-based menu for server selection and app launching.
 * Uses OpenGL ES 2.0 for rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_ttf.h"
#include <GLES2/gl2.h>

#include "client.h"
#include "webos_platform.h"

// Screen dimensions
#define SCREEN_W 1024
#define SCREEN_H 768

// Colors (RGBA floats)
static const float COLOR_BG[] = {0.1f, 0.1f, 0.18f, 1.0f};
static const float COLOR_PRIMARY[] = {0.086f, 0.129f, 0.243f, 1.0f};
static const float COLOR_ACCENT[] = {0.059f, 0.204f, 0.376f, 1.0f};
static const float COLOR_HIGHLIGHT[] = {0.914f, 0.271f, 0.376f, 1.0f};
static const float COLOR_TEXT[] = {1.0f, 1.0f, 1.0f, 1.0f};

// Button structure
typedef struct {
    int x, y, w, h;
    char label[32];
    int id;
    int pressed;
} Button;

// GUI state
static int guiState = GUI_STATE_MAIN;
static TTF_Font *fontLarge = NULL;
static TTF_Font *fontMedium = NULL;
static TTF_Font *fontSmall = NULL;

// Server IP input
static char serverIP[64] = "";
static int ipCursorPos = 0;

// App list
static PAPP_LIST appList = NULL;
static int selectedAppId = 0;

// Numpad buttons
#define NUM_NUMPAD_BTNS 14
static Button numpadBtns[NUM_NUMPAD_BTNS];

// App list buttons
#define MAX_APP_BTNS 8
static Button appBtns[MAX_APP_BTNS];
static int numAppBtns = 0;

// Other buttons
static Button connectBtn;
static Button backBtn;

// Status message
static char statusMsg[128] = "";

// GL resources for GUI
static GLuint guiProgram = 0;
static GLint guiPosLoc, guiColorLoc;

// Simple shader for colored rectangles
static const char *guiVertexSrc =
    "attribute vec2 position;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *guiFragmentSrc =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "    gl_FragColor = uColor;\n"
    "}\n";

// Text texture rendering
static GLuint textProgram = 0;
static GLint textPosLoc, textTexCoordLoc, textTexLoc, textColorLoc;
static GLuint textTexture = 0;

static const char *textVertexSrc =
    "attribute vec2 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    vTexCoord = texCoord;\n"
    "}\n";

static const char *textFragmentSrc =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "    vec4 texColor = texture2D(uTexture, vTexCoord);\n"
    "    // SDL_ttf ARGB8888 is BGRA in memory (little-endian)\n"
    "    // texColor.r = B, texColor.g = G, texColor.b = R, texColor.a = A\n"
    "    // Use alpha from texture, color from uniform\n"
    "    gl_FragColor = vec4(uColor.rgb, texColor.a);\n"
    "}\n";

// Compile shader helper
static GLuint compileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("Shader compile error: %s\n", log);
        return 0;
    }
    return shader;
}

// Create program helper
static GLuint createProgram(const char *vSrc, const char *fSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fSrc);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        printf("Program link error: %s\n", log);
        return 0;
    }
    return prog;
}

// Convert pixel coords to GL coords (-1 to 1)
static float toGLX(int x) { return (x / (float)SCREEN_W) * 2.0f - 1.0f; }
static float toGLY(int y) { return 1.0f - (y / (float)SCREEN_H) * 2.0f; }

// Draw filled rectangle
static void drawRect(int x, int y, int w, int h, const float *color) {
    float x1 = toGLX(x), y1 = toGLY(y);
    float x2 = toGLX(x + w), y2 = toGLY(y + h);

    float vertices[] = {
        x1, y1,
        x2, y1,
        x1, y2,
        x2, y2
    };

    glUseProgram(guiProgram);
    glUniform4fv(guiColorLoc, 1, color);
    glVertexAttribPointer(guiPosLoc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(guiPosLoc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Draw text using texture
static void drawText(TTF_Font *font, const char *text, int x, int y, const float *color, int centered) {
    if (!font || !text || !text[0]) return;

    // Render white text - we'll tint it with the shader
    SDL_Color sdlColor = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderText_Blended(font, text, sdlColor);
    if (!surf) {
        printf("TTF_RenderText_Blended failed: %s\n", TTF_GetError());
        return;
    }

    // Activate texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textTexture);

    // Upload texture - SDL_ttf ARGB8888 is BGRA in memory on little-endian
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Calculate position
    int tx = centered ? x - surf->w / 2 : x;
    int ty = y;

    float x1 = toGLX(tx), y1 = toGLY(ty);
    float x2 = toGLX(tx + surf->w), y2 = toGLY(ty + surf->h);

    float vertices[] = { x1, y1, x2, y1, x1, y2, x2, y2 };
    float texCoords[] = { 0, 0, 1, 0, 0, 1, 1, 1 };

    glUseProgram(textProgram);
    glUniform4fv(textColorLoc, 1, color);
    glUniform1i(textTexLoc, 0);

    glVertexAttribPointer(textPosLoc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(textTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(textPosLoc);
    glEnableVertexAttribArray(textTexCoordLoc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

    glDisableVertexAttribArray(textPosLoc);
    glDisableVertexAttribArray(textTexCoordLoc);

    SDL_FreeSurface(surf);
}

// Draw text centered in rect
static void drawTextCentered(TTF_Font *font, const char *text, int x, int y, int w, int h, const float *color) {
    drawText(font, text, x + w/2, y + h/4, color, 1);
}

// Draw button
static void drawButton(Button *btn, TTF_Font *font) {
    const float *bgColor = btn->pressed ? COLOR_HIGHLIGHT : COLOR_ACCENT;
    drawRect(btn->x, btn->y, btn->w, btn->h, bgColor);
    drawTextCentered(font, btn->label, btn->x, btn->y, btn->w, btn->h, COLOR_TEXT);
}

// Check if point is in button
static int pointInButton(int x, int y, Button *btn) {
    return x >= btn->x && x < btn->x + btn->w &&
           y >= btn->y && y < btn->y + btn->h;
}

// Initialize numpad buttons
static void initNumpad(void) {
    int startX = 280;
    int startY = 280;
    int btnW = 80;
    int btnH = 70;
    int spacing = 10;

    const char *labels[] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        ".", "0", "<-"
    };

    for (int i = 0; i < 12; i++) {
        int row = i / 3;
        int col = i % 3;
        Button *btn = &numpadBtns[i];
        btn->x = startX + col * (btnW + spacing);
        btn->y = startY + row * (btnH + spacing);
        btn->w = btnW;
        btn->h = btnH;
        strncpy(btn->label, labels[i], sizeof(btn->label) - 1);
        btn->id = i;
        btn->pressed = 0;
    }

    // DEL button
    numpadBtns[12].x = startX + 3 * (btnW + spacing) + 30;
    numpadBtns[12].y = startY;
    numpadBtns[12].w = 100;
    numpadBtns[12].h = btnH;
    strncpy(numpadBtns[12].label, "DEL", sizeof(numpadBtns[12].label) - 1);
    numpadBtns[12].id = 12;

    // CONNECT button
    connectBtn.x = startX + 3 * (btnW + spacing) + 30;
    connectBtn.y = startY + 2 * (btnH + spacing);
    connectBtn.w = 140;
    connectBtn.h = btnH * 2 + spacing;
    strncpy(connectBtn.label, "CONNECT", sizeof(connectBtn.label) - 1);
    connectBtn.id = 100;
}

// Initialize GUI
int webos_gui_init(void) {
    if (TTF_Init() < 0) {
        printf("TTF_Init failed: %s\n", TTF_GetError());
        return -1;
    }

    // Load font
    const char *fontPaths[] = {
        "/media/cryptofs/apps/usr/palm/applications/com.stark.moonlight/font.ttf",
        "./font.ttf",
        "/media/internal/.moonlight/font.ttf"
    };

    for (int i = 0; i < 3; i++) {
        printf("Trying font: %s\n", fontPaths[i]);
        fontLarge = TTF_OpenFont(fontPaths[i], 36);
        if (fontLarge) {
            printf("SUCCESS: Loaded font from: %s\n", fontPaths[i]);
            fontMedium = TTF_OpenFont(fontPaths[i], 28);
            fontSmall = TTF_OpenFont(fontPaths[i], 20);
            break;
        } else {
            printf("  Failed: %s\n", TTF_GetError());
        }
    }

    if (!fontLarge) {
        printf("WARNING: No font loaded - text will not display\n");
        // Continue without fonts - buttons will still work
    }

    // Create GL programs
    guiProgram = createProgram(guiVertexSrc, guiFragmentSrc);
    if (!guiProgram) {
        printf("Failed to create GUI program\n");
        return -1;
    }
    guiPosLoc = glGetAttribLocation(guiProgram, "position");
    guiColorLoc = glGetUniformLocation(guiProgram, "uColor");

    textProgram = createProgram(textVertexSrc, textFragmentSrc);
    if (!textProgram) {
        printf("Failed to create text program\n");
        return -1;
    }
    textPosLoc = glGetAttribLocation(textProgram, "position");
    textTexCoordLoc = glGetAttribLocation(textProgram, "texCoord");
    textTexLoc = glGetUniformLocation(textProgram, "uTexture");
    textColorLoc = glGetUniformLocation(textProgram, "uColor");

    printf("Text shader locations: pos=%d texCoord=%d tex=%d color=%d\n",
           textPosLoc, textTexCoordLoc, textTexLoc, textColorLoc);

    // Create text texture
    glGenTextures(1, &textTexture);
    printf("Text texture ID: %u\n", textTexture);

    initNumpad();

    // Back button
    backBtn.x = 20;
    backBtn.y = 20;
    backBtn.w = 100;
    backBtn.h = 50;
    strncpy(backBtn.label, "< BACK", sizeof(backBtn.label) - 1);
    backBtn.id = 200;

    // Load saved server IP
    const char *savedIP = webos_config_get_server();
    if (savedIP && savedIP[0]) {
        strncpy(serverIP, savedIP, sizeof(serverIP) - 1);
        ipCursorPos = strlen(serverIP);
    }

    printf("GUI initialized (OpenGL)\n");
    return 0;
}

void webos_gui_cleanup(void) {
    if (textTexture) glDeleteTextures(1, &textTexture);
    if (guiProgram) glDeleteProgram(guiProgram);
    if (textProgram) glDeleteProgram(textProgram);
    if (fontLarge) TTF_CloseFont(fontLarge);
    if (fontMedium) TTF_CloseFont(fontMedium);
    if (fontSmall) TTF_CloseFont(fontSmall);
    TTF_Quit();
}

void webos_gui_set_state(int state) {
    guiState = state;
    statusMsg[0] = '\0';

    if (state == GUI_STATE_APPS && appList) {
        numAppBtns = 0;
        PAPP_LIST app = appList;
        int y = 100;
        while (app && numAppBtns < MAX_APP_BTNS) {
            Button *btn = &appBtns[numAppBtns];
            btn->x = 100;
            btn->y = y;
            btn->w = SCREEN_W - 200;
            btn->h = 70;
            strncpy(btn->label, app->name, sizeof(btn->label) - 1);
            btn->id = app->id;
            btn->pressed = 0;
            numAppBtns++;
            y += 80;
            app = app->next;
        }
    }
}

int webos_gui_get_state(void) { return guiState; }

void webos_gui_set_status(const char *msg) {
    if (msg) strncpy(statusMsg, msg, sizeof(statusMsg) - 1);
    else statusMsg[0] = '\0';
}

const char* webos_gui_get_server(void) { return serverIP; }
void webos_gui_set_apps(PAPP_LIST apps) { appList = apps; }
int webos_gui_get_selected_app(void) { return selectedAppId; }

// Render main menu
static void renderMainMenu(void) {
    // Title
    drawText(fontLarge, "MOONLIGHT", SCREEN_W/2, 70, COLOR_TEXT, 1);

    static const float dimColor[] = {0.5f, 0.5f, 0.5f, 1.0f};
    drawText(fontSmall, "Game Streaming for webOS", SCREEN_W/2, 120, dimColor, 1);

    // IP display box
    drawRect(280, 180, 280, 60, COLOR_PRIMARY);
    if (serverIP[0]) {
        drawText(fontMedium, serverIP, 420, 195, COLOR_TEXT, 1);
    } else {
        drawText(fontMedium, "Enter IP", 420, 195, dimColor, 1);
    }

    // Numpad
    for (int i = 0; i < 13; i++) {
        drawButton(&numpadBtns[i], fontMedium);
    }

    // Connect button
    drawButton(&connectBtn, fontMedium);

    // Status message
    if (statusMsg[0]) {
        drawText(fontSmall, statusMsg, SCREEN_W/2, SCREEN_H - 60, COLOR_HIGHLIGHT, 1);
    }
}

// Render connecting screen
static void renderConnecting(void) {
    drawText(fontLarge, "Connecting...", SCREEN_W/2, SCREEN_H/2 - 80, COLOR_TEXT, 1);

    if (statusMsg[0]) {
        if (strstr(statusMsg, "PIN") != NULL) {
            drawText(fontLarge, statusMsg, SCREEN_W/2, SCREEN_H/2 + 20, COLOR_HIGHLIGHT, 1);
        } else {
            static const float dimColor[] = {0.5f, 0.5f, 0.5f, 1.0f};
            drawText(fontMedium, statusMsg, SCREEN_W/2, SCREEN_H/2 + 40, dimColor, 1);
        }
    }
}

// Render app list
static void renderAppList(void) {
    drawButton(&backBtn, fontMedium);
    drawText(fontLarge, "SELECT APP", SCREEN_W/2, 30, COLOR_TEXT, 1);

    // Separator
    drawRect(50, 80, SCREEN_W - 100, 2, COLOR_ACCENT);

    for (int i = 0; i < numAppBtns; i++) {
        drawButton(&appBtns[i], fontMedium);
    }

    if (numAppBtns == 0) {
        static const float dimColor[] = {0.5f, 0.5f, 0.5f, 1.0f};
        drawText(fontMedium, "No apps found", SCREEN_W/2, SCREEN_H/2, dimColor, 1);
    }
}

// Main render function
void webos_gui_render(SDL_Surface *unused) {
    (void)unused;  // Not used with GL rendering

    // Clear screen
    glClearColor(COLOR_BG[0], COLOR_BG[1], COLOR_BG[2], COLOR_BG[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    switch (guiState) {
        case GUI_STATE_MAIN:
            renderMainMenu();
            break;
        case GUI_STATE_CONNECTING:
            renderConnecting();
            break;
        case GUI_STATE_APPS:
            renderAppList();
            break;
    }
}

// Handle numpad input
static void handleNumpadPress(int btnId) {
    if (btnId >= 0 && btnId <= 8) {
        // Numbers 1-9
        if (ipCursorPos < 15) {
            serverIP[ipCursorPos++] = '1' + btnId;
            serverIP[ipCursorPos] = '\0';
        }
    } else if (btnId == 9) {
        // Dot
        if (ipCursorPos < 15 && ipCursorPos > 0) {
            serverIP[ipCursorPos++] = '.';
            serverIP[ipCursorPos] = '\0';
        }
    } else if (btnId == 10) {
        // 0
        if (ipCursorPos < 15) {
            serverIP[ipCursorPos++] = '0';
            serverIP[ipCursorPos] = '\0';
        }
    } else if (btnId == 11) {
        // Backspace
        if (ipCursorPos > 0) {
            serverIP[--ipCursorPos] = '\0';
        }
    } else if (btnId == 12) {
        // DEL - clear all
        serverIP[0] = '\0';
        ipCursorPos = 0;
    }
}

// Handle touch event
int webos_gui_handle_event(SDL_Event *event) {
    if (event->type != SDL_MOUSEBUTTONDOWN && event->type != SDL_MOUSEBUTTONUP) {
        return 0;
    }

    int x = event->button.x;
    int y = event->button.y;
    int pressed = (event->type == SDL_MOUSEBUTTONDOWN);

    if (guiState == GUI_STATE_MAIN) {
        for (int i = 0; i < 13; i++) {
            if (pointInButton(x, y, &numpadBtns[i])) {
                numpadBtns[i].pressed = pressed;
                if (!pressed) handleNumpadPress(i);
                return 1;
            }
        }

        if (pointInButton(x, y, &connectBtn)) {
            connectBtn.pressed = pressed;
            if (!pressed && serverIP[0]) {
                webos_config_set_server(serverIP);
                webos_config_save();
                return 2;
            }
            return 1;
        }
    }
    else if (guiState == GUI_STATE_APPS) {
        if (pointInButton(x, y, &backBtn)) {
            backBtn.pressed = pressed;
            if (!pressed) guiState = GUI_STATE_MAIN;
            return 1;
        }

        for (int i = 0; i < numAppBtns; i++) {
            if (pointInButton(x, y, &appBtns[i])) {
                appBtns[i].pressed = pressed;
                if (!pressed) {
                    selectedAppId = appBtns[i].id;
                    return 3;
                }
                return 1;
            }
        }
    }

    return 0;
}
