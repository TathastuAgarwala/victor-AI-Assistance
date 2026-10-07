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
