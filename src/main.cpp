#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "AudioTask.h"
#include "GeminiClient.h"

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n=== Tanuki Robot Starting ===");

    // 1. Init microphone (I2S RX only)
    setupI2S();

    // 2. Connect to WiFi
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // 3. Init Gemini REST client
    setupGemini();

    // 4. Start mic capture task on Core 1
    startAudioTasks();

    Serial.println("=== Ready! Speak to the robot ===");
}

void loop() {
    loopGemini();
    delay(1);
}