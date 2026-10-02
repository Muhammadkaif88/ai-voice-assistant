/*
 ============================================================================
  Project: ESP32 AI Voice Assistant (Ultra Stable - No Reset Loops)
  Features:
    - Brownout Detector Disabled (Prevents sudden power-drop resets)
    - Captive Portal (WiFiManager): AP Hotspot "AI-Voice-Bot" (Password: 12345678)
    - Web Config for WiFi SSID, Password & Render Server Domain
    - Click-to-Talk (Press BOOT once to start, Press once to send)
    - INMP441 I2S Microphone (Port 0)
    - MAX98357A I2S Amplifier + Speaker (Port 1)
    - 0.96" SSD1306 OLED (I2C)
 ============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>          // WiFiManager by tzapu
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Disable ESP32 Brownout Reset
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ----------------- HARDWARE PINS -----------------
#define BUTTON_PIN 0 // BOOT button

// 1. INMP441 Microphone (I2S Port 0)
#define I2S_MIC_WS   25
#define I2S_MIC_SD   32
#define I2S_MIC_SCK  33

// 2. MAX98357A Amplifier (I2S Port 1)
#define I2S_SPK_BCLK 26
#define I2S_SPK_LRC  27
#define I2S_SPK_DIN  14

// 3. OLED Display (I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----------------- CONFIGURATION STORAGE -----------------
Preferences preferences;
char server_host[80] = "ai-voice-assistant.onrender.com";
char server_port[6]  = "443";
bool shouldSaveConfig = false;

// ----------------- GLOBAL OBJECTS -----------------
WebSocketsClient webSocket;

enum BotState { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_SPEAKING, STATE_PORTAL };
BotState currentState = STATE_PORTAL;

bool isRecording = false;
#define MIC_BUFFER_SIZE 1024
uint8_t micBuffer[MIC_BUFFER_SIZE];

bool i2sInitialized = false;

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
  display.display();
}

// ----------------- I2S INITIALIZATION -----------------
void initI2SPeripherals() {
  if (i2sInitialized) return;

  // 1. INMP441 Microphone (Port 0 - RX)
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

  // 2. MAX98357A Speaker (Port 1 - TX)
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

// ----------------- WEBSOCKET HANDLER -----------------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from server");
      break;
      
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to AI Voice Server!");
      currentState = STATE_IDLE;
      drawEyes(currentState);
      break;
      
    case WStype_TEXT: {
      StaticJsonDocument<512> doc;
      deserializeJson(doc, payload);
      const char* msgType = doc["type"];
      
      if (strcmp(msgType, "state") == 0) {
        const char* val = doc["value"];
        if (strcmp(val, "idle") == 0) currentState = STATE_IDLE;
        else if (strcmp(val, "listening") == 0) currentState = STATE_LISTENING;
        else if (strcmp(val, "thinking") == 0) currentState = STATE_THINKING;
        else if (strcmp(val, "speaking") == 0) currentState = STATE_SPEAKING;
        drawEyes(currentState);
      }
      else if (strcmp(msgType, "audio_end") == 0) {
        Serial.println("[WS] Audio stream finished");
        currentState = STATE_IDLE;
        drawEyes(currentState);
      }
      break;
    }
    
    case WStype_BIN: {
      if (length > 0) {
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_1, (const char*)payload, length, &bytesWritten, portMAX_DELAY);
      }
      break;
    }
  }
}

void saveConfigCallback() {
  Serial.println("Config updated by user.");
  shouldSaveConfig = true;
}

// ----------------- WIFI SETUP -----------------
void connectToCloudServer() {
  String hostStr = String(server_host);
  hostStr.replace("http://", "");
  hostStr.replace("https://", "");
  hostStr.replace("ws://", "");
  hostStr.replace("wss://", "");
  int slashIdx = hostStr.indexOf('/');
  if (slashIdx != -1) {
    hostStr = hostStr.substring(0, slashIdx);
  }
  hostStr.trim();
  hostStr.toCharArray(server_host, 80);

  int port = atoi(server_port);
  bool isCloudDomain = (hostStr.indexOf(".onrender.com") != -1 || hostStr.indexOf(".app") != -1 || hostStr.indexOf(".com") != -1 || port == 443);

  if (isCloudDomain) {
    Serial.printf("[WS] Connecting Secure WSS: %s:443/ws\n", server_host);
    webSocket.beginSSL(server_host, 443, "/ws");
  } else {
    Serial.printf("[WS] Connecting WS: %s:%d/ws\n", server_host, port);
    webSocket.begin(server_host, port, "/ws");
  }
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  initI2SPeripherals();
  currentState = STATE_IDLE;
  drawEyes(currentState);
}

void setupWiFiManager() {
  drawEyes(STATE_PORTAL);
  
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter custom_server_host("server", "Render Domain (e.g. your-app.onrender.com)", server_host, 80);
  WiFiManagerParameter custom_server_port("port", "Port (443 for Render)", server_port, 6);
  
  wm.addParameter(&custom_server_host);
  wm.addParameter(&custom_server_port);

  Serial.println("[WiFi] Starting WiFiManager (AI-Voice-Bot)...");
  
  // Start AP & wait until user configures WiFi (No restart loop!)
  if (wm.autoConnect("AI-Voice-Bot", "12345678")) {
    Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString());
    
    strcpy(server_host, custom_server_host.getValue());
    strcpy(server_port, custom_server_port.getValue());

    if (shouldSaveConfig) {
      preferences.begin("voicebot", false);
      preferences.putString("server_host", server_host);
      preferences.putString("server_port", server_port);
      preferences.end();
      Serial.println("[Config] Saved server settings.");
    }

    connectToCloudServer();
  }
}

// ----------------- SETUP -----------------
void setup() {
  // Disable Brownout reset so weak USB doesn't reboot ESP32
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // OLED Init
  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init error");
  }

  // Load Saved Host
  preferences.begin("voicebot", true);
  String savedHost = preferences.getString("server_host", "ai-voice-assistant.onrender.com");
  String savedPort = preferences.getString("server_port", "443");
  savedHost.toCharArray(server_host, 80);
  savedPort.toCharArray(server_port, 6);
  preferences.end();

  // Setup WiFi
  setupWiFiManager();
}

// ----------------- LOOP -----------------
unsigned long lastDebounceTime = 0;
int lastButtonState = HIGH;
int currentButtonState = HIGH;
unsigned long recordingStartTime = 0;

void loop() {
  webSocket.loop();

  int reading = digitalRead(BUTTON_PIN);

  // Debounce
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > 80) {
    if (reading != currentButtonState) {
      currentButtonState = reading;

      // When BOOT button is clicked
      if (currentButtonState == LOW) {
        if (!isRecording) {
          isRecording = true;
          recordingStartTime = millis();
          currentState = STATE_LISTENING;
          drawEyes(currentState);
          webSocket.sendTXT("{\"event\":\"START_RECORDING\"}");
          Serial.println("[Voice] Started recording...");
        } else {
          isRecording = false;
          webSocket.sendTXT("{\"event\":\"STOP_RECORDING\"}");
          Serial.println("[Voice] Stopped recording. Sending to AI...");
          currentState = STATE_THINKING;
          drawEyes(currentState);
        }
      }
    }
  }

  lastButtonState = reading;

  // Auto-stop recording if 12 seconds passed
  if (isRecording && (millis() - recordingStartTime > 12000)) {
    isRecording = false;
    webSocket.sendTXT("{\"event\":\"STOP_RECORDING\"}");
    Serial.println("[Voice] Max time reached. Sent to AI...");
    currentState = STATE_THINKING;
    drawEyes(currentState);
  }

  // Stream Mic Audio while speaking
  if (isRecording) {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, (void*)micBuffer, MIC_BUFFER_SIZE, &bytesRead, portMAX_DELAY);
    if (bytesRead > 0) {
      webSocket.sendBIN(micBuffer, bytesRead);
    }
  }
}
