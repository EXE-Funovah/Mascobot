#ifndef GEMINI_CLIENT_H
#define GEMINI_CLIENT_H

#include <Arduino.h>
#include "Config.h"

// Initialise the Gemini REST client.
void setupGemini();

// Call from loop() on Core 0. Processes a pending recording when ready.
void loopGemini();

// Called by AudioTask (Core 1) when a full recording buffer is ready.
// BLOCKS the calling task until the HTTP pipeline is complete.
void onRecordingReady(const uint8_t* pcm16, size_t lenBytes);

#endif