#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include <Arduino.h>
#include "driver/i2s.h"
#include "Config.h"

// Initialise the I2S peripheral for the INMP441 microphone (RX only).
void setupI2S();

// Start speaker TX mode for streamed PCM playback.
// Safe to call only while the mic task is blocked.
bool beginSpeakerPlayback(uint32_t sampleRate);

// Write a chunk of raw PCM16 data to speaker TX.
// Returns bytes written (may be less than lenBytes if driver times out).
size_t writeSpeakerPCM(const uint8_t* pcm, size_t lenBytes);

// Stop speaker TX mode and restore microphone RX mode.
void endSpeakerPlayback();

// Play raw 16-bit PCM through the MAX98357A speaker.
// Tears down mic I2S, installs TX driver, plays, then restores mic.
// Safe to call only while the mic task is blocked (i.e. from loopGemini).
void playPCM(const uint8_t* pcm, size_t lenBytes, uint32_t sampleRate);

// Start the FreeRTOS mic-capture task on Core 1.
void startAudioTasks();

#endif