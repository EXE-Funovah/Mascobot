#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "AudioTask.h"
#include "GeminiClient.h"

static bool waitForWifiConnection(uint32_t timeoutMs) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutMs)) {
        delay(500);
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    return true;
}

static bool waitForOnline(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        IPAddress resolved;
        int dnsResult = WiFi.hostByName(AI_WS_HOST, resolved);
        if (dnsResult == 1) {
            return true;
        }
        delay(1000);
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    Serial.println("[BOOT] Serial started at 115200");
    delay(1500);
    Serial.println("\n=== Tanuki Robot Starting ===");

    // 1. Init microphone (I2S RX only)
    setupI2S();

    // 2. Connect to WiFi and verify internet reachability before startup.
    bool wifiOk = waitForWifiConnection(30000);
    bool onlineOk = wifiOk ? waitForOnline(15000) : false;

    if (!wifiOk || !onlineOk) {
        wifiOk = waitForWifiConnection(30000);
        onlineOk = wifiOk ? waitForOnline(15000) : false;
    }

    if (!wifiOk || !onlineOk) {
        Serial.println("[Setup] Still offline. Gemini init may fail until network recovers.");
    }
    // 3. Init Mascoteach live mascot client (WebSocket)
    setupGemini();

    // 4. Start mic capture task on Core 1
    startAudioTasks();

    Serial.println("=== Ready! Speak to the robot ===");
}

void loop() {
    loopGemini();
    delay(1);
}