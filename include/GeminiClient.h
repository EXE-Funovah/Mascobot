#ifndef GEMINI_CLIENT_H
#define GEMINI_CLIENT_H

#include <Arduino.h>
#include "Config.h"

// Initialize the Mascoteach Live WebSocket client.
void setupGemini();

// Call from loop() on Core 0. Maintains WS state and processes pending recordings.
void loopGemini();

// Called by AudioTask (Core 1) when a full recording buffer is ready.
// Blocks the calling task until one model turn is finished.
void onRecordingReady(const uint8_t* pcm16, size_t lenBytes);

#endif