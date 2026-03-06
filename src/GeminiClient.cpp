#include "GeminiClient.h"
#include "AudioTask.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.hpp>

static const char* BASE_URL = "https://generativelanguage.googleapis.com/v1beta/";

// Shared state between AudioTask (Core 1) and loopGemini (Core 0)
static volatile bool   s_audioReady    = false;
static const uint8_t*  s_pendingBuf    = nullptr;
static size_t          s_pendingLen    = 0;
static TaskHandle_t    s_micTaskHandle = nullptr;

// -------------------------------------------------------
// Called from AudioTask (Core 1) — blocks until pipeline done
// -------------------------------------------------------
void onRecordingReady(const uint8_t* pcm16, size_t lenBytes) {
    s_pendingBuf    = pcm16;
    s_pendingLen    = lenBytes;
    s_micTaskHandle = xTaskGetCurrentTaskHandle();
    s_audioReady    = true;
    // Suspend this task until loopGemini() wakes it
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// -------------------------------------------------------
// Step 1 — POST audio to Flash model, get text back
// -------------------------------------------------------
// Build a minimal 44-byte WAV header + PCM into a single PSRAM buffer.
// The REST inlineData API only accepts encoded audio (WAV/MP3/FLAC etc.),
// NOT raw PCM. Wrapping in WAV is the simplest zero-dependency solution.
// -------------------------------------------------------
static uint8_t* buildWav(const uint8_t* pcm, size_t pcmLen, size_t& outLen) {
    outLen = 44 + pcmLen;
    uint8_t* wav = (uint8_t*)ps_malloc(outLen);
    if (!wav) return nullptr;

    uint32_t sampleRate  = 16000;
    uint16_t channels    = 1;
    uint16_t bitsPerSamp = 16;
    uint32_t byteRate    = sampleRate * channels * bitsPerSamp / 8;
    uint16_t blockAlign  = channels * bitsPerSamp / 8;

    // RIFF chunk
    memcpy(wav +  0, "RIFF", 4);
    uint32_t riffSize = (uint32_t)(36 + pcmLen);
    memcpy(wav +  4, &riffSize, 4);
    memcpy(wav +  8, "WAVE", 4);
    // fmt sub-chunk
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmtSize = 16; memcpy(wav + 16, &fmtSize, 4);
    uint16_t audioFmt = 1; memcpy(wav + 20, &audioFmt, 2);
    memcpy(wav + 22, &channels,    2);
    memcpy(wav + 24, &sampleRate,  4);
    memcpy(wav + 28, &byteRate,    4);
    memcpy(wav + 32, &blockAlign,  2);
    memcpy(wav + 34, &bitsPerSamp, 2);
    // data sub-chunk
    memcpy(wav + 36, "data", 4);
    uint32_t dataSize = (uint32_t)pcmLen;
    memcpy(wav + 40, &dataSize, 4);
    memcpy(wav + 44, pcm, pcmLen);
    return wav;
}

// -------------------------------------------------------
static bool stepAudioToText(const uint8_t* pcm, size_t len, char* outText, size_t outTextSize) {
    // Wrap raw PCM in a WAV container — required by the REST inlineData API.
    size_t wavLen = 0;
    uint8_t* wav = buildWav(pcm, len, wavLen);
    if (!wav) { Serial.println("[Gemini] ps_malloc fail (WAV buffer)"); return false; }

    // Base64-encode the WAV
    unsigned int encLen = encode_base64_length(wavLen);
    char* encoded = (char*)ps_malloc(encLen + 1);
    if (!encoded) { free(wav); Serial.println("[Gemini] ps_malloc fail (base64)"); return false; }
    encode_base64(wav, wavLen, (unsigned char*)encoded);
    free(wav);
    encoded[encLen] = '\0';

    // Build JSON body — text instruction + WAV inlineData
    size_t bodySize = encLen + 512;
    char*  body     = (char*)ps_malloc(bodySize);
    if (!body) { free(encoded); Serial.println("[Gemini] ps_malloc fail (body)"); return false; }
    snprintf(body, bodySize,
        "{\"contents\":[{\"parts\":["
          "{\"text\":\"You are a helpful voice assistant. Respond conversationally to what the user said.\"},"
          "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"%s\"}}"
        "]}]}",
        encoded);
    free(encoded);

    HTTPClient http;
    String url = String(BASE_URL) + GEMINI_MODEL_FLASH + ":generateContent?key=" + GEMINI_API_KEY;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(30000);

    Serial.printf("[Gemini] POST → %s%s:generateContent\n", BASE_URL, GEMINI_MODEL_FLASH);
    Serial.printf("[Gemini] Sending %d KB WAV (%d KB PCM)\n", wavLen / 1024, len / 1024);
    int code = http.POST((uint8_t*)body, strlen(body));
    free(body);

    if (code != 200) {
        Serial.printf("[Gemini] Flash API error: HTTP %d\n", code);
        Serial.println(http.getString());
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    // Parse "text": "..." — handle optional space after colon
    const char* raw     = response.c_str();
    const char* textPtr = strstr(raw, "\"text\":");
    if (!textPtr) {
        Serial.println("[Gemini] No text field in response:");
        Serial.println(response.substring(0, 400));
        return false;
    }
    textPtr += 7; // skip past "text":
    while (*textPtr == ' ') textPtr++;  // skip optional whitespace
    if (*textPtr != '"') { Serial.println("[Gemini] Unexpected text field format"); return false; }
    textPtr++; // skip opening quote
    const char* endPtr = strchr(textPtr, '"');
    if (!endPtr) return false;
    size_t tLen = min((size_t)(endPtr - textPtr), outTextSize - 1);
    memcpy(outText, textPtr, tLen);
    outText[tLen] = '\0';
    Serial.printf("[Gemini] Flash says: %s\n", outText);
    return true;
}

// -------------------------------------------------------
// Step 2 — POST text to TTS model, decode base64 PCM, play it
// -------------------------------------------------------
static void stepTextToSpeech(const char* text) {
    // Minimal JSON escape
    String escaped = String(text);
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");

    String body =
        "{\"contents\":[{\"parts\":[{\"text\":\"" + escaped + "\"}]}],"
        "\"generationConfig\":{"
          "\"responseModalities\":[\"AUDIO\"],"
          "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Puck\"}}}}}";

    HTTPClient http;
    String url = String(BASE_URL) + GEMINI_MODEL_TTS + ":generateContent?key=" + GEMINI_API_KEY;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(30000);

    Serial.println("[Gemini] Requesting TTS audio...");
    int code = http.POST(body);

    if (code != 200) {
        Serial.printf("[Gemini] TTS API error: HTTP %d\n", code);
        Serial.println(http.getString().substring(0, 400));
        http.end();
        return;
    }

    // Load the full JSON response into PSRAM
    int   totalSize = http.getSize();
    // If chunked (totalSize=-1), cap at 512 KB; otherwise use actual size + 1
    size_t bufSize = (totalSize > 0) ? (size_t)totalSize + 1 : 512 * 1024;
    char* respBuf = (char*)ps_malloc(bufSize);
    if (!respBuf) {
        Serial.println("[Gemini] ps_malloc fail (TTS response buffer)");
        http.end();
        return;
    }

    WiFiClient*   stream   = http.getStreamPtr();
    size_t        received = 0;
    unsigned long startMs  = millis();

    while (http.connected() && received < bufSize - 1) {
        if (millis() - startMs > 30000) { Serial.println("[Gemini] TTS read timeout."); break; }
        size_t avail = stream->available();
        if (!avail) { delay(1); continue; }
        size_t n = stream->readBytes(respBuf + received,
                                     min(avail, bufSize - 1 - received));
        received += n;
        if (totalSize > 0 && received >= (size_t)totalSize) break;
    }
    respBuf[received] = '\0';
    http.end();
    Serial.printf("[Gemini] TTS response loaded: %d bytes\n", received);

    // Locate the base64 audio payload inside "inlineData":{"mimeType":...,"data":"<b64>"}
    char* dataStart = strstr(respBuf, "\"data\":\"");
    if (!dataStart) {
        Serial.println("[Gemini] No audio data field in TTS response:");
        Serial.println(String(respBuf).substring(0, 400));
        free(respBuf);
        return;
    }
    dataStart += 8; // skip past "data":"

    char* dataEnd = strchr(dataStart, '"');
    if (!dataEnd) {
        Serial.println("[Gemini] TTS data field not terminated");
        free(respBuf);
        return;
    }
    *dataEnd = '\0'; // null-terminate so decode_base64 stops here

    // Decode base64 → raw PCM
    unsigned int pcmLen = decode_base64_length((unsigned char*)dataStart);
    uint8_t* pcmBuf = (uint8_t*)ps_malloc(pcmLen + 4);
    if (!pcmBuf) {
        Serial.println("[Gemini] ps_malloc fail (PCM decode buffer)");
        free(respBuf);
        return;
    }
    decode_base64((unsigned char*)dataStart, pcmBuf);
    free(respBuf);

    Serial.printf("[Gemini] TTS decoded: %d KB PCM — playing\n", pcmLen / 1024);
    playPCM(pcmBuf, pcmLen, SPEAKER_SAMPLE_RATE);
    free(pcmBuf);
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------
void setupGemini() {
    Serial.println("[Gemini] Hybrid REST client ready.");
    Serial.println("[Gemini] Pipeline: mic → Flash (STT/chat) → TTS model → audio.");
}

void loopGemini() {
    if (!s_audioReady) return;
    s_audioReady = false;

    char textBuf[512] = {};
    bool ok = stepAudioToText(s_pendingBuf, s_pendingLen, textBuf, sizeof(textBuf));
    if (ok && textBuf[0] != '\0') {
        stepTextToSpeech(textBuf);
    }

    // Wake the mic task so it can start the next recording
    if (s_micTaskHandle) {
        xTaskNotifyGive(s_micTaskHandle);
        s_micTaskHandle = nullptr;
    }
}
