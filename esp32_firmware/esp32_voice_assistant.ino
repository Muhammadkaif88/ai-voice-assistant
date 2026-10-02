/*
 ============================================================================
  Project: ESP32 AI Voice Assistant (Ultra-Reliable HTTPS Streaming)
  Features:
    - 100% Hands-Free Auto-VAD voice detection
    - HTTPS Direct Audio Stream (Zero WebSocket disconnect / SSL errors)
    - Captive Portal (WiFiManager): AP "AI-Voice-Bot" (Pass: 12345678)
    - Native I2S for INMP441 Mic (Port 0) & MAX98357A Speaker (Port 1)
    - SSD1306 OLED Animated Expressions
 ============================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>          // WiFiManager by tzapu
#include <Preferences.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Disable Brownout Reset
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ----------------- HARDWARE PINS -----------------
#define BUTTON_PIN 0 // BOOT button (Optional manual click)

// 1. INMP441 Microphone (I2S Port 0 - Input)
#define I2S_MIC_WS   25
#define I2S_MIC_SD   32
#define I2S_MIC_SCK  33

// 2. MAX98357A Amplifier (I2S Port 1 - Output)
#define I2S_SPK_BCLK 26
#define I2S_SPK_LRC  27
#define I2S_SPK_DIN  14

// 3. OLED Display (I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----------------- VAD (VOICE DETECTION) CONFIG -----------------
#define VAD_THRESHOLD          1800   // Sensitivity threshold
#define SILENCE_TIMEOUT_MS     1300   // Silence duration before sending to AI
#define MIN_RECORDING_TIME_MS  800    // Minimum speech duration
#define MAX_AUDIO_BYTES        (16000 * 2 * 10) // 10 seconds max buffer

// ----------------- CONFIGURATION STORAGE -----------------
Preferences preferences;
char server_host[80] = "ai-voice-assistant-fu7m.onrender.com";
bool shouldSaveConfig = false;

enum BotState { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_SPEAKING, STATE_PORTAL, STATE_ERROR };
BotState currentState = STATE_PORTAL;

bool isRecording = false;
uint8_t *audioRecordBuffer = NULL;
size_t audioRecordSize = 0;

bool i2sInitialized = false;
unsigned long speechStartTime = 0;
unsigned long lastSoundTime = 0;

// ----------------- OLED DISPLAY DRAWING -----------------
void drawEyes(BotState state) {
  display.clearDisplay();
  
  if (state == STATE_PORTAL) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(">> WiFi Setup <<");
    display.println("---------------------");
    display.println("Connect to WiFi AP:");
    display.println("AI-Voice-Bot");
    display.println("Pass: 12345678");
    display.println("IP: 192.168.4.1");
    display.display();
    return;
  }
  
  if (state == STATE_IDLE) {
    display.fillRoundRect(28, 20, 26, 30, 8, SSD1306_WHITE);
    display.fillRoundRect(74, 20, 26, 30, 8, SSD1306_WHITE);
    display.setCursor(25, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Say something...");
  } 
  else if (state == STATE_LISTENING) {
    display.fillRoundRect(24, 14, 32, 38, 12, SSD1306_WHITE);
    display.fillRoundRect(72, 14, 32, 38, 12, SSD1306_WHITE);
    display.setCursor(30, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Listening...");
  } 
  else if (state == STATE_THINKING) {
    display.fillRoundRect(30, 15, 24, 18, 6, SSD1306_WHITE);
    display.fillRoundRect(74, 15, 24, 18, 6, SSD1306_WHITE);
    display.setCursor(35, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Thinking...");
  } 
  else if (state == STATE_SPEAKING) {
    display.fillRoundRect(28, 24, 26, 16, 6, SSD1306_WHITE);
    display.fillRoundRect(74, 24, 26, 16, 6, SSD1306_WHITE);
    display.drawLine(48, 50, 80, 50, SSD1306_WHITE);
    display.drawLine(52, 54, 76, 54, SSD1306_WHITE);
  }
  else if (state == STATE_ERROR) {
    display.fillRoundRect(30, 20, 24, 10, 3, SSD1306_WHITE);
    display.fillRoundRect(74, 20, 24, 10, 3, SSD1306_WHITE);
    display.setCursor(20, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Server Retry...");
  }
  display.display();
}

// ----------------- I2S INITIALIZATION -----------------
void initI2SPeripherals() {
  if (i2sInitialized) return;

  // 1. INMP441 Mic (Port 0)
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false
  };

  i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);

  // 2. MAX98357A Speaker (Port 1)
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = false
  };

  i2s_pin_config_t spk_pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_1, &spk_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &spk_pins);
  i2s_set_clk(I2S_NUM_1, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  i2sInitialized = true;
  Serial.println("[I2S] Audio hardware initialized.");
}

void saveConfigCallback() {
  Serial.println("Config updated by user.");
  shouldSaveConfig = true;
}

// ----------------- SEND AUDIO TO AI SERVER VIA HTTPS -----------------
void sendAudioToAIServer() {
  if (audioRecordSize < 1600) {
    Serial.println("[AI] Audio too short, ignoring.");
    currentState = STATE_IDLE;
    drawEyes(currentState);
    return;
  }

  Serial.printf("[AI] Sending %d bytes to Render AI Server (Free Heap: %d)...\n", audioRecordSize, ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();               // Disable strict SSL certificate validation
  client.setBufferSizes(2048, 1024);  // Drastically save TLS RAM footprint for ESP32
  client.setTimeout(25000);

  HTTPClient http;
  http.setReuse(false);
  String url = "https://" + String(server_host) + "/api/chat-voice";
  
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/octet-stream");
    
    int httpCode = http.POST(audioRecordBuffer, audioRecordSize);
    Serial.printf("[HTTP] Response code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      currentState = STATE_SPEAKING;
      drawEyes(currentState);

      WiFiClient *stream = http.getStreamPtr();
      uint8_t playBuffer[1024];
      
      while (http.connected() && stream->available()) {
        int bytesRead = stream->readBytes(playBuffer, sizeof(playBuffer));
        if (bytesRead > 0) {
          size_t bytesWritten = 0;
          i2s_write(I2S_NUM_1, (const char*)playBuffer, bytesRead, &bytesWritten, pdMS_TO_TICKS(100));
        }
      }
      Serial.println("[AI] Finished playing response.");
    } else {
      Serial.printf("[HTTP Error] Code: %d, Error: %s\n", httpCode, http.errorToString(httpCode).c_str());
      currentState = STATE_ERROR;
      drawEyes(currentState);
      delay(1500);
    }
    http.end();
  } else {
    Serial.println("[HTTP] Unable to connect to server.");
    currentState = STATE_ERROR;
    drawEyes(currentState);
    delay(1500);
  }

  // Reset back to IDLE
  audioRecordSize = 0;
  currentState = STATE_IDLE;
  drawEyes(currentState);
}

// ----------------- WIFI MANAGER -----------------
void setupWiFiManager() {
  drawEyes(STATE_PORTAL);
  
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter custom_server_host("server", "Render Domain", server_host, 80);
  wm.addParameter(&custom_server_host);

  Serial.println("[WiFi] Starting WiFiManager (AI-Voice-Bot)...");
  
  if (wm.autoConnect("AI-Voice-Bot", "12345678")) {
    Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString());
    
    String newHost = String(custom_server_host.getValue());
    newHost.trim();
    newHost.replace("https://", "");
    newHost.replace("http://", "");
    int slashIdx = newHost.indexOf('/');
    if (slashIdx != -1) newHost = newHost.substring(0, slashIdx);
    
    if (newHost.length() > 3) {
      newHost.toCharArray(server_host, 80);
    }

    if (shouldSaveConfig) {
      preferences.begin("voicebot", false);
      preferences.putString("server_host", server_host);
      preferences.end();
    }

    initI2SPeripherals();
    currentState = STATE_IDLE;
    drawEyes(currentState);
  }
}

// ----------------- SETUP -----------------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Allocate RAM buffer for audio recording (2-3 seconds)
  audioRecordBuffer = (uint8_t*)ps_malloc(MAX_AUDIO_BYTES);
  if (!audioRecordBuffer) {
    audioRecordBuffer = (uint8_t*)malloc(64000); // 2 seconds in internal RAM
  }

  // OLED Init
  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init error");
  }

  // Load Saved Host
  preferences.begin("voicebot", true);
  String savedHost = preferences.getString("server_host", "ai-voice-assistant-fu7m.onrender.com");
  if (savedHost.length() > 5) {
    savedHost.toCharArray(server_host, 80);
  }
  preferences.end();

  setupWiFiManager();
}

// ----------------- AUDIO RMS (VAD) -----------------
float calculateAudioRMS(int16_t *buffer, size_t samples) {
  if (samples == 0) return 0.0;
  int64_t sumSquares = 0;
  for (size_t i = 0; i < samples; i++) {
    int32_t val = buffer[i];
    sumSquares += (val * val);
  }
  return sqrt((float)sumSquares / samples);
}

// ----------------- MAIN LOOP -----------------
int16_t sampleChunk[256];

void loop() {
  if (currentState != STATE_IDLE && currentState != STATE_LISTENING) {
    delay(10);
    return;
  }

  size_t bytesRead = 0;
  if (i2sInitialized) {
    i2s_read(I2S_NUM_0, (void*)sampleChunk, sizeof(sampleChunk), &bytesRead, pdMS_TO_TICKS(20));
  }

  if (bytesRead > 0) {
    size_t samples = bytesRead / sizeof(int16_t);
    float rms = calculateAudioRMS(sampleChunk, samples);

    // 1. Idle -> User starts speaking
    if (!isRecording && currentState == STATE_IDLE) {
      if (rms > VAD_THRESHOLD) {
        isRecording = true;
        audioRecordSize = 0;
        speechStartTime = millis();
        lastSoundTime = millis();
        currentState = STATE_LISTENING;
        drawEyes(currentState);
        Serial.printf("[VAD] Voice Detected! (RMS: %.0f)\n", rms);
      }
    }

    // 2. Listening -> Store Audio in Buffer
    else if (isRecording) {
      if (audioRecordBuffer && (audioRecordSize + bytesRead < (32000 * 3))) {
        memcpy(audioRecordBuffer + audioRecordSize, sampleChunk, bytesRead);
        audioRecordSize += bytesRead;
      }

      if (rms > VAD_THRESHOLD) {
        lastSoundTime = millis();
      }

      unsigned long currentSpeechDuration = millis() - speechStartTime;
      unsigned long silenceDuration = millis() - lastSoundTime;

      // Stop speech when silence is detected
      if ((silenceDuration > SILENCE_TIMEOUT_MS && currentSpeechDuration > MIN_RECORDING_TIME_MS) || (currentSpeechDuration > 7000)) {
        isRecording = false;
        currentState = STATE_THINKING;
        drawEyes(currentState);
        Serial.printf("[VAD] Silence detected -> Processing (%d bytes)...\n", audioRecordSize);
        
        sendAudioToAIServer();
      }
    }
  }

  // Optional manual BOOT button
  if (digitalRead(BUTTON_PIN) == LOW && !isRecording && currentState == STATE_IDLE) {
    isRecording = true;
    audioRecordSize = 0;
    speechStartTime = millis();
    lastSoundTime = millis();
    currentState = STATE_LISTENING;
    drawEyes(currentState);
    Serial.println("[Button] Recording started...");
    delay(200);
  }
}
