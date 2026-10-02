/*
 ============================================================================
  Project: ESP32 AI Voice Assistant (with WiFiManager & Captive Portal)
  Features:
    - Captive Portal (WiFiManager): Connect via phone hotspot setup
    - Web Config for WiFi SSID, Password & Python Server IP
    - Long-press Button (5s) to Reset WiFi / Change Network
    - INMP441 I2S Microphone (Speech Input)
    - MAX98357A I2S Amplifier + Speaker (Speech Output)
    - SSD1306 OLED Animated Eyes & Status
 ============================================================================
  Required Arduino Libraries:
    1. WiFiManager (by tzapu) -> Install from Library Manager
    2. ArduinoJson (by Benoit Blanchon)
    3. WebSockets (by Markus Sattler)
    4. Adafruit SSD1306 & Adafruit GFX Library
    5. ESP8266Audio (by Earle F. Philhower, III)
 ============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Audio playback libraries (ESP8266Audio)
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ----------------- HARDWARE PINS -----------------
// Push-to-talk button & Reset button (BOOT Button = GPIO 0)
#define BUTTON_PIN 0 

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
char server_host[40] = "192.168.1.100";
char server_port[6]  = "8000";

// Flag for saving new config
bool shouldSaveConfig = false;

// ----------------- GLOBAL OBJECTS -----------------
WebSocketsClient webSocket;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;

enum BotState { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_SPEAKING, STATE_PORTAL };
BotState currentState = STATE_IDLE;

bool isRecording = false;
#define MIC_BUFFER_SIZE 1024
uint8_t micBuffer[MIC_BUFFER_SIZE];

unsigned long buttonPressStartTime = 0;
bool buttonWasPressed = false;

// ----------------- OLED DISPLAY DRAWING -----------------
void drawEyes(BotState state) {
  display.clearDisplay();
  
  if (state == STATE_PORTAL) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("WiFi Setup Mode");
    display.println("---------------------");
    display.println("Connect to WiFi:");
    display.println(">> AI-Voice-Bot <<");
    display.println("Open IP: 192.168.4.1");
    display.display();
    return;
  }
  
  if (state == STATE_IDLE) {
    // Friendly open eyes
    display.fillRoundRect(28, 20, 26, 30, 8, SSD1306_WHITE);
    display.fillRoundRect(74, 20, 26, 30, 8, SSD1306_WHITE);
  } 
  else if (state == STATE_LISTENING) {
    // Big round listening eyes + label
    display.fillRoundRect(24, 14, 32, 38, 12, SSD1306_WHITE);
    display.fillRoundRect(72, 14, 32, 38, 12, SSD1306_WHITE);
    display.setCursor(30, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Listening...");
  } 
  else if (state == STATE_THINKING) {
    // Thinking eyes
    display.fillRoundRect(30, 15, 24, 18, 6, SSD1306_WHITE);
    display.fillRoundRect(74, 15, 24, 18, 6, SSD1306_WHITE);
    display.setCursor(35, 56);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.print("Thinking...");
  } 
  else if (state == STATE_SPEAKING) {
    // Happy squinting eyes + smile mouth
    display.fillRoundRect(28, 24, 26, 16, 6, SSD1306_WHITE);
    display.fillRoundRect(74, 24, 26, 16, 6, SSD1306_WHITE);
    display.drawLine(48, 50, 80, 50, SSD1306_WHITE);
    display.drawLine(52, 54, 76, 54, SSD1306_WHITE);
  }
  display.display();
}

// ----------------- I2S MICROPHONE INIT -----------------
void initI2SMic() {
  i2s_config_t i2s_config = {
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

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
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
        Serial.println("[WS] Audio stream playback finished");
      }
      break;
    }
    
    case WStype_BIN:
      // Binary MP3 audio chunks received for speaker
      break;
  }
}

// Callback notifying us of the need to save config
void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// ----------------- CAPTIVE PORTAL (WIFI MANAGER) -----------------
void startWiFiManager(bool forcePortal = false) {
  drawEyes(STATE_PORTAL);
  
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  // Custom parameters for Server IP and Port
  WiFiManagerParameter custom_server_host("server", "Python Server IP", server_host, 40);
  WiFiManagerParameter custom_server_port("port", "Server Port", server_port, 6);
  
  wm.addParameter(&custom_server_host);
  wm.addParameter(&custom_server_port);

  // Set timeout so it doesn't get stuck forever
  wm.setConfigPortalTimeout(180); // 3 minutes timeout

  bool res;
  if (forcePortal) {
    Serial.println("[WiFi] Starting Config Portal On Demand...");
    res = wm.startConfigPortal("AI-Voice-Bot");
  } else {
    Serial.println("[WiFi] Connecting or Auto-starting Portal...");
    res = wm.autoConnect("AI-Voice-Bot"); 
  }

  if (!res) {
    Serial.println("[WiFi] Failed to connect or hit timeout. Restarting...");
    ESP.restart();
  }

  // Read updated parameters
  strcpy(server_host, custom_server_host.getValue());
  strcpy(server_port, custom_server_port.getValue());

  // Save to persistent Flash memory (NVS Preferences)
  if (shouldSaveConfig) {
    preferences.begin("voicebot", false);
    preferences.putString("server_host", server_host);
    preferences.putString("server_port", server_port);
    preferences.end();
    Serial.println("[Config] New Server IP saved to Flash memory!");
  }

  Serial.println("[WiFi] Connected successfully! Local IP: " + WiFi.localIP().toString());
  
  // Connect WebSocket to configured Server
  int port = atoi(server_port);
  Serial.printf("[WS] Connecting to %s:%d/ws\n", server_host, port);
  webSocket.begin(server_host, port, "/ws");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  currentState = STATE_IDLE;
  drawEyes(currentState);
}

// ----------------- SETUP -----------------
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // OLED Init
  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  }

  // Load Saved Server IP from Flash
  preferences.begin("voicebot", true);
  String savedHost = preferences.getString("server_host", "192.168.1.100");
  String savedPort = preferences.getString("server_port", "8000");
  savedHost.toCharArray(server_host, 40);
  savedPort.toCharArray(server_port, 6);
  preferences.end();

  // Audio Amp Setup
  out = new AudioOutputI2S();
  out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DIN);
  out->SetGain(0.8);

  // Mic Setup
  initI2SMic();

  // Check if button is held at boot (Force WiFi Reset)
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("[WiFi] BOOT Button held during startup -> Resetting WiFi...");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 25);
    display.println("Resetting WiFi...");
    display.display();
    delay(2000);
    startWiFiManager(true);
  } else {
    startWiFiManager(false);
  }
}

// ----------------- LOOP -----------------
void loop() {
  webSocket.loop();

  int buttonState = digitalRead(BUTTON_PIN);

  // Check for Long Press (5 seconds) to Reset WiFi at runtime
  if (buttonState == LOW) {
    if (!buttonWasPressed) {
      buttonWasPressed = true;
      buttonPressStartTime = millis();
    } 
    else {
      unsigned long pressDuration = millis() - buttonPressStartTime;
      if (pressDuration > 5000) { // Held for 5 seconds
        Serial.println("[Reset] 5s long press -> Clearing WiFi & Opening Portal...");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(5, 25);
        display.println("WiFi Reset Triggered!");
        display.display();
        delay(1500);
        
        WiFiManager wm;
        wm.resetSettings(); // Clear saved WiFi
        ESP.restart();
      }
    }
  } 
  else {
    buttonWasPressed = false;
  }

  // Push-to-talk voice recording logic
  bool buttonPressed = (buttonState == LOW);

  if (buttonPressed && !isRecording && (millis() - buttonPressStartTime < 4500)) {
    isRecording = true;
    currentState = STATE_LISTENING;
    drawEyes(currentState);
    webSocket.sendTXT("{\"event\":\"START_RECORDING\"}");
    Serial.println("Recording Started...");
    delay(150);
  } 
  else if (!buttonPressed && isRecording) {
    isRecording = false;
    webSocket.sendTXT("{\"event\":\"STOP_RECORDING\"}");
    Serial.println("Recording Stopped. Sending to AI...");
    currentState = STATE_THINKING;
    drawEyes(currentState);
    delay(150);
  }

  // Stream Mic Audio chunks while speaking
  if (isRecording) {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, (void*)micBuffer, MIC_BUFFER_SIZE, &bytesRead, portMAX_DELAY);
    if (bytesRead > 0) {
      webSocket.sendBIN(micBuffer, bytesRead);
    }
  }

  // Handle MP3 playback loop
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      currentState = STATE_IDLE;
      drawEyes(currentState);
    }
  }
}
