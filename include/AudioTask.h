#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include <Arduino.h>
#include "driver/i2s.h"
#include "Config.h"

// Initialise the I2S peripheral for the INMP441 microphone (RX only).
void setupI2S();

// Play raw 16-bit PCM through the MAX98357A speaker.
// Tears down mic I2S, installs TX driver, plays, then restores mic.
// Safe to call only while the mic task is blocked (i.e. from loopGemini).
void playPCM(const uint8_t* pcm, size_t lenBytes, uint32_t sampleRate);

// Start the FreeRTOS mic-capture task on Core 1.
void startAudioTasks();

#endif