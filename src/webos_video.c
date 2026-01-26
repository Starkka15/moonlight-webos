/*
 * Moonlight webOS - Video Rendering Backend
 *
 * Uses FFmpeg for H.264 decoding and OpenGL ES 2.0 for rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "PDL.h"
#include <GLES2/gl2.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

#include "Limelight.h"
#include "webos_platform.h"

static SDL_Surface *screen = NULL;
static int video_width = 1024;
static int video_height = 768;

// FFmpeg decoder state
static AVCodec *codec = NULL;
static AVCodecContext *codec_ctx = NULL;
static AVFrame *frame = NULL;
static AVPacket *packet = NULL;

// OpenGL ES resources
static GLuint program = 0;
static GLuint yTexture = 0, uTexture = 0, vTexture = 0;
static GLint positionLoc, texCoordLoc;
static GLint yTexLoc, uTexLoc, vTexLoc;

// Temporary buffers for texture upload (OpenGL ES doesn't support row stride)
static unsigned char *yBuffer = NULL;
static unsigned char *uBuffer = NULL;
static unsigned char *vBuffer = NULL;
static int bufferWidth = 0, bufferHeight = 0;

// Frame queue for thread-safe rendering (decoder thread -> main thread)
static unsigned char *pendingY = NULL;
static unsigned char *pendingU = NULL;
static unsigned char *pendingV = NULL;
static volatile int frameReady = 0;
static SDL_mutex *frameMutex = NULL;

// Shader for solid color overlay (gamepad buttons)
static const char *overlayVertexSrc =
    "attribute vec4 position;\n"
    "void main() {\n"
    "    gl_Position = position;\n"
    "}\n";

static const char *overlayFragmentSrc =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "    gl_FragColor = uColor;\n"
    "}\n";

static GLuint overlayProgram = 0;
static GLint overlayPosLoc, overlayColorLoc;

// Simple vertex and fragment shaders for YUV to RGB conversion
static const char *vertexShaderSrc =
    "attribute vec4 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = position;\n"
    "    vTexCoord = texCoord;\n"
    "}\n";

static const char *fragmentShaderSrc =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D yTex;\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D vTex;\n"
    "void main() {\n"
    "    float y = texture2D(yTex, vTexCoord).r;\n"
    "    float u = texture2D(uTex, vTexCoord).r - 0.5;\n"
    "    float v = texture2D(vTex, vTexCoord).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344 * u - 0.714 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static int setup_gl(void) {
    printf("setup_gl: compiling shaders\n"); fflush(stdout);
    GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragmentShaderSrc);

    if (!vertexShader || !fragmentShader) {
        return -1;
    }

    program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        printf("Shader link error\n");
        return -1;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Compile overlay shader (for gamepad buttons)
    GLuint ovVert = compile_shader(GL_VERTEX_SHADER, overlayVertexSrc);
    GLuint ovFrag = compile_shader(GL_FRAGMENT_SHADER, overlayFragmentSrc);
    if (ovVert && ovFrag) {
        overlayProgram = glCreateProgram();
        glAttachShader(overlayProgram, ovVert);
        glAttachShader(overlayProgram, ovFrag);
        glLinkProgram(overlayProgram);
        glDeleteShader(ovVert);
        glDeleteShader(ovFrag);

        overlayPosLoc = glGetAttribLocation(overlayProgram, "position");
        overlayColorLoc = glGetUniformLocation(overlayProgram, "uColor");
        printf("Overlay shader ready: pos=%d color=%d\n", overlayPosLoc, overlayColorLoc);
    }

    // Get attribute/uniform locations
    positionLoc = glGetAttribLocation(program, "position");
    texCoordLoc = glGetAttribLocation(program, "texCoord");
    yTexLoc = glGetUniformLocation(program, "yTex");
    uTexLoc = glGetUniformLocation(program, "uTex");
    vTexLoc = glGetUniformLocation(program, "vTex");

    printf("Shader locations: pos=%d tex=%d yTex=%d uTex=%d vTex=%d\n",
           positionLoc, texCoordLoc, yTexLoc, uTexLoc, vTexLoc);
    fflush(stdout);

    // Create textures
    glGenTextures(1, &yTexture);
    glGenTextures(1, &uTexture);
    glGenTextures(1, &vTexture);

    // Setup Y texture
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Setup U texture
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Setup V texture
    glBindTexture(GL_TEXTURE_2D, vTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    return 0;
}

static void check_gl_error(const char *op) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("GL error at %s: 0x%x\n", op, err);
        fflush(stdout);
    }
}

// Early OpenGL initialization - call this before GUI
int webos_video_early_init(void) {
    printf("Early video init (OpenGL)...\n");
    fflush(stdout);

    // Set GL attributes for ES 2.0
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Create fullscreen OpenGL window
    screen = SDL_SetVideoMode(1024, 768, 0, SDL_OPENGL | SDL_FULLSCREEN);
    if (!screen) {
        printf("SDL_SetVideoMode (GL) failed: %s\n", SDL_GetError());
        return -1;
    }
    printf("OpenGL surface created: %dx%d\n", screen->w, screen->h);

    video_width = screen->w;
    video_height = screen->h;

    // Set viewport
    glViewport(0, 0, video_width, video_height);

    // Print GL info
    printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    fflush(stdout);

    return 0;
}

int webos_video_init(void) {
    printf("Initializing video decoder...\n");
    fflush(stdout);

    // GL context should already exist from early_init
    if (!screen) {
        printf("Error: GL context not initialized. Call webos_video_early_init first.\n");
        return -1;
    }

    printf("Got surface: %dx%d, pitch=%d, flags=0x%x\n",
           screen->w, screen->h, screen->pitch, screen->flags);

    video_width = screen->w;
    video_height = screen->h;
    printf("Video mode: %dx%d\n", video_width, video_height);

    // Set viewport to full screen
    glViewport(0, 0, video_width, video_height);
    printf("Viewport set to %dx%d\n", video_width, video_height);

    // Print GL info
    printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    fflush(stdout);

    // Setup OpenGL ES
    if (setup_gl() != 0) {
        printf("OpenGL ES setup failed\n");
        return -1;
    }

    // Create mutex for frame queue
    frameMutex = SDL_CreateMutex();
    if (!frameMutex) {
        printf("Failed to create frame mutex\n");
        return -1;
    }

    printf("Video initialized\n");
    return 0;
}

void webos_video_cleanup(void) {
    if (frame) {
        av_frame_free(&frame);
        frame = NULL;
    }
    if (packet) {
        av_packet_free(&packet);
        packet = NULL;
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
        codec_ctx = NULL;
    }

    free(yBuffer); yBuffer = NULL;
    free(uBuffer); uBuffer = NULL;
    free(vBuffer); vBuffer = NULL;
    free(pendingY); pendingY = NULL;
    free(pendingU); pendingU = NULL;
    free(pendingV); pendingV = NULL;
    bufferWidth = bufferHeight = 0;

    if (frameMutex) {
        SDL_DestroyMutex(frameMutex);
        frameMutex = NULL;
    }

    if (yTexture) glDeleteTextures(1, &yTexture);
    if (uTexture) glDeleteTextures(1, &uTexture);
    if (vTexture) glDeleteTextures(1, &vTexture);

    if (program) glDeleteProgram(program);

    printf("Video cleaned up\n");
}

// Called from main thread to render any pending frames
static int texturesAllocated = 0;

void webos_video_render(void) {
    if (!frameMutex || !frameReady) {
        return;
    }

    SDL_LockMutex(frameMutex);
    if (!frameReady) {
        SDL_UnlockMutex(frameMutex);
        return;
    }

    int width = bufferWidth;
    int height = bufferHeight;
    int uvWidth = width / 2;
    int uvHeight = height / 2;

    // Copy from pending buffers to render buffers
    memcpy(yBuffer, pendingY, width * height);
    memcpy(uBuffer, pendingU, uvWidth * uvHeight);
    memcpy(vBuffer, pendingV, uvWidth * uvHeight);
    frameReady = 0;

    SDL_UnlockMutex(frameMutex);

    // Allocate textures on first frame (must be on main thread)
    if (!texturesAllocated && width > 0 && height > 0) {
        printf("Allocating textures on main thread: %dx%d\n", width, height);
        fflush(stdout);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, yTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uvWidth, uvHeight, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uvWidth, uvHeight, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            printf("GL error after texture allocation: 0x%x\n", err);
        }
        printf("Textures allocated (main thread)\n");
        fflush(stdout);

        texturesAllocated = 1;
    }

    // Upload texture data
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, yBuffer);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, uBuffer);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, vTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, vBuffer);

    // Render with correct aspect ratio (letterboxing)
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    // Calculate aspect-correct quad coordinates
    // Video: 854x480 (16:9 = 1.779), Screen: 1024x768 (4:3 = 1.333)
    float videoAspect = (float)width / (float)height;
    float screenAspect = 1024.0f / 768.0f;

    float quadW = 1.0f, quadH = 1.0f;
    if (videoAspect > screenAspect) {
        // Video is wider - fit to width, letterbox top/bottom
        quadH = screenAspect / videoAspect;
    } else {
        // Video is taller - fit to height, pillarbox left/right
        quadW = videoAspect / screenAspect;
    }

    float vertices[] = {
        -quadW, -quadH,
         quadW, -quadH,
        -quadW,  quadH,
         quadW,  quadH,
    };

    static const float texCoords[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
    };

    glVertexAttribPointer(positionLoc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(positionLoc);

    glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(texCoordLoc);

    glUniform1i(yTexLoc, 0);
    glUniform1i(uTexLoc, 1);
    glUniform1i(vTexLoc, 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Draw gamepad overlay if enabled
    if (webos_gamepad_enabled() && overlayProgram) {
        glUseProgram(overlayProgram);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Get button regions from gamepad
        int rx[16], ry[16], rw[16], rh[16], rbtn[16];
        int count = webos_gamepad_get_regions(rx, ry, rw, rh, rbtn, 16);

        // Use actual screen dimensions (1024x768) for overlay, not video dimensions
        float sw = 1024.0f;
        float sh = 768.0f;

        for (int i = 0; i < count; i++) {
            // Convert screen coords to GL coords (-1 to 1)
            float x1 = (rx[i] / sw) * 2.0f - 1.0f;
            float y1 = 1.0f - ((ry[i] + rh[i]) / sh) * 2.0f;  // Flip Y
            float x2 = ((rx[i] + rw[i]) / sw) * 2.0f - 1.0f;
            float y2 = 1.0f - (ry[i] / sh) * 2.0f;

            float verts[] = {
                x1, y1,
                x2, y1,
                x1, y2,
                x2, y2,
            };

            glVertexAttribPointer(overlayPosLoc, 2, GL_FLOAT, GL_FALSE, 0, verts);
            glEnableVertexAttribArray(overlayPosLoc);

            // Semi-transparent white buttons
            glUniform4f(overlayColorLoc, 1.0f, 1.0f, 1.0f, 0.3f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        // Draw analog stick areas
        int lsX, lsY, rsX, rsY, stickR;
        webos_gamepad_get_sticks(&lsX, &lsY, &rsX, &rsY, &stickR);
        float lsRf = (float)stickR;

        // Left stick
        float lx1 = ((lsX - stickR) / sw) * 2.0f - 1.0f;
        float ly1 = 1.0f - ((lsY + stickR) / sh) * 2.0f;
        float lx2 = ((lsX + stickR) / sw) * 2.0f - 1.0f;
        float ly2 = 1.0f - ((lsY - stickR) / sh) * 2.0f;
        float lVerts[] = { lx1, ly1, lx2, ly1, lx1, ly2, lx2, ly2 };
        glVertexAttribPointer(overlayPosLoc, 2, GL_FLOAT, GL_FALSE, 0, lVerts);
        glUniform4f(overlayColorLoc, 0.5f, 0.5f, 1.0f, 0.25f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Right stick
        float srx1 = ((rsX - stickR) / sw) * 2.0f - 1.0f;
        float sry1 = 1.0f - ((rsY + stickR) / sh) * 2.0f;
        float srx2 = ((rsX + stickR) / sw) * 2.0f - 1.0f;
        float sry2 = 1.0f - ((rsY - stickR) / sh) * 2.0f;
        float rVerts[] = { srx1, sry1, srx2, sry1, srx1, sry2, srx2, sry2 };
        glVertexAttribPointer(overlayPosLoc, 2, GL_FLOAT, GL_FALSE, 0, rVerts);
        glUniform4f(overlayColorLoc, 1.0f, 0.5f, 0.5f, 0.25f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisable(GL_BLEND);
    }

    SDL_GL_SwapBuffers();
}

// Render a decoded frame
static void render_frame(AVFrame *decoded_frame) {
    int width = decoded_frame->width;
    int height = decoded_frame->height;
    int uvWidth = width / 2;
    int uvHeight = height / 2;

    // Debug: print frame info every 30 frames
    static int debug_counter = 0;
    static int test_mode = 1;  // Start with test pattern

    if (debug_counter++ % 30 == 0) {
        printf("render_frame: %dx%d, format=%d, linesize=[%d,%d,%d], data=[%p,%p,%p]\n",
               width, height, decoded_frame->format,
               decoded_frame->linesize[0], decoded_frame->linesize[1], decoded_frame->linesize[2],
               decoded_frame->data[0], decoded_frame->data[1], decoded_frame->data[2]);
        // Sample first few Y values
        if (decoded_frame->data[0]) {
            printf("Y sample: %d %d %d %d\n",
                   decoded_frame->data[0][0], decoded_frame->data[0][1],
                   decoded_frame->data[0][2], decoded_frame->data[0][3]);
        }
        fflush(stdout);

        // After 90 frames, switch to real video
        if (debug_counter > 90) test_mode = 0;
    }

    // TEST: Fill with gradient pattern for first 90 frames to test GL pipeline
    if (test_mode) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // Gradient: black at top, white at bottom
                yBuffer[y * width + x] = (unsigned char)((y * 255) / height);
            }
        }
        // Neutral chroma (gray)
        memset(uBuffer, 128, (width/2) * (height/2));
        memset(vBuffer, 128, (width/2) * (height/2));

        if (debug_counter == 1) {
            printf("TEST MODE: Rendering gradient pattern\n");
            fflush(stdout);
        }
    } else {

        // Validate frame data
        if (!decoded_frame->data[0] || !decoded_frame->data[1] || !decoded_frame->data[2]) {
            printf("ERROR: NULL frame data pointers!\n");
            return;
        }

        // Copy Y plane (handle stride)
        if (decoded_frame->linesize[0] == width) {
            memcpy(yBuffer, decoded_frame->data[0], width * height);
        } else {
            for (int y = 0; y < height; y++) {
                memcpy(yBuffer + y * width, decoded_frame->data[0] + y * decoded_frame->linesize[0], width);
            }
        }

        // Copy U plane
        if (decoded_frame->linesize[1] == uvWidth) {
            memcpy(uBuffer, decoded_frame->data[1], uvWidth * uvHeight);
        } else {
            for (int y = 0; y < uvHeight; y++) {
                memcpy(uBuffer + y * uvWidth, decoded_frame->data[1] + y * decoded_frame->linesize[1], uvWidth);
            }
        }

        // Copy V plane
        if (decoded_frame->linesize[2] == uvWidth) {
            memcpy(vBuffer, decoded_frame->data[2], uvWidth * uvHeight);
        } else {
            for (int y = 0; y < uvHeight; y++) {
                memcpy(vBuffer + y * uvWidth, decoded_frame->data[2] + y * decoded_frame->linesize[2], uvWidth);
            }
        }
    }  // end of else (real video mode)

    // Clear any pending errors
    glGetError();

    // Set pixel unpack alignment to 1 (our data is byte-aligned, not 4-byte aligned)
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Update textures with new frame data (textures pre-allocated in decoder_setup)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, yBuffer);
    if (debug_counter % 30 == 1) check_gl_error("Y texSubImage");

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, uBuffer);
    if (debug_counter % 30 == 1) check_gl_error("U texSubImage");

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, vTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, vBuffer);
    if (debug_counter % 30 == 1) check_gl_error("V texSubImage");

    // DEBUG: Try rendering just a solid color first
    static int solid_test = 1;
    if (solid_test && debug_counter < 60) {
        // Just clear to bright red - no textures, no shaders
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapBuffers();
        if (debug_counter == 1) {
            printf("SOLID RED TEST - if you don't see red, GL context is broken\n");
            fflush(stdout);
        }
        return;
    }

    // Render fullscreen quad
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    static const float vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    static const float texCoords[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
    };

    glVertexAttribPointer(positionLoc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(positionLoc);

    glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(texCoordLoc);

    glUniform1i(yTexLoc, 0);
    glUniform1i(uTexLoc, 1);
    glUniform1i(vTexLoc, 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (debug_counter % 30 == 1) check_gl_error("drawArrays");

    SDL_GL_SwapBuffers();
    if (debug_counter % 30 == 1) check_gl_error("swapBuffers");

    // Sync to prevent GPU command queue overflow
    glFinish();
}

// Limelight callbacks

static int decoder_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
    printf("Decoder setup: %dx%d @ %d Hz, format %d\n", width, height, redrawRate, videoFormat);
    fflush(stdout);

    video_width = width;
    video_height = height;

    // Pre-allocate CPU buffers for YUV data (decoder thread)
    yBuffer = malloc(width * height);
    uBuffer = malloc(width * height / 4);
    vBuffer = malloc(width * height / 4);

    // Allocate pending frame buffers (for thread-safe queueing)
    pendingY = malloc(width * height);
    pendingU = malloc(width * height / 4);
    pendingV = malloc(width * height / 4);

    bufferWidth = width;
    bufferHeight = height;

    if (!yBuffer || !uBuffer || !vBuffer || !pendingY || !pendingU || !pendingV) {
        printf("Failed to allocate YUV buffers\n");
        return -1;
    }

    printf("CPU buffers allocated for %dx%d\n", width, height);
    fflush(stdout);

    // NOTE: GL texture allocation moved to main thread (webos_video_render)
    // because GL context is thread-bound

    // Find H.264 decoder
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("H.264 codec not found!\n");
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        printf("Failed to allocate codec context\n");
        return -1;
    }

    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Low latency settings
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("Failed to open codec\n");
        return -1;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !packet) {
        printf("Failed to allocate frame/packet\n");
        return -1;
    }

    printf("FFmpeg H.264 decoder initialized\n");
    return 0;
}

static void decoder_cleanup(void) {
    printf("Decoder cleanup\n");
}

static int frames_decoded = 0;

static int decoder_submit_decode_unit(PDECODE_UNIT decodeUnit) {
    if (!codec_ctx || !frame || !packet) {
        return DR_OK;
    }

    // Build packet from decode unit buffers
    PLENTRY entry = decodeUnit->bufferList;
    int total_size = 0;

    // Calculate total size
    while (entry != NULL) {
        total_size += entry->length;
        entry = entry->next;
    }

    // Allocate packet data
    if (av_new_packet(packet, total_size) < 0) {
        return DR_OK;
    }

    // Copy data from all buffers
    entry = decodeUnit->bufferList;
    int offset = 0;
    while (entry != NULL) {
        memcpy(packet->data + offset, entry->data, entry->length);
        offset += entry->length;
        entry = entry->next;
    }

    // Send packet to decoder
    int ret = avcodec_send_packet(codec_ctx, packet);
    av_packet_unref(packet);

    if (ret < 0) {
        return DR_OK;
    }

    // Receive decoded frames
    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            break;
        }

        // Queue the decoded frame for rendering on main thread
        // (GL context is thread-bound, so we can't render here)
        SDL_LockMutex(frameMutex);

        int width = frame->width;
        int height = frame->height;
        int uvWidth = width / 2;
        int uvHeight = height / 2;

        // Copy Y plane
        if (frame->linesize[0] == width) {
            memcpy(pendingY, frame->data[0], width * height);
        } else {
            for (int y = 0; y < height; y++) {
                memcpy(pendingY + y * width, frame->data[0] + y * frame->linesize[0], width);
            }
        }

        // Copy U plane
        if (frame->linesize[1] == uvWidth) {
            memcpy(pendingU, frame->data[1], uvWidth * uvHeight);
        } else {
            for (int y = 0; y < uvHeight; y++) {
                memcpy(pendingU + y * uvWidth, frame->data[1] + y * frame->linesize[1], uvWidth);
            }
        }

        // Copy V plane
        if (frame->linesize[2] == uvWidth) {
            memcpy(pendingV, frame->data[2], uvWidth * uvHeight);
        } else {
            for (int y = 0; y < uvHeight; y++) {
                memcpy(pendingV + y * uvWidth, frame->data[2] + y * frame->linesize[2], uvWidth);
            }
        }

        frameReady = 1;
        SDL_UnlockMutex(frameMutex);

        frames_decoded++;
        av_frame_unref(frame);
    }

    return DR_OK;
}

DECODER_RENDERER_CALLBACKS webos_video_callbacks = {
    .setup = decoder_setup,
    .cleanup = decoder_cleanup,
    .submitDecodeUnit = decoder_submit_decode_unit,
};
