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
