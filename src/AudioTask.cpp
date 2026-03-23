#include "AudioTask.h"
#include "GeminiClient.h"
#include <math.h>

static bool s_speakerTxActive = false;

// -------------------------------------------------------
// VAD tuning constants
// -------------------------------------------------------
// Raise VAD_THRESHOLD if silence triggers recordings.
// Lower it if speech is missed.
#define VAD_THRESHOLD      500   // mean-abs energy level to detect speech
#define VAD_PRE_ROLL       10    // frames to keep before speech starts (~160ms)
#define VAD_SILENCE_FRAMES 50    // frames of silence before ending (~800ms)
#define VAD_MAX_FRAMES     375   // max recording cap (~6s)

// Frame = 256 int16 samples = 512 bytes ≈ 16ms at 16kHz
#define CHUNK_SAMPLES 256

// -------------------------------------------------------
// I2S setup — INMP441 mic, RX only, 16-bit samples
// -------------------------------------------------------
void setupI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = MIC_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = 0,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_DIN
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) { Serial.printf("[I2S] Driver install FAILED: %d\n", err); return; }
    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) { Serial.printf("[I2S] Pin config FAILED: %d\n", err); return; }
    i2s_zero_dma_buffer(I2S_NUM_0);

    pinMode(I2S_DOUT, OUTPUT);
    digitalWrite(I2S_DOUT, LOW);

    Serial.println("[I2S] Microphone ready.");
}

bool beginSpeakerPlayback(uint32_t sampleRate) {
    if (s_speakerTxActive) return true;

    // Uninstall mic driver so we can reuse the BCK/WS pins for TX
    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = sampleRate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = 0,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[Speaker] TX driver install FAILED: %d - restoring mic\n", err);
        setupI2S();
        return false;
    }
    i2s_set_pin(I2S_NUM_0, &pins);

    s_speakerTxActive = true;
    return true;
}

size_t writeSpeakerPCM(const uint8_t* pcm, size_t lenBytes) {
    if (!s_speakerTxActive || !pcm || lenBytes == 0) return 0;

    size_t written = 0;
    i2s_write(I2S_NUM_0, pcm, lenBytes, &written, pdMS_TO_TICKS(500));
    return written;
}

void endSpeakerPlayback() {
    if (!s_speakerTxActive) return;

    // Drain DMA buffers before teardown
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(120);

    i2s_driver_uninstall(I2S_NUM_0);
    setupI2S();
    s_speakerTxActive = false;
    Serial.println("[Speaker] Playback done, mic restored.");
}

// -------------------------------------------------------
// Speaker playback — tears down mic RX, installs speaker TX,
// plays PCM, then restores mic RX. Only call while mic task
// is blocked (e.g. from inside loopGemini on Core 0).
// -------------------------------------------------------
void playPCM(const uint8_t* pcm, size_t lenBytes, uint32_t sampleRate) {
    if (!beginSpeakerPlayback(sampleRate)) {
        return;
    }

    Serial.printf("[Speaker] Playing %d KB at %lu Hz...\n", lenBytes / 1024, (unsigned long)sampleRate);

    // Write PCM to the speaker in 1 KB chunks
    const size_t CHUNK = 1024;
    size_t offset = 0;
    while (offset < lenBytes) {
        size_t toWrite = min(CHUNK, lenBytes - offset);
        size_t written = writeSpeakerPCM(pcm + offset, toWrite);
        offset += written > 0 ? written : toWrite; // avoid stall if write returns 0
    }

    endSpeakerPlayback();
}

// -------------------------------------------------------
// Mean absolute energy — fast VAD metric (no sqrt needed)
// -------------------------------------------------------
static uint32_t frameEnergy(const int16_t* buf, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = buf[i];
        sum += (s < 0) ? -s : s;
    }
    return sum / n;
}

// -------------------------------------------------------
// Mic capture task with VAD
// -------------------------------------------------------
static void micCaptureTask(void* pv) {
    const size_t FRAME_BYTES = CHUNK_SAMPLES * sizeof(int16_t);

    // With 16-bit I2S, i2s_read fills int16 directly — no separate raw buffer needed.
    int16_t* frameBuf = (int16_t*)ps_malloc(FRAME_BYTES);
    int16_t* preRoll  = (int16_t*)ps_malloc(VAD_PRE_ROLL  * FRAME_BYTES);
    int16_t* recBuf   = (int16_t*)ps_malloc(VAD_MAX_FRAMES * FRAME_BYTES);

    if (!frameBuf || !preRoll || !recBuf) {
        Serial.println("[Mic] PSRAM alloc failed — task aborted!");
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("[VAD] Buffers ready. Pre-roll=%dms, Silence=%dms, Max=%ds\n",
                  VAD_PRE_ROLL * 16, VAD_SILENCE_FRAMES * 16, VAD_MAX_FRAMES * 16 / 1000);

    enum VadState { IDLE, RECORDING } state = IDLE;
    int preRollHead  = 0;
    int recFrames    = 0;
    int silenceCount = 0;
    int speechFrames = 0;   // speech frames counted during recording
    Serial.println("[VAD] Listening for speech...");

    while (true) {
        // Read one I2S frame — 16-bit samples arrive directly in frameBuf
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_0, frameBuf, FRAME_BYTES, &bytesRead, pdMS_TO_TICKS(100));
        size_t samplesRead = bytesRead / sizeof(int16_t);
        if (samplesRead == 0) continue;

        uint32_t energy  = frameEnergy(frameBuf, samplesRead);
        bool     speech  = (energy > VAD_THRESHOLD);

        if (state == IDLE) {
            // Always store in pre-roll ring buffer (overwrites oldest)
            memcpy(preRoll + (preRollHead % VAD_PRE_ROLL) * CHUNK_SAMPLES,
                   frameBuf, samplesRead * sizeof(int16_t));
            preRollHead++;

            if (speech) {
                Serial.printf("[VAD] Speech detected (energy=%u). Recording...\n", energy);
                state        = RECORDING;
                silenceCount = 0;
                recFrames    = 0;
                speechFrames = 1;  // this frame is speech

                // Copy preroll frames in order (includes the triggering frame)
                int preCount = (preRollHead < VAD_PRE_ROLL) ? preRollHead : VAD_PRE_ROLL;
                int startIdx = (preRollHead >= (int)VAD_PRE_ROLL)
                               ? (preRollHead % VAD_PRE_ROLL) : 0;
                for (int i = 0; i < preCount && recFrames < VAD_MAX_FRAMES; i++) {
                    int idx = (startIdx + i) % VAD_PRE_ROLL;
                    memcpy(recBuf + recFrames * CHUNK_SAMPLES,
                           preRoll + idx * CHUNK_SAMPLES,
                           CHUNK_SAMPLES * sizeof(int16_t));
                    recFrames++;
                }
            }
        } else { // RECORDING
            if (recFrames < VAD_MAX_FRAMES) {
                memcpy(recBuf + recFrames * CHUNK_SAMPLES,
                       frameBuf, samplesRead * sizeof(int16_t));
                recFrames++;
            }

            if (speech) speechFrames++;
            silenceCount = speech ? 0 : silenceCount + 1;

            bool hitSilence = (silenceCount >= VAD_SILENCE_FRAMES);
            bool hitMax     = (recFrames >= VAD_MAX_FRAMES);

            if (hitSilence || hitMax) {
                size_t totalBytes = (size_t)recFrames * FRAME_BYTES;
                Serial.printf("[VAD] End of speech (%s). %d frames (%d KB), %d speech frames.\n",
                              hitMax ? "max" : "silence", recFrames, totalBytes / 1024, speechFrames);

                if (speechFrames >= 3) {
                    onRecordingReady((const uint8_t*)recBuf, totalBytes);
                    // Blocks until loopGemini() finishes and wakes us
                    Serial.println("[VAD] Task resumed. Flushing hardware startup pop...");
                    for (int i = 0; i < 60; i++) { // Discard 60 frames (~1 second)
                        size_t bytesRead = 0;
                        i2s_read(I2S_NUM_0, frameBuf, FRAME_BYTES, &bytesRead, pdMS_TO_TICKS(100));
                    }

                } else {
                    Serial.println("[VAD] Skipped — no real speech detected (spurious trigger).");
                }

                state        = IDLE;
                preRollHead  = 0;
                silenceCount = 0;
                speechFrames = 0;
                Serial.println("[VAD] Listening for speech...");
            }
        }
    }
}

void startAudioTasks() {
    xTaskCreatePinnedToCore(micCaptureTask, "MicTask", 8192, NULL, 5, NULL, 1);
}

