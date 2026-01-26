/*
 * Moonlight webOS - Audio Backend
 *
 * Uses SDL audio with Opus decoding for streaming audio.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include <opus/opus.h>
#include "Limelight.h"
#include "webos_platform.h"

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_FRAME_SIZE 240  // 5ms at 48kHz

static OpusDecoder *decoder = NULL;
static int channelCount = AUDIO_CHANNELS;

// Ring buffer for decoded audio (enough for ~200ms)
#define AUDIO_BUFFER_SAMPLES (AUDIO_SAMPLE_RATE / 5)  // 200ms
#define AUDIO_BUFFER_SIZE (AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS)
static short audioBuffer[AUDIO_BUFFER_SIZE];
static volatile int bufferReadPos = 0;
static volatile int bufferWritePos = 0;
static SDL_mutex *bufferMutex = NULL;

static int audioInitialized = 0;

// Get number of samples available in buffer
static int buffer_available(void) {
    int avail = bufferWritePos - bufferReadPos;
    if (avail < 0) avail += AUDIO_BUFFER_SIZE;
    return avail;
}

// SDL audio callback - called from audio thread
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    int samplesNeeded = len / sizeof(short);
    short *out = (short *)stream;

    SDL_LockMutex(bufferMutex);

    int available = buffer_available();

    if (available >= samplesNeeded) {
        // We have enough data
        for (int i = 0; i < samplesNeeded; i++) {
            out[i] = audioBuffer[bufferReadPos];
            bufferReadPos = (bufferReadPos + 1) % AUDIO_BUFFER_SIZE;
        }
    } else {
        // Not enough data - output what we have, pad with silence
        for (int i = 0; i < available; i++) {
            out[i] = audioBuffer[bufferReadPos];
            bufferReadPos = (bufferReadPos + 1) % AUDIO_BUFFER_SIZE;
        }
        // Fill rest with silence
        memset(out + available, 0, (samplesNeeded - available) * sizeof(short));
    }

    SDL_UnlockMutex(bufferMutex);
}

int webos_audio_init(void) {
    printf("Initializing audio...\n");

    bufferMutex = SDL_CreateMutex();
    if (!bufferMutex) {
        printf("Failed to create audio mutex\n");
        return -1;
    }

    // Don't open audio device yet - wait for Limelight to tell us the format
    printf("Audio pre-initialized\n");
    return 0;
}

void webos_audio_cleanup(void) {
    if (audioInitialized) {
        SDL_CloseAudio();
        audioInitialized = 0;
    }

    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = NULL;
    }

    if (bufferMutex) {
        SDL_DestroyMutex(bufferMutex);
        bufferMutex = NULL;
    }

    printf("Audio cleaned up\n");
}

// Limelight audio callbacks

static int audio_init_callback(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void *context, int flags) {
    int err;

    printf("Audio init: config=%d, channels=%d, streams=%d\n",
           audioConfiguration, opusConfig->channelCount, opusConfig->streams);

    channelCount = opusConfig->channelCount;

    // Create Opus decoder
    decoder = opus_decoder_create(AUDIO_SAMPLE_RATE, channelCount, &err);
    if (err != OPUS_OK || !decoder) {
        printf("Failed to create Opus decoder: %d\n", err);
        return -1;
    }

    // Reset buffer
    bufferReadPos = 0;
    bufferWritePos = 0;
    memset(audioBuffer, 0, sizeof(audioBuffer));

    // Open SDL audio device
    SDL_AudioSpec desired, obtained;
    memset(&desired, 0, sizeof(desired));
    desired.freq = AUDIO_SAMPLE_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = channelCount;
    desired.samples = 1024;  // Buffer size in samples
    desired.callback = audio_callback;
    desired.userdata = NULL;

    if (SDL_OpenAudio(&desired, &obtained) < 0) {
        printf("SDL_OpenAudio failed: %s\n", SDL_GetError());
        opus_decoder_destroy(decoder);
        decoder = NULL;
        return -1;
    }

    printf("Audio opened: %d Hz, %d channels, %d samples buffer\n",
           obtained.freq, obtained.channels, obtained.samples);

    audioInitialized = 1;

    // Start audio playback
    SDL_PauseAudio(0);

    return 0;
}

static void audio_cleanup_callback(void) {
    SDL_PauseAudio(1);

    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = NULL;
    }
}

static void audio_decode_and_play(unsigned char *data, int length) {
    if (!decoder) return;

    short pcmBuffer[AUDIO_FRAME_SIZE * AUDIO_CHANNELS * 2];  // Extra space for safety
    int samples;

    // Decode Opus to PCM
    samples = opus_decode(decoder, data, length, pcmBuffer, AUDIO_FRAME_SIZE * 2, 0);
    if (samples < 0) {
        // Decode error - try to decode with FEC or just skip
        return;
    }

    // Add to ring buffer
    SDL_LockMutex(bufferMutex);

    int samplesToWrite = samples * channelCount;
    int available = buffer_available();
    int freeSpace = AUDIO_BUFFER_SIZE - available - 1;

    if (samplesToWrite > freeSpace) {
        // Buffer is getting full - drop oldest samples to make room
        int toDrop = samplesToWrite - freeSpace;
        bufferReadPos = (bufferReadPos + toDrop) % AUDIO_BUFFER_SIZE;
    }

    // Write samples to buffer
    for (int i = 0; i < samplesToWrite; i++) {
        audioBuffer[bufferWritePos] = pcmBuffer[i];
        bufferWritePos = (bufferWritePos + 1) % AUDIO_BUFFER_SIZE;
    }

    SDL_UnlockMutex(bufferMutex);
}

AUDIO_RENDERER_CALLBACKS webos_audio_callbacks = {
    .init = audio_init_callback,
    .cleanup = audio_cleanup_callback,
    .decodeAndPlaySample = audio_decode_and_play,
    .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
