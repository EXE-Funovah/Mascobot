#include "GeminiClient.h"
#include "AudioTask.h"
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <base64.hpp>

static const uint32_t WS_CONNECT_WAIT_MS   = 10000;
static const uint32_t LIVE_READY_WAIT_MS   = 10000;
static const uint32_t TURN_TIMEOUT_MS      = 30000;
static const uint32_t TURN_NUDGE_AFTER_MS  = 2500;
static const uint32_t TURN_SILENCE_TAIL_MS = 500;
static const size_t   TX_CHUNK_BYTES       = 1024;
static const size_t   MAX_RX_AUDIO_BYTES   = 512 * 1024;
static const bool     USE_TEXT_NUDGE       = false;
static const bool     STREAM_RX_AUDIO      = true;

// Shared state between AudioTask (Core 1) and loopGemini (Core 0)
static volatile bool   s_audioReady    = false;
static const uint8_t* s_pendingBuf    = nullptr;
static size_t          s_pendingLen    = 0;
static TaskHandle_t    s_micTaskHandle = nullptr;

static WebSocketsClient s_ws;
static bool s_wsTransportConnected = false;
static bool s_liveSessionConnected = false;
static bool s_waitingForTurn = false;
static bool s_turnComplete = false;
static bool s_disconnectedDuringTurn = false;
static String s_lastError = "none";

static uint8_t* s_rxAudio = nullptr;
static size_t s_rxAudioLen = 0;
static size_t s_rxAudioCap = 0;
static uint32_t s_rxAudioChunks = 0;
static bool s_rxAudioTruncated = false;
static bool s_rxStreamStarted = false;
static size_t s_rxStreamBytes = 0;

static uint8_t* s_decodeScratch = nullptr;
static size_t s_decodeScratchCap = 0;

static void sendHelloHandshake();

static void setLastError(const String& reason) {
    s_lastError = reason;
    Serial.printf("[MascotWS][ERR] %s\n", s_lastError.c_str());
}

static void logState(const char* phase) {
    Serial.printf(
        "[MascotWS][STATE] phase=%s ws=%d live=%d waiting=%d rxKB=%d chunks=%lu lastErr=%s\n",
        phase,
        (int)s_wsTransportConnected,
        (int)s_liveSessionConnected,
        (int)s_waitingForTurn,
        (int)(s_rxAudioLen / 1024),
        (unsigned long)s_rxAudioChunks,
        s_lastError.c_str()
    );
}

static void clearRxAudio() {
    if (s_rxAudio) {
        free(s_rxAudio);
        s_rxAudio = nullptr;
    }
    s_rxAudioLen = 0;
    s_rxAudioCap = 0;
    s_rxAudioChunks = 0;
    s_rxAudioTruncated = false;
    s_rxStreamStarted = false;
    s_rxStreamBytes = 0;
}

static bool reserveDecodeScratch(size_t needed) {
    if (needed <= s_decodeScratchCap) return true;

    size_t newCap = s_decodeScratchCap == 0 ? 4096 : s_decodeScratchCap;
    while (newCap < needed) {
        newCap *= 2;
    }

    uint8_t* next = (uint8_t*)ps_malloc(newCap);
    if (!next) return false;

    if (s_decodeScratch) {
        free(s_decodeScratch);
    }

    s_decodeScratch = next;
    s_decodeScratchCap = newCap;
    return true;
}

static bool streamRxAudioToSpeaker(const uint8_t* data, size_t len) {
    if (!STREAM_RX_AUDIO || !data || len == 0) return false;

    if (!s_rxStreamStarted) {
        if (!beginSpeakerPlayback(SPEAKER_SAMPLE_RATE)) {
            setLastError("speaker_stream_begin_failed");
            return false;
        }
        s_rxStreamStarted = true;
        Serial.println("[MascotWS] Streaming model audio to speaker...");
    }

    size_t offset = 0;
    while (offset < len) {
        size_t written = writeSpeakerPCM(data + offset, len - offset);
        if (written == 0) {
            setLastError("speaker_stream_write_short");
            return false;
        }
        offset += written;
    }

    s_rxStreamBytes += len;
    s_rxAudioChunks++;
    if ((s_rxAudioChunks % 8) == 0) {
        Serial.printf("[MascotWS] RX stream chunks=%lu total=%d KB\n",
                      (unsigned long)s_rxAudioChunks,
                      (int)(s_rxStreamBytes / 1024));
    }

    return true;
}

static void finishRxAudioStreamIfNeeded() {
    if (!s_rxStreamStarted) return;
    endSpeakerPlayback();
    s_rxStreamStarted = false;
}

static bool reserveRxAudio(size_t needed) {
    if (needed <= s_rxAudioCap) return true;

    size_t newCap = s_rxAudioCap == 0 ? 8192 : s_rxAudioCap;
    while (newCap < needed) {
        newCap *= 2;
        if (newCap > MAX_RX_AUDIO_BYTES) {
            newCap = MAX_RX_AUDIO_BYTES;
            break;
        }
    }

    if (newCap < needed) return false;

    uint8_t* next = (uint8_t*)ps_malloc(newCap);
    if (!next) return false;

    if (s_rxAudio && s_rxAudioLen > 0) {
        memcpy(next, s_rxAudio, s_rxAudioLen);
        free(s_rxAudio);
    }
    s_rxAudio = next;
    s_rxAudioCap = newCap;
    return true;
}

static bool appendRawRxAudio(const uint8_t* data, size_t len) {
    if (!data || len == 0) return true;

    bool streamWasActive = s_rxStreamStarted;
    if (streamRxAudioToSpeaker(data, len)) return true;

    // If streaming was already active, do not fall back to buffering this chunk,
    // otherwise partially written audio could be played twice.
    if (streamWasActive || s_rxStreamStarted) {
        return false;
    }

    if (s_rxAudioLen >= MAX_RX_AUDIO_BYTES || (MAX_RX_AUDIO_BYTES - s_rxAudioLen) < len) {
        if (!s_rxAudioTruncated) {
            s_rxAudioTruncated = true;
            setLastError("rx_audio_truncated_at_cap");
            Serial.printf("[MascotWS] RX audio capped at %d KB. Extra audio will be dropped this turn.\n",
                          (int)(MAX_RX_AUDIO_BYTES / 1024));
        }
        return false;
    }

    if (!reserveRxAudio(s_rxAudioLen + len)) {
        if (!s_rxAudioTruncated) {
            s_rxAudioTruncated = true;
            setLastError("rx_audio_buffer_oom");
        }
        return false;
    }

    memcpy(s_rxAudio + s_rxAudioLen, data, len);
    s_rxAudioLen += len;
    s_rxAudioChunks++;
    return true;
}

static void handleServerMessage(const char* payload, size_t len) {
    if (len > 0) {
        size_t snippetLen = min((size_t)180, len);
        Serial.printf("[MascotWS] <- %.*s%s\n",
                      (int)snippetLen,
                      payload,
                      (len > snippetLen) ? "..." : "");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        setLastError(String("json_parse:") + err.c_str());
        return;
    }

    const char* type = doc["type"] | "";
    if (strcmp(type, "connected") == 0) {
        s_liveSessionConnected = true;
        Serial.println("[MascotWS] Live session connected.");
        return;
    }

    if (strcmp(type, "audio") == 0) {
        const char* b64 = doc["data"] | "";
        size_t b64Len = strlen(b64);
        if (b64Len == 0) return;

        unsigned int decodedLen = decode_base64_length((const unsigned char*)b64, (unsigned int)b64Len);
        if (decodedLen == 0) return;

        if (!reserveDecodeScratch(decodedLen)) {
            setLastError("rx_decode_scratch_oom");
            return;
        }

        unsigned int n = decode_base64(
            (const unsigned char*)b64,
            (unsigned int)b64Len,
            s_decodeScratch
        );
        appendRawRxAudio(s_decodeScratch, n);
        return;
    }

    if (strcmp(type, "turn_complete") == 0) {
        s_turnComplete = true;
        Serial.printf("[MascotWS] Turn complete (streamed=%d KB buffered=%d KB).\n",
                      (int)(s_rxStreamBytes / 1024),
                      (int)(s_rxAudioLen / 1024));
        return;
    }

    if (strcmp(type, "interrupted") == 0) {
        // Match web behavior: clear queued model audio when interrupted.
        s_turnComplete = true;
        finishRxAudioStreamIfNeeded();
        clearRxAudio();
        Serial.println("[MascotWS] Turn interrupted.");
        return;
    }

    if (strcmp(type, "error") == 0) {
        const char* message = doc["message"] | "Unknown error";
        setLastError(String("server_error:") + message);
        s_turnComplete = true;
        return;
    }

    if (strcmp(type, "session_ended") == 0) {
        s_liveSessionConnected = false;
        Serial.println("[MascotWS] Live session ended by server.");
        return;
    }

    Serial.printf("[MascotWS] Unhandled message type: %s\n", type);
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            if (payload && length > 0) {
                Serial.printf("[MascotWS] Transport disconnected. reason=%.*s\n", (int)length, (const char*)payload);
            } else {
                Serial.println("[MascotWS] Transport disconnected.");
            }
            s_wsTransportConnected = false;
            s_liveSessionConnected = false;
            setLastError("ws_transport_disconnected");
            if (s_waitingForTurn) {
                s_disconnectedDuringTurn = true;
                s_turnComplete = true;
            }
            break;

        case WStype_CONNECTED:
            Serial.printf("[MascotWS] Transport connected: %s\n", payload ? (char*)payload : "");
            s_wsTransportConnected = true;
            s_liveSessionConnected = false;
            sendHelloHandshake();
            break;

        case WStype_TEXT:
            handleServerMessage((const char*)payload, length);
            break;

        case WStype_BIN:
            if (payload && length > 0) {
                if (appendRawRxAudio(payload, length)) {
                    if ((s_rxAudioChunks % 8) == 0) {
                        Serial.printf("[MascotWS] RX binary chunks=%lu total(stream=%d KB, buffer=%d KB)\n",
                                      (unsigned long)s_rxAudioChunks,
                                      (int)(s_rxStreamBytes / 1024),
                                      (int)(s_rxAudioLen / 1024));
                    }
                }
            }
            break;

        case WStype_ERROR:
            Serial.println("[MascotWS] Transport error.");
            if (payload && length > 0) {
                Serial.printf("[MascotWS] Error payload: %.*s\n", (int)length, (const char*)payload);
                setLastError(String("ws_transport_error:") + String((const char*)payload));
            } else {
                setLastError("ws_transport_error");
            }
            break;

        default:
            break;
    }
}

static bool ensureLiveSession(uint32_t timeoutMs) {
    if (s_liveSessionConnected) return true;

    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        s_ws.loop();
        if (s_liveSessionConnected) return true;
        delay(2);
    }

    setLastError("live_session_connect_timeout");
    logState("ensureLiveSession-timeout");
    return false;
}

static bool ensureWsTransport(uint32_t timeoutMs) {
    if (s_wsTransportConnected) return true;

    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        s_ws.loop();
        if (s_wsTransportConnected) return true;
        delay(2);
    }

    setLastError("ws_transport_reconnect_timeout");
    logState("ensureWsTransport-timeout");
    return false;
}

static bool sendAudioChunk(const uint8_t* pcm, size_t lenBytes) {
    if (!pcm || lenBytes == 0) return false;
    return s_ws.sendBIN((uint8_t*)pcm, lenBytes);
}

static void sendHelloHandshake() {
    if (!s_wsTransportConnected) return;

    JsonDocument doc;
    doc["type"] = "hello";
    doc["client"] = "esp32";
    doc["transport"] = "binary";

    String out;
    serializeJson(doc, out);
    if (s_ws.sendTXT(out)) {
        Serial.println("[MascotWS] -> hello (esp32,binary)");
    } else {
        setLastError("tx_hello_failed");
    }
}

static bool sendEndTurnSignal() {
    if (strlen(AI_WS_END_TURN_TYPE) == 0) return true;

    JsonDocument doc;
    doc["type"] = AI_WS_END_TURN_TYPE;

    String out;
    serializeJson(doc, out);
    bool ok = s_ws.sendTXT(out);
    if (ok) {
        Serial.printf("[MascotWS] -> %s\n", AI_WS_END_TURN_TYPE);
    } else {
        setLastError("tx_end_turn_failed");
    }
    return ok;
}

static bool sendTextPrompt(const char* text) {
    if (!text || text[0] == '\0') return false;

    JsonDocument doc;
    doc["type"] = "text";
    doc["text"] = text;

    String out;
    serializeJson(doc, out);
    bool ok = s_ws.sendTXT(out);
    if (ok) {
        Serial.printf("[MascotWS] -> text (%d chars)\n", (int)strlen(text));
    } else {
        setLastError("tx_text_prompt_failed");
    }
    return ok;
}

static bool sendRecordingToLiveSession(const uint8_t* pcm, size_t lenBytes) {
    size_t offset = 0;
    size_t chunks = 0;
    uint32_t startedAt = millis();

    while (offset < lenBytes) {
        size_t chunkLen = min(TX_CHUNK_BYTES, lenBytes - offset);
        if (!sendAudioChunk(pcm + offset, chunkLen)) {
            setLastError("tx_audio_chunk_send_failed_or_oom");
            return false;
        }
        offset += chunkLen;
        chunks++;

        if ((chunks % 16) == 0 || offset == lenBytes) {
            Serial.printf("[MascotWS] TX progress: %d/%d KB (%d chunks)\n",
                          (int)(offset / 1024),
                          (int)(lenBytes / 1024),
                          (int)chunks);
        }

        // Pace upload close to real-time so server VAD/turn detection behaves predictably.
        uint32_t audioMsSent = (uint32_t)((offset / 2) * 1000 / MIC_SAMPLE_RATE);
        uint32_t elapsedMs = millis() - startedAt;
        if (audioMsSent > elapsedMs) {
            delay(audioMsSent - elapsedMs);
        }

        s_ws.loop();
        delay(1);
    }

    Serial.printf("[MascotWS] Uploaded %d KB in %d chunks.\n", (int)(lenBytes / 1024), (int)chunks);
    return true;
}

static bool sendSilenceTailToLiveSession(uint32_t silenceMs) {
    if (silenceMs == 0) return true;

    const size_t bytesPerMs = (MIC_SAMPLE_RATE * 2) / 1000; // mono 16-bit PCM
    size_t totalBytes = silenceMs * bytesPerMs;
    if (totalBytes == 0) return true;

    uint8_t zeroChunk[TX_CHUNK_BYTES] = {0};
    size_t offset = 0;
    size_t chunks = 0;
    uint32_t startedAt = millis();

    while (offset < totalBytes) {
        size_t chunkLen = min(TX_CHUNK_BYTES, totalBytes - offset);
        if (!sendAudioChunk(zeroChunk, chunkLen)) {
            setLastError("tx_silence_tail_failed_or_oom");
            return false;
        }
        offset += chunkLen;
        chunks++;

        uint32_t audioMsSent = (uint32_t)((offset / 2) * 1000 / MIC_SAMPLE_RATE);
        uint32_t elapsedMs = millis() - startedAt;
        if (audioMsSent > elapsedMs) {
            delay(audioMsSent - elapsedMs);
        }

        s_ws.loop();
        delay(1);
    }

    Serial.printf("[MascotWS] Appended %lums silence tail (%d chunks).\n",
                  (unsigned long)silenceMs,
                  (int)chunks);
    return true;
}

static void processPendingRecording() {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureLiveSession(LIVE_READY_WAIT_MS)) {
            Serial.println("[MascotWS] Live session not ready; skipping turn.");
            logState("processPendingRecording-live-not-ready");
            return;
        }

        clearRxAudio();
        s_waitingForTurn = true;
        s_turnComplete = false;
        s_disconnectedDuringTurn = false;

        Serial.printf("[MascotWS] Sending %d KB recorded PCM...\n", (int)(s_pendingLen / 1024));
        bool sent = sendRecordingToLiveSession(s_pendingBuf, s_pendingLen);
        if (!sent) {
            s_waitingForTurn = false;
            logState("processPendingRecording-send-failed");
            return;
        }

        if (!sendSilenceTailToLiveSession(TURN_SILENCE_TAIL_MS)) {
            s_waitingForTurn = false;
            logState("processPendingRecording-silence-tail-failed");
            return;
        }

        if (!sendEndTurnSignal()) {
            s_waitingForTurn = false;
            logState("processPendingRecording-end-turn-failed");
            return;
        }

        const uint32_t start = millis();
        bool nudgeSent = false;
        uint32_t lastWaitLogSec = 0;
        while (!s_turnComplete && (millis() - start < TURN_TIMEOUT_MS)) {
            s_ws.loop();
            uint32_t elapsedMs = millis() - start;
            uint32_t elapsedSec = (millis() - start) / 1000;
            if (elapsedSec >= 5 && (elapsedSec % 5 == 0) && elapsedSec != lastWaitLogSec) {
                lastWaitLogSec = elapsedSec;
                Serial.printf("[MascotWS] Waiting model turn... %lus\n", (unsigned long)elapsedSec);
            }

            // Current backend protocol has no explicit end_turn.
            // Nudge model finalization once if no audio has arrived yet.
            if (USE_TEXT_NUDGE && !nudgeSent && s_rxAudioLen == 0 && s_rxStreamBytes == 0 && elapsedMs >= TURN_NUDGE_AFTER_MS) {
                if (sendTextPrompt("Mình đã nói xong.")) {
                    nudgeSent = true;
                    Serial.println("[MascotWS] Sent text nudge while waiting for model reply.");
                }
            }
            delay(2);
        }
        s_waitingForTurn = false;

        finishRxAudioStreamIfNeeded();

        if (s_rxStreamBytes > 0 || s_rxAudioLen > 0) {
            if (s_rxAudioLen > 0) {
                Serial.printf("[MascotWS] Playing %d KB buffered model audio (%lu chunks).\n",
                              (int)(s_rxAudioLen / 1024),
                              (unsigned long)s_rxAudioChunks);
                playPCM(s_rxAudio, s_rxAudioLen, SPEAKER_SAMPLE_RATE);
            }
            clearRxAudio();
            return;
        }

        if (s_disconnectedDuringTurn && attempt == 0) {
            Serial.println("[MascotWS] Disconnected mid-turn. Reconnecting and retrying once...");
            if (!ensureWsTransport(WS_CONNECT_WAIT_MS) || !ensureLiveSession(LIVE_READY_WAIT_MS)) {
                clearRxAudio();
                logState("processPendingRecording-retry-reconnect-failed");
                return;
            }
            continue;
        }

        if (!s_turnComplete) {
            setLastError("turn_timeout_waiting_for_model");
            Serial.printf("[MascotWS] Turn timed out after %lus. ws=%d live=%d\n",
                          (unsigned long)(TURN_TIMEOUT_MS / 1000),
                          (int)s_wsTransportConnected,
                          (int)s_liveSessionConnected);
            logState("processPendingRecording-turn-timeout");
        }

        setLastError("model_returned_no_audio");
        Serial.println("[MascotWS] No playable audio returned (0 bytes).");
        logState("processPendingRecording-no-audio");
        clearRxAudio();
        return;
    }
}

void onRecordingReady(const uint8_t* pcm16, size_t lenBytes) {
    Serial.printf("[Mic] Captured utterance ready: %d bytes (%d KB).\n",
                  (int)lenBytes,
                  (int)(lenBytes / 1024));
    s_pendingBuf = pcm16;
    s_pendingLen = lenBytes;
    s_micTaskHandle = xTaskGetCurrentTaskHandle();
    s_audioReady = true;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void setupGemini() {
    Serial.println("[MascotWS] Initializing live mascot client...");

#if AI_WS_USE_SSL
    s_ws.beginSSL(AI_WS_HOST, AI_WS_PORT, AI_WS_PATH);
#if AI_WS_ALLOW_INSECURE_TLS
    Serial.println("[MascotWS] TLS insecure mode requested (library uses insecure when no CA/fingerprint).");
#endif
#else
    s_ws.begin(AI_WS_HOST, AI_WS_PORT, AI_WS_PATH);
#endif

    s_ws.onEvent(onWsEvent);
    s_ws.setReconnectInterval(3000);

    const uint32_t start = millis();
    while (!s_wsTransportConnected && (millis() - start < WS_CONNECT_WAIT_MS)) {
        s_ws.loop();
        delay(2);
    }

    if (!s_wsTransportConnected) {
        Serial.println("[MascotWS] Transport connect timeout; will keep retrying in loop.");
    }
}

void loopGemini() {
    s_ws.loop();

    if (!s_audioReady) return;
    s_audioReady = false;

    processPendingRecording();

    if (s_micTaskHandle) {
        xTaskNotifyGive(s_micTaskHandle);
        s_micTaskHandle = nullptr;
    }
}