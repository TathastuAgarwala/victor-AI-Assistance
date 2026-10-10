#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "########";
const char* WIFI_PASSWORD = "#######";

// ============================================================
// LAPTOP SERVER
// ============================================================

const char* SERVER_IP = "##########";
const uint16_t SERVER_PORT = 8765;
const char* SERVER_PATH = "/";

// ============================================================
// OBJECTS
// ============================================================

WebSocketsClient webSocket;

// ============================================================
// BUTTON + LED
// ============================================================

#define BUTTON_PIN 4

#define LED_RED_PIN 18
#define LED_GREEN_PIN 19

// 0 = common pin goes to GND   (common cathode)
// 1 = common pin goes to 3.3V  (common anode)
#define LED_COMMON_ANODE 0

#define LED_ON_LEVEL   (LED_COMMON_ANODE ? LOW  : HIGH)
#define LED_OFF_LEVEL  (LED_COMMON_ANODE ? HIGH : LOW)

// Short red blink while listening mode is ON (150 ms every 2 s)
#define LISTEN_HEARTBEAT 1

// ============================================================
// INMP441
// ============================================================

// The built-in DAC only works on I2S0, so playback uses I2S0.
// The microphone runs on I2S1 and is NEVER stopped, so Victor can hear
// you while music or an answer is playing.
#define I2S_DAC_PORT I2S_NUM_0
#define I2S_MIC_PORT I2S_NUM_1

#define I2S_SCK 26
#define I2S_WS  27
#define I2S_SD  33

#define I2S_BUFFER_SIZE 256          // samples per block (16 ms @ 16 kHz)
#define BLOCK_MS (I2S_BUFFER_SIZE * 1000 / 16000)

// Mic gain: 32-bit sample >> MIC_SHIFT. 16 = unity, 14 = 4x gain.
#define MIC_SHIFT 14

int32_t i2sBuffer[I2S_BUFFER_SIZE];
int16_t micPcmBuffer[I2S_BUFFER_SIZE];

float dcPrevIn = 0.0f;
float dcPrevOut = 0.0f;

volatile bool micReady = false;

// ============================================================
// VOICE ACTIVITY DETECTION (end-of-speech detection)
// ============================================================

// Speech must exceed max(VAD_MIN_THRESHOLD, noiseFloor * VAD_NOISE_RATIO)
#define VAD_MIN_THRESHOLD     120.0f
#define VAD_NOISE_RATIO       3.0f

// Consecutive loud blocks needed to start recording (3 x 16 ms)
#define VAD_START_BLOCKS      3

// Silence after speech before we tell the AI to answer
#define VAD_SILENCE_MS        1200

// Safety limit for one utterance
#define VAD_MAX_UTTERANCE_MS  15000

// Audio kept from BEFORE speech was detected (20 x 16 ms = 320 ms)
#define VAD_PREROLL_BLOCKS    20

// Utterances with fewer loud blocks than this are discarded (clicks, bumps)
#define VAD_MIN_LOUD_BLOCKS   8

// Calibrate noise floor for this many blocks when listening starts
#define VAD_CALIB_BLOCKS      40

// Print mic level / threshold once per second (for tuning)
#define VAD_DEBUG             1

#define PLAY_VAD_RATIO        3.0f
// Consecutive loud blocks needed while playing (6 x 16 ms = ~100 ms)
#define PLAY_START_BLOCKS     6
// Blocks used to measure the playback level when playback starts (~0.5 s)
#define PLAY_CALIB_BLOCKS     30
// Playback volume while you are speaking (%) so the mic can hear you
#define TTS_DUCK_PCT          15

#define SILENCE_BLOCKS   (VAD_SILENCE_MS / BLOCK_MS)
#define MAX_BLOCKS       (VAD_MAX_UTTERANCE_MS / BLOCK_MS)

bool listening = false;              // listening mode (toggled by button)
bool inSpeech = false;               // currently recording an utterance

float noiseFloor = 30.0f;
int loudStreak = 0;
int silentBlocks = 0;
int loudBlocks = 0;
int utteranceBlocks = 0;
int calibLeft = 0;

int16_t preroll[VAD_PREROLL_BLOCKS][I2S_BUFFER_SIZE];
int prerollHead = 0;
int prerollCount = 0;

unsigned long ignoreMicUntil = 0;
// After we send speech_end the server is busy (STT -> search -> AI -> TTS,
// can take 20+ s). Do NOT record/send audio during that time: it floods the
// server's receive queue and makes its keepalive ping time out.
bool awaitingReply = false;
unsigned long awaitingReplyUntil = 0;
#define AWAIT_REPLY_TIMEOUT_MS 45000

// Playback statistics (diagnostics)
volatile uint32_t ttsRxBytes = 0;
volatile uint32_t ttsPlayedBytes = 0;

unsigned long ignoreButtonUntil = 0;   // ignore presses right after a stop
unsigned long readyCueUntil = 0;       // solid red = ready, say "Victor"

// ============================================================
// TTS / MUSIC SETTINGS  (must match the server)
// ============================================================

#define TTS_SAMPLE_RATE      22050
#define TTS_INPUT_CHANNELS   1

#define TTS_VOLUME_PCT       60

#define TTS_OVERSAMPLE       ((TTS_SAMPLE_RATE <= 24000) ? 4 : 2)

#define TTS_STREAM_SIZE      (96 * 1024)

#define TTS_PREBUFFER_BYTES  (24 * 1024)

#define TTS_REBUFFER_BYTES   (8 * 1024)

#define TTS_CHUNK_BYTES      1024

#define IDLE_DAC_HOLD        1
StreamBufferHandle_t ttsStream = nullptr;

volatile bool ttsActive = false;
volatile bool ttsFinished = false;
volatile bool ttsStopRequested = false;

bool dacInstalled = false;                 // I2S0 DAC driver state
volatile int ttsVolumePct = TTS_VOLUME_PCT;
volatile int playCalib = 0;
float playLevel = 0.0f;                    // mic level caused by playback

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void setupI2S();
void resetVad();

// ============================================================
// DSP STATE (used only by playback task)
// ============================================================

static uint16_t silenceBuf[256 * 2];
static uint16_t fadeBuf[256 * 2];

static int32_t  nsErr = 0;
static uint32_t rngState = 0x1234ABCDu;
static int32_t  prevS = 0;
static uint32_t fadeGain = 0;

static void initSilence()
{
    for (size_t i = 0; i < 256 * 2; i++)
    {
        silenceBuf[i] = 0x8000;
    }
}

static void resetDsp()
{
    nsErr = 0;
    prevS = 0;
    fadeGain = 0;
}

static inline uint16_t quantize(int32_t s)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;

    int32_t r1 = (int32_t)(rngState & 0xFF);
    int32_t r2 = (int32_t)((rngState >> 8) & 0xFF);
    int32_t d = (r1 - r2) >> 1;

    int32_t v = s + nsErr;
    int32_t q = (v + d + 128) >> 8;

    if (q > 127)  q = 127;
    if (q < -128) q = -128;

    nsErr = v - (q << 8);

    if (nsErr > 512)  nsErr = 512;
    if (nsErr < -512) nsErr = -512;

    return (uint16_t)((q + 128) << 8);
}

static void writeFrames(const uint16_t* buf, size_t frames, bool checkActive)
{
    size_t bytes = frames * 2 * sizeof(uint16_t);
    size_t offset = 0;
    int stalls = 0;

    while (offset < bytes)
    {
        if (checkActive && !ttsActive)
        {
            break;
        }

        size_t written = 0;

        i2s_write(
            I2S_DAC_PORT,
            (const uint8_t*)buf + offset,
            bytes - offset,
            &written,
            pdMS_TO_TICKS(100)
        );

        offset += written;

        if (written == 0)
        {
            if (++stalls > 3)
            {
                break;
            }
        }
        else
        {
            stalls = 0;
        }
    }
}

static void writeMidSilence(size_t frames, bool checkActive)
{
    while (frames > 0)
    {
        size_t n = frames > 256 ? 256 : frames;
        writeFrames(silenceBuf, n, checkActive);
        frames -= n;
    }
}

static void fadeOutToSilence()
{
    const int32_t total = 1024;

    for (int c = 0; c < 4; c++)
    {
        for (int j = 0; j < 256; j++)
        {
            int32_t idx = c * 256 + j;
            int32_t s = (prevS * (total - idx)) / total;

            uint16_t u = quantize(s);

            fadeBuf[2 * j]     = u;
            fadeBuf[2 * j + 1] = u;
        }

        writeFrames(fadeBuf, 256, true);
    }

    prevS = 0;
}
// ============================================================
// INITIALIZE TTS STREAM BUFFER
// ============================================================

bool initAudioBuffer()
{
    ttsStream = xStreamBufferCreate(TTS_STREAM_SIZE, 1);

    if (ttsStream == nullptr)
    {
        Serial.println("ERROR: TTS stream buffer allocation failed!");
        Serial.print("Free heap: ");
        Serial.println(ESP.getFreeHeap());
        return false;
    }

    Serial.print("TTS buffer: ");
    Serial.print(TTS_STREAM_SIZE);
    Serial.println(" bytes");

    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());

    return true;
}

// ============================================================
// PLAYBACK USING ESP32 INTERNAL DAC (GPIO25 -> amp)
// ============================================================

void setupPlaybackI2S()
{
    if (dacInstalled)
    {
        i2s_driver_uninstall(I2S_DAC_PORT);
        dacInstalled = false;
    }

    i2s_config_t config = {};

    config.mode =
        (i2s_mode_t)(
            I2S_MODE_MASTER |
            I2S_MODE_TX |
            I2S_MODE_DAC_BUILT_IN
        );

    config.sample_rate = TTS_SAMPLE_RATE * TTS_OVERSAMPLE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8;
    config.dma_buf_len = 512;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 0;

    esp_err_t result = i2s_driver_install(I2S_DAC_PORT, &config, 0, NULL);

    if (result != ESP_OK)
    {
        Serial.print("Playback I2S install error: ");
        Serial.println(result);
        return;
    }

    dacInstalled = true;

    i2s_set_pin(I2S_DAC_PORT, NULL);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);

    // Prime DMA with DAC midpoint (not zero) to avoid a thump
    writeMidSilence(256 * 16, false);

    Serial.print("TTS DAC playback ready: GPIO25 @ ");
    Serial.print(TTS_SAMPLE_RATE * TTS_OVERSAMPLE);
    Serial.println(" Hz");
}

void stopPlaybackI2S()
{
    ttsActive = false;

    // Give the playback task time to leave i2s_write()
    delay(70);

    // Task is idle now, so it is safe to drop any leftover audio
    xStreamBufferReset(ttsStream);

    if (dacInstalled)
    {
        i2s_driver_uninstall(I2S_DAC_PORT);
        dacInstalled = false;
    }

#if IDLE_DAC_HOLD
    dacWrite(25, 128);
#endif

    ttsFinished = false;

    // The microphone (I2S1) never stopped. Ignore the speaker's echo tail
    // for a moment and forget the playback level.
    ignoreMicUntil = millis() + 500;
    playLevel = 0.0f;

    Serial.println("Playback finished.");
}

// ============================================================
// PLAYBACK TASK
// ============================================================

void ttsPlaybackTask(void* parameter)
{
    alignas(4) static uint8_t raw[TTS_CHUNK_BYTES + 8];
    static uint16_t out[(TTS_CHUNK_BYTES / 2) * 4 * 2];

    size_t leftover = 0;
    bool started = false;
    size_t prebufferNeed = TTS_PREBUFFER_BYTES;
    int curVol = TTS_VOLUME_PCT;

    const size_t bytesPerFrame = 2 * TTS_INPUT_CHANNELS;

    for (;;)
    {
        if (!ttsActive)
        {
            started = false;
            leftover = 0;
            prebufferNeed = TTS_PREBUFFER_BYTES;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!started)
        {
            if (
                xStreamBufferBytesAvailable(ttsStream) < prebufferNeed &&
                !ttsFinished
            )
            {
                writeMidSilence(128, true);
                continue;
            }

            started = true;
            leftover = 0;
            resetDsp();

            curVol = ttsVolumePct;
            playCalib = PLAY_CALIB_BLOCKS;   // measure playback level in the mic

            Serial.println("Playback started.");
        }

        size_t got =
            xStreamBufferReceive(
                ttsStream,
                raw + leftover,
                TTS_CHUNK_BYTES - leftover,
                pdMS_TO_TICKS(30)
            );

        if (got == 0)
        {
            if (
                ttsFinished &&
                xStreamBufferBytesAvailable(ttsStream) == 0
            )
            {
                fadeOutToSilence();
                writeMidSilence(256 * 8, true);

                started = false;
                leftover = 0;

                ttsActive = false;
                ttsStopRequested = true;
            }
            else if (!ttsFinished)
            {
                fadeOutToSilence();

                started = false;
                leftover = 0;
                prebufferNeed = TTS_REBUFFER_BYTES;
            }

            continue;
        }

        size_t total = leftover + got;
        size_t frames = total / bytesPerFrame;
        size_t used = frames * bytesPerFrame;

        ttsPlayedBytes += used;

        const int16_t* in = (const int16_t*)raw;

        size_t o = 0;

        for (size_t i = 0; i < frames; i++)
        {
            int32_t m;

            if (TTS_INPUT_CHANNELS == 2)
            {
                m = ((int32_t)in[2 * i] + (int32_t)in[2 * i + 1]) / 2;
            }
            else
            {
                m = in[i];
            }

            // Smooth volume changes (ducking) so there are no clicks
            int target = ttsVolumePct;

            if (curVol < target)
            {
                curVol++;
            }
            else if (curVol > target)
            {
                curVol--;
            }

            m = (m * curVol) / 100;

            if (m > 32767)  m = 32767;
            if (m < -32768) m = -32768;

            if (fadeGain < 256)
            {
                m = (m * (int32_t)fadeGain) >> 8;
                fadeGain++;
            }

            for (int k = 1; k <= TTS_OVERSAMPLE; k++)
            {
                int32_t s = prevS + ((m - prevS) * k) / TTS_OVERSAMPLE;

                uint16_t u = quantize(s);

                out[o++] = u;
                out[o++] = u;
            }

            prevS = m;
        }

        leftover = total - used;

        if (leftover > 0)
        {
            memmove(raw, raw + used, leftover);
        }

        writeFrames(out, frames * TTS_OVERSAMPLE, true);
    }
}

// ============================================================
// I2S SETUP (INMP441 MICROPHONE)
// ============================================================

void setupI2S()
{
    i2s_config_t config = {};

    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = 16000;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8;
    config.dma_buf_len = I2S_BUFFER_SIZE;
    config.use_apll = false;
    config.tx_desc_auto_clear = false;
    config.fixed_mclk = 0;

    i2s_pin_config_t pins = {};

    pins.bck_io_num = I2S_SCK;
    pins.ws_io_num = I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = I2S_SD;

    esp_err_t result;

    result = i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL);

    if (result != ESP_OK)
    {
        Serial.print("I2S driver error: ");
        Serial.println(result);
        return;
    }

    result = i2s_set_pin(I2S_MIC_PORT, &pins);

    if (result != ESP_OK)
    {
        Serial.print("I2S pin error: ");
        Serial.println(result);
        return;
    }

    i2s_zero_dma_buffer(I2S_MIC_PORT);

    micReady = true;

    Serial.println("INMP441 ready.");
}

// ============================================================
// STATUS LED  (single place that decides the colour)
//
//   GREEN = AI speaking / music playing
//   RED   = recording your voice
//   short red blink = listening mode ON, waiting for you
//   OFF   = listening mode OFF
// ============================================================

void setLed(bool red, bool green)
{
    digitalWrite(LED_RED_PIN,   red   ? LED_ON_LEVEL : LED_OFF_LEVEL);
    digitalWrite(LED_GREEN_PIN, green ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

void updateLed()
{
    bool red = false;
    bool green = false;

    if (inSpeech)
    {
        red = true;
    }
    else if (ttsActive)
    {
        green = true;
    }
    else if (listening && millis() < readyCueUntil)
    {
        red = true;     // solid red for 2.5 s after a stop = "I'm listening"
    }
#if LISTEN_HEARTBEAT
    else if (listening && (millis() % 2000) < 150)
    {
        red = true;
    }
#endif

    setLed(red, green);
}

// ============================================================
// VAD / LISTENING HELPERS
// ============================================================

void resetVad()
{
    inSpeech = false;
    loudStreak = 0;
    silentBlocks = 0;
    loudBlocks = 0;
    utteranceBlocks = 0;
    prerollHead = 0;
    prerollCount = 0;
    calibLeft = VAD_CALIB_BLOCKS;
    dcPrevIn = 0.0f;
    dcPrevOut = 0.0f;
}

void endUtterance(bool cancel)
{
    if (!inSpeech)
    {
        return;
    }

    inSpeech = false;

    if (webSocket.isConnected())
    {
        if (cancel || loudBlocks < VAD_MIN_LOUD_BLOCKS)
        {
            Serial.println("(too short, discarded)");
            webSocket.sendTXT("{\"type\":\"speech_cancel\"}");
        }
        else
        {
            Serial.println("STOP RECORDING (silence detected)");
            webSocket.sendTXT("{\"type\":\"speech_end\"}");

            awaitingReply = true;
            awaitingReplyUntil = millis() + AWAIT_REPLY_TIMEOUT_MS;
        }
    }

    loudStreak = 0;
    silentBlocks = 0;
    loudBlocks = 0;
    utteranceBlocks = 0;
    prerollHead = 0;
    prerollCount = 0;
}

// ------------------------------------------------------------
// Button pressed WHILE music / TTS is playing:
// stop the audio, tell the server, and KEEP listening.
// ------------------------------------------------------------

void stopAudioKeepListening()
{
    Serial.println();
    Serial.println("PHYSICAL STOP: audio stopped, still listening.");

    if (webSocket.isConnected())
    {
        // Server kills music/TTS. Session stays active.
        webSocket.sendTXT("{\"type\":\"stop\"}");
    }

    listening = true;           // keep listening mode ON
    awaitingReply = false;

    ttsActive = false;          // playback task stops writing
    ttsStopRequested = true;    // loop() -> stopPlaybackI2S() -> mic restored

    ignoreButtonUntil = millis() + 1500;   // ignore accidental double-click
    readyCueUntil     = millis() + 2500;   // solid red = ready, say "Victor"
}

// ------------------------------------------------------------
// Button pressed while idle: toggle listening ON / OFF
// ------------------------------------------------------------

void toggleListening()
{
    if (!listening)
    {
        if (!webSocket.isConnected())
        {
            Serial.println("WebSocket not connected.");
            return;
        }

        resetVad();
        ttsStopRequested = false;
        ttsFinished = false;
        listening = true;

        webSocket.sendTXT("{\"type\":\"session\",\"active\":true}");

        Serial.println();
        Serial.println("==============================");
        Serial.println("LISTENING MODE ON");
        Serial.println("Say: \"Victor, ...\"");
        Serial.println("Press button again to stop.");
        Serial.println("==============================");
    }
    else
    {
        endUtterance(true);

        if (webSocket.isConnected())
        {
            webSocket.sendTXT("{\"type\":\"stop\"}");
            webSocket.sendTXT("{\"type\":\"session\",\"active\":false}");
        }

        listening = false;
        awaitingReply = false;
        readyCueUntil = 0;
        ttsStopRequested = true;
        ttsFinished = true;

        Serial.println();
        Serial.println("LISTENING MODE OFF");
    }
}

void handleButton()
{
    static bool lastRaw = HIGH;
    static bool stable = HIGH;
    static unsigned long lastChange = 0;

    bool raw = digitalRead(BUTTON_PIN);

    if (raw != lastRaw)
    {
        lastRaw = raw;
        lastChange = millis();
    }

    if (millis() - lastChange > 40 && raw != stable)
    {
        stable = raw;

        if (stable == LOW)
        {
            if (millis() < ignoreButtonUntil)
            {
                // Ignore a press right after a stop (double-click protection)
            }
            else if (ttsActive)
            {
                // Music or TTS is playing: stop it, keep listening
                stopAudioKeepListening();
            }
            else
            {
                // Idle: turn listening ON / OFF
                toggleListening();
            }
        }
    }
}

// ============================================================
// MICROPHONE: continuous listening with voice activity detection
// ============================================================

void sendBlock(const int16_t* block)
{
    if (webSocket.isConnected())
    {
        webSocket.sendBIN(
            (const uint8_t*)block,
            I2S_BUFFER_SIZE * sizeof(int16_t)
        );
    }
}

void processMicrophone()
{
    if (!listening || !micReady)
    {
        return;
    }

    // Collect one full block WITHOUT blocking, so webSocket.loop() keeps
    // running often enough to receive the music stream.
    static size_t fill = 0;

    size_t got = 0;

    i2s_read(
        I2S_MIC_PORT,
        ((uint8_t*)i2sBuffer) + fill,
        sizeof(i2sBuffer) - fill,
        &got,
        0
    );

    fill += got;

    if (fill < sizeof(i2sBuffer))
    {
        return;
    }

    fill = 0;

    // Server is busy answering: keep the mic drained but send nothing
    if (awaitingReply)
    {
        if (millis() > awaitingReplyUntil)
        {
            Serial.println("(reply timeout - listening again)");
            awaitingReply = false;
            resetVad();
        }

        return;
    }

    // Skip speaker echo right after playback / button stop
    if (millis() < ignoreMicUntil)
    {
        return;
    }

    bool playing = ttsActive;

    // ----------------------------------------------------
    // 32-bit I2S -> 16-bit PCM, DC-blocking filter, level
    // ----------------------------------------------------

    float levelSum = 0.0f;

    for (int i = 0; i < I2S_BUFFER_SIZE; i++)
    {
        float x = (float)(i2sBuffer[i] >> MIC_SHIFT);

        float y = x - dcPrevIn + 0.995f * dcPrevOut;

        dcPrevIn = x;
        dcPrevOut = y;

        if (y > 32767.0f)  y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;

        micPcmBuffer[i] = (int16_t)y;

        levelSum += fabsf(y);
    }

    float level = levelSum / I2S_BUFFER_SIZE;

    // ----------------------------------------------------
    // Calibration
    // ----------------------------------------------------

    if (!playing)
    {
        playLevel = 0.0f;
    }

    if (calibLeft > 0)
    {
        calibLeft--;

        if (!playing)
        {
            noiseFloor = 0.9f * noiseFloor + 0.1f * level;
        }

        return;
    }

    if (playing && playCalib > 0)
    {
        // Playback just started: learn how loud it is in the mic
        playCalib--;
        playLevel = 0.6f * playLevel + 0.4f * level;
        return;
    }

    // ----------------------------------------------------
    // Threshold (higher while music / speech is playing)
    // ----------------------------------------------------

    float threshold = noiseFloor * VAD_NOISE_RATIO;

    if (threshold < VAD_MIN_THRESHOLD)
    {
        threshold = VAD_MIN_THRESHOLD;
    }

    int startBlocks = VAD_START_BLOCKS;

    if (playing)
    {
        float pt = playLevel * PLAY_VAD_RATIO;

        if (pt > threshold)
        {
            threshold = pt;
        }

        startBlocks = PLAY_START_BLOCKS;
    }

#if VAD_DEBUG
    static unsigned long lastDebug = 0;

    if (millis() - lastDebug > 1000)
    {
        lastDebug = millis();

        Serial.printf(
            "[mic] level=%.0f floor=%.0f play=%.0f threshold=%.0f %s%s\n",
            level, noiseFloor, playLevel, threshold,
            playing ? "(playing) " : "",
            inSpeech ? "(recording)" : ""
        );
    }
#endif

    // ----------------------------------------------------
    // NOT RECORDING: keep pre-roll, wait for speech
    // ----------------------------------------------------

    if (!inSpeech)
    {
        if (playing)
        {
            // Track the playback level, but never let your voice raise it
            if (level < threshold)
            {
                playLevel += 0.03f * (level - playLevel);
            }
            else
            {
                playLevel += 0.003f * (level - playLevel);
            }
        }
        else
        {
            // Adapt the noise floor to the room
            if (level < threshold)
            {
                noiseFloor += 0.02f * (level - noiseFloor);
            }
            else
            {
                noiseFloor += 0.001f * (level - noiseFloor);
            }
        }

        memcpy(
            preroll[prerollHead],
            micPcmBuffer,
            I2S_BUFFER_SIZE * sizeof(int16_t)
        );

        prerollHead = (prerollHead + 1) % VAD_PREROLL_BLOCKS;

        if (prerollCount < VAD_PREROLL_BLOCKS)
        {
            prerollCount++;
        }

        if (level > threshold)
        {
            loudStreak++;
        }
        else
        {
            loudStreak = 0;
        }

        if (loudStreak >= startBlocks && webSocket.isConnected())
        {
            Serial.println();
            Serial.println("==============================");
            Serial.println(
                playing
                    ? "RECORDING... (speech detected over playback)"
                    : "RECORDING... (speech detected)"
            );
            Serial.println("==============================");

            inSpeech = true;
            silentBlocks = 0;
            loudBlocks = loudStreak;
            utteranceBlocks = 0;

            webSocket.sendTXT("{\"type\":\"speech_start\"}");

            // Send the pre-roll (includes the current block)
            int start = (prerollCount < VAD_PREROLL_BLOCKS) ? 0 : prerollHead;

            for (int k = 0; k < prerollCount; k++)
            {
                sendBlock(preroll[(start + k) % VAD_PREROLL_BLOCKS]);
            }

            prerollHead = 0;
            prerollCount = 0;
            loudStreak = 0;
        }

        return;
    }

  // ----------------------------------------------------
    // RECORDING: stream audio, watch for silence
    // ----------------------------------------------------

    sendBlock(micPcmBuffer);

    utteranceBlocks++;

    if (level > threshold * 0.7f)
    {
        silentBlocks = 0;
        loudBlocks++;
    }
    else
    {
        silentBlocks++;
    }

    if (silentBlocks >= SILENCE_BLOCKS)
    {
        endUtterance(false);
    }
    else if (utteranceBlocks >= MAX_BLOCKS)
    {
        Serial.println("(max length reached)");
        endUtterance(false);
    }
}
// ============================================================
// WEBSOCKET EVENT
// ============================================================

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length)
{
    if (type == WStype_CONNECTED)
    {
        Serial.println();
        Serial.println("WebSocket CONNECTED");

        webSocket.sendTXT("{\"type\":\"hello\",\"device\":\"esp32\"}");

        if (listening)
        {
            webSocket.sendTXT("{\"type\":\"session\",\"active\":true}");
        }

        return;
    }

    if (type == WStype_DISCONNECTED)
    {
        Serial.println();
        Serial.println("WebSocket DISCONNECTED");
        Serial.printf("  uptime=%lu ms  heap=%u  min heap=%u  ttsBuf=%u\n",
                      millis(),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMinFreeHeap(),
                      ttsStream ? (unsigned)xStreamBufferBytesAvailable(ttsStream) : 0u);

        inSpeech = false;
        loudStreak = 0;
        prerollHead = 0;
        prerollCount = 0;
        awaitingReply = false;

        // Connection lost while playing: let the player drain and finish,
        // otherwise it would wait forever and the mic would stay disabled.
        if (ttsActive)
        {
            ttsFinished = true;
        }

        return;
    }

    // ========================================================
    // BINARY (TTS / MUSIC AUDIO)
    // ========================================================

    if (type == WStype_BIN)
    {
        // Dropped automatically after a physical STOP (ttsActive = false)
        if (!ttsActive || ttsStream == nullptr)
        {
            return;
        }

        // Short timeout: never block the WebSocket loop for long
        size_t sent =
            xStreamBufferSend(
                ttsStream,
                payload,
                length,
                pdMS_TO_TICKS(150)
            );

        ttsRxBytes += sent;

        if (sent != length)
        {
            Serial.printf(
                "TTS BUFFER FULL: %u/%u\n",
                (unsigned)sent,
                (unsigned)length
            );
        }

        return;
    }

    // ========================================================
    // TEXT
    // ========================================================

    if (type == WStype_TEXT)
    {
        JsonDocument doc;

        DeserializationError error = deserializeJson(doc, payload, length);

        if (error)
        {
            Serial.print("JSON error: ");
            Serial.println(error.c_str());
            return;
        }

        const char* event = doc["type"];

        if (!event)
        {
            return;
        }

        if (strcmp(event, "transcript") == 0)
        {
            const char* text = doc["text"];

            Serial.println();
            Serial.println("YOU:");
            Serial.println(text ? text : "");
        }
        else if (strcmp(event, "ai_response") == 0)
        {
            const char* text = doc["text"];

            Serial.println();
            Serial.println("AI:");
            Serial.println(text ? text : "");
        }
        else if (strcmp(event, "idle") == 0)
        {
            // Server finished processing this utterance
            awaitingReply = false;
        }
        else if (strcmp(event, "ignored") == 0)
        {
            awaitingReply = false;

            const char* text = doc["text"];

            Serial.print("(ignored - no wake word) ");
            Serial.println(text ? text : "");
        }
        else if (strcmp(event, "tts_start") == 0)
        {
            Serial.println();
            Serial.println("TTS / MUSIC START");

            // The microphone has its own I2S port, so a recording in progress
            // is NOT interrupted by playback.

            if (ttsActive)
            {
                ttsActive = false;
                delay(120);
            }

            ttsStopRequested = false;
            ttsFinished = false;
            awaitingReply = false;
            ttsRxBytes = 0;
            ttsPlayedBytes = 0;

            xStreamBufferReset(ttsStream);

            setupPlaybackI2S();

            ttsActive = true;
        }
        else if (
            strcmp(event, "tts_stop") == 0 ||
            strcmp(event, "stopped") == 0
        )
        {
            // Server cancelled speech/music (voice "stop", new command...).
            // Stop right away and bring the microphone back.
            awaitingReply = false;

            if (ttsActive)
            {
                Serial.println("Server stopped the audio.");

                ttsActive = false;
                ttsStopRequested = true;
            }
        }
        else if (strcmp(event, "tts_end") == 0)
        {
            Serial.println("TTS NETWORK END");

            // Ignore a late tts_end that arrives after a physical STOP
            if (ttsActive)
            {
                ttsFinished = true;
            }
        }
        else if (strcmp(event, "error") == 0)
        {
            const char* message = doc["message"];

            awaitingReply = false;

            Serial.print("SERVER ERROR: ");
            Serial.println(message ? message : "");
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println("ESP32 VOICE ASSISTANT - VICTOR");
    Serial.println("================================");

    initSilence();

    if (!initAudioBuffer())
    {
        Serial.println("FATAL: Cannot allocate audio buffer.");

        while (true)
        {
            delay(1000);
        }
    }

    xTaskCreatePinnedToCore(
        ttsPlaybackTask,
        "ttsPlay",
        6144,
        NULL,
        3,
        NULL,
        1
    );

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_GREEN_PIN, OUTPUT);

    // LED self-test: red, then green, then off
    setLed(true, false);  delay(500);
    setLed(false, true);  delay(500);
    setLed(false, false);

    setupI2S();

#if IDLE_DAC_HOLD
    dacWrite(25, 128);
#endif

    Serial.println();
    Serial.println("Connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected.");

    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Server: ");
    Serial.print(SERVER_IP);
    Serial.print(":");
    Serial.println(SERVER_PORT);

    webSocket.begin(SERVER_IP, SERVER_PORT, SERVER_PATH);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    webSocket.enableHeartbeat(25000, 10000, 5);   // tolerant while streaming audio

    Serial.println();
    Serial.println("================================");
    Serial.println("SYSTEM READY");
    Serial.println("================================");
    Serial.println();
    Serial.println("Button (idle): listening ON / OFF.");
    Serial.println("Button (while audio plays): stop audio, keep listening.");
    Serial.println("Then say: \"Victor, ...\"");
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    webSocket.loop();

    handleButton();

    processMicrophone();

    // Playback finished or stopped -> restore microphone
    if (ttsStopRequested)
    {
        ttsStopRequested = false;
        stopPlaybackI2S();
    }

    // Lower the playback volume while you are speaking
    ttsVolumePct = (ttsActive && inSpeech) ? TTS_DUCK_PCT : TTS_VOLUME_PCT;

    updateLed();

    // Playback statistics: PLAYED should grow ~44100 bytes/s
    static unsigned long lastStats = 0;

    if (ttsActive && millis() - lastStats > 2000)
    {
        lastStats = millis();

        Serial.printf(
            "[play] rx=%u played=%u buffered=%u heap=%u\n",
            (unsigned)ttsRxBytes,
            (unsigned)ttsPlayedBytes,
            (unsigned)xStreamBufferBytesAvailable(ttsStream),
            (unsigned)ESP.getFreeHeap()
        );
    }

    // Memory monitor
    static unsigned long lastMemoryPrint = 0;

    if (millis() - lastMemoryPrint > 10000)
    {
        lastMemoryPrint = millis();

        Serial.print("Free heap: ");
        Serial.print(ESP.getFreeHeap());

        Serial.print(" | TTS buffer: ");
        Serial.println(
            ttsStream
                ? (unsigned)xStreamBufferBytesAvailable(ttsStream)
                : 0u
        );
    }

    // i2s_read paces the loop only while the mic is actually being read.
    // During playback (or when not listening) yield so WiFi/WebSocket
    // get time and the loop does not spin at 100% CPU.
    delay(1);
}
