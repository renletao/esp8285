#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// OLED Enable/Disable (comment out to disable OLED)
//#define ENABLE_OLED

#ifdef ENABLE_OLED
#include <Wire.h>
#include <U8g2lib.h>
// OLED Configuration (GPIO12/GPIO14 repurposed as SCAN_TRIGGER/BUTTON, re-enable OLED needs different SCL/SDA pins)
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0,/*scl*/12,/*sda*/14, /* reset=*/ U8X8_PIN_NONE);
#endif

// WiFi Configuration (input via serial)
String WIFI_SSID = "";
String WIFI_PASSWORD = "";

#define ALARM_PIN 13
#define GREEN_LED 4
#define BLUE_LED 2
#define SCAN_TRIGGER 12
#define SCAN_INTERRUPT 5
#define BUTTON_PIN 14

// EEPROM addresses
#define EEPROM_SIZE 512
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_MAGIC_VAL 0xAA
#define EEPROM_EXTIMODE_ADDR 1
#define EEPROM_SSID_ADDR 2
#define EEPROM_PASS_ADDR 34
#define EEPROM_MAX_SSID 32
#define EEPROM_MAX_PASS 64

const int OUTTIME = 1;
// API Configuration
String API_URL = "";  // Dynamically generated
const char* API_PATH = "/api/externalinterface/addMaterialBoxScanningRecord";

const int API_LAST_OCTET = 96;  // Last octet of API server IP

void buzzerBeep(unsigned long durationMs);
// Buzzer beep at 500ms interval for given duration
void buzzerBeepAlway(unsigned long durationMs);

// Build API URL based on WiFi IP (use first 3 octets from local IP)
void buildAPIURL() {
  int API_PORT = 90;
  IPAddress localIP = WiFi.localIP();
  if(localIP[2]==2)
    API_PORT=92;
  API_URL = "http://";
  API_URL += localIP[0];
  API_URL += ".";
  API_URL += localIP[1];
  API_URL += ".";
  API_URL += localIP[2];
  API_URL += ".";
  API_URL += API_LAST_OCTET;
  API_URL += ":";
  API_URL += API_PORT;
  API_URL += API_PATH;
  
  Serial.print("API URL: ");
  Serial.println(API_URL);
}

// Global Variables
String inputBuffer = "";
String lastSentData = "";
unsigned long lastReceiveTime = 0;
const unsigned long TIMEOUT_MS = 500;
bool wifiConnected = false;
bool apiSent = false;

// Scan interrupt flag and debug counter
volatile bool scanTriggered = false;
volatile unsigned long interruptCount = 0;
unsigned long lastDebugPrint = 0;
int extiMode = FALLING;  // default interrupt mode

void IRAM_ATTR onScanInterrupt() {
  scanTriggered = true;
//  interruptCount++;
}

// WiFi Initialization
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(BLUE_LED, HIGH);
  } else {
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(BLUE_LED, LOW);
  }
}

// Clean invalid UTF-8 characters from string
String cleanUTF8(String input) {
  String result = "";
  for (int i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c >= 32 && c <= 126) {
      result += c;
    }
  }
  return result;
}

// Send data to API with retry mechanism
bool sendToAPI(String data) {
  if (!wifiConnected) return false;
  
  const int MAX_RETRIES = 2;
  const int RETRY_DELAY_MS = 1000;
  
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    WiFiClient client;
    HTTPClient http;
    
    if (!http.begin(client, API_URL)) {
      Serial.println("HTTP begin failed");
      http.end();
      if (attempt < MAX_RETRIES) {
        Serial.print("Retry in ");
        Serial.print(RETRY_DELAY_MS);
        Serial.println("ms...");
        delay(RETRY_DELAY_MS);
        continue;
      }
      return false;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    String cleanData = cleanUTF8(data);
    String jsonPayload = "{\"qrcode\":\"" + cleanData + "\"}";
    Serial.print("Sending (attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_RETRIES);
    Serial.print("): ");
    Serial.println(jsonPayload);
    
    int httpCode = http.POST(jsonPayload);
    
    Serial.print("HTTP Code: ");
    Serial.println(httpCode);
    
    if (httpCode > 0) {
      String response = http.getString();
      Serial.print("API Response: ");
      Serial.println(response);
      http.end();
      
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);
      if (!error && doc.containsKey("data")) {
        float apiData = doc["data"].as<float>();
        Serial.print("API Data: ");
        Serial.println(apiData);
        if ((apiData < OUTTIME)&&(apiData>=0)) {
          Serial.println("Invalid data, buzzer 10s...");
          buzzerBeepAlway(3000);
        } else {
          Serial.println("Valid data, buzzer 3s...");
          buzzerBeep(3000);
        }
      }
      
      return true;
    } else {
      Serial.print("API Request Failed (attempt ");
      Serial.print(attempt);
      Serial.println(")");
      http.end();
      
      if (attempt < MAX_RETRIES) {
        Serial.print("Retrying in ");
        Serial.print(RETRY_DELAY_MS);
        Serial.println("ms...");
        delay(RETRY_DELAY_MS);
      }
    }
  }
  
  Serial.println("All retries failed");
  return false;
}

// Buzzer beep at 500ms interval for given duration
void buzzerBeep(unsigned long durationMs) {
  pinMode(ALARM_PIN, OUTPUT);
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    digitalWrite(ALARM_PIN, HIGH);
    delay(500);
    digitalWrite(ALARM_PIN, LOW);
    delay(500);
  }
}
// Buzzer beep at 500ms interval for given duration
void buzzerBeepAlway(unsigned long durationMs) {
  pinMode(ALARM_PIN, OUTPUT);
  unsigned long start = millis();
  digitalWrite(ALARM_PIN, HIGH);
  while (millis() - start < durationMs) {
    delay(1000);
    //delay(500);
  }
  digitalWrite(ALARM_PIN, LOW);
}

// ================== EEPROM Config ==================
void saveConfigToEEPROM(String ssid, String pass, int mode) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.write(EEPROM_EXTIMODE_ADDR, mode == RISING ? 1 : 0);

  for (int i = 0; i < EEPROM_MAX_SSID; i++) {
    EEPROM.write(EEPROM_SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  }
  for (int i = 0; i < EEPROM_MAX_PASS; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Config saved to EEPROM");
}

bool loadConfigFromEEPROM(String &ssid, String &pass, int &mode) {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    EEPROM.end();
    return false;
  }
  mode = (EEPROM.read(EEPROM_EXTIMODE_ADDR) == 1) ? RISING : FALLING;
  ssid = "";
  for (int i = 0; i < EEPROM_MAX_SSID && EEPROM.read(EEPROM_SSID_ADDR + i) != 0; i++) {
    ssid += (char)EEPROM.read(EEPROM_SSID_ADDR + i);
  }
  pass = "";
  for (int i = 0; i < EEPROM_MAX_PASS && EEPROM.read(EEPROM_PASS_ADDR + i) != 0; i++) {
    pass += (char)EEPROM.read(EEPROM_PASS_ADDR + i);
  }
  EEPROM.end();
  return ssid.length() > 0 && pass.length() > 0;
}

// ================== WiFi Config from Serial ==================
void connectWiFiFromSerial() {
  digitalWrite(SCAN_TRIGGER, LOW);
  Serial.println("\nEnter WiFi config (SSID+PASSWORD+GPIO5EXTIMOD): ");
  Serial.println("GPIO5EXTIMOD: 1=rising edge, 0=falling edge");
  while (!Serial.available()) {
    delay(100);
#ifdef ENABLE_OLED
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(0, 10);
    u8g2.print("Enter WiFi config");
    u8g2.setCursor(0, 40);
    u8g2.print("SSID+PW+MODE:");
    u8g2.sendBuffer();
#endif
  }

  String config = Serial.readStringUntil('\n');
  config.trim();
  Serial.print("Received config: '");
  Serial.print(config);
  Serial.println("'");

  int firstPlus = config.indexOf('+');
  int secondPlus = config.indexOf('+', firstPlus + 1);
  if (firstPlus > 0 && secondPlus > firstPlus + 1) {
    WIFI_SSID = config.substring(0, firstPlus);
    WIFI_PASSWORD = config.substring(firstPlus + 1, secondPlus);
    String extiMod = config.substring(secondPlus + 1);
    extiMod.trim();

    Serial.print("SSID: "); Serial.println(WIFI_SSID);
    Serial.print("Password: "); Serial.println(WIFI_PASSWORD);
    Serial.print("GPIO5EXTIMOD: "); Serial.println(extiMod);

    detachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT));
    if (extiMod == "1") {
      extiMode = RISING;
      attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, RISING);
      Serial.println("GPIO5 interrupt mode: RISING");
    } else {
      extiMode = FALLING;
      attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, FALLING);
      Serial.println("GPIO5 interrupt mode: FALLING");
    }

    saveConfigToEEPROM(WIFI_SSID, WIFI_PASSWORD, extiMode);

    Serial.println("WiFi connecting...");
    initWiFi();
    if (wifiConnected) {
      Serial.print("WiFi connected! IP: ");
      Serial.println(WiFi.localIP().toString());
      buildAPIURL();
    } else {
      Serial.println("WiFi connection failed");
    }
  } else {
    Serial.println("Invalid format! Use: SSID+PASSWORD+GPIO5EXTIMOD");
  }
  digitalWrite(SCAN_TRIGGER, HIGH);
}

// Serial Receive Handling
void handleSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (millis() - lastReceiveTime > TIMEOUT_MS) {
      inputBuffer = "";
    }
    inputBuffer += c;
    Serial.print("Received char: 0x");
    Serial.println((unsigned char)c, HEX);
    lastReceiveTime = millis();
  }
  
  if (inputBuffer.length() > 0 && millis() - lastReceiveTime > TIMEOUT_MS) {
    Serial.print("Buffer timeout, sending: '");
    Serial.print(inputBuffer);
    Serial.println("' (length: " + String(inputBuffer.length()) + ")");
    bool success = sendToAPI(inputBuffer);
    if (success) {
      Serial.println("API Request Sent");
      lastSentData = inputBuffer;
      apiSent = true;
    } else {
      Serial.println("API Request Failed");
    }
    inputBuffer = "";
    digitalWrite(SCAN_TRIGGER, HIGH);
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
  }
}

#ifdef ENABLE_OLED
// OLED Display Handling
void handleOLED() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 10);
  u8g2.print("WiFi: ");
  u8g2.print(WiFi.SSID());
  
  u8g2.setCursor(0, 26);
  if (apiSent) {
    u8g2.print("API: Sent");
  } else {
    u8g2.print("API: Ready");
  }
  
  u8g2.setCursor(0, 42);
  u8g2.print("RX: ");
  if (inputBuffer.length() > 0) {
    u8g2.print(inputBuffer);
  } else {
    u8g2.print(lastSentData);
  }
  
  u8g2.setCursor(0, 58);
  if (wifiConnected) {
    u8g2.print(WiFi.localIP().toString());
  } else {
    u8g2.print("IP: ---");
  }
  
  u8g2.sendBuffer();
}
#else
void handleOLED() {}
#endif

void setup() {
  Serial.begin(9600);
  delay(3000);
  
  // Send init hex: 44 43 4D 4F 4D 41 4E 55
  byte connectedData[] = {0x44, 0x43, 0x4D, 0x4F, 0x4D, 0x41, 0x4E, 0x55};
  Serial.write(connectedData, sizeof(connectedData));
  
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BLUE_LED, HIGH);
  
  pinMode(SCAN_TRIGGER, OUTPUT);
  pinMode(SCAN_INTERRUPT, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(SCAN_TRIGGER, HIGH);
  
#ifdef ENABLE_OLED
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.enableUTF8Print();
#endif
  
  // Try to load saved config from EEPROM and auto-connect
  String savedSSID, savedPass;
  int savedMode;
  if (loadConfigFromEEPROM(savedSSID, savedPass, savedMode)) {
    Serial.println("Found saved WiFi config, auto-connecting...");
    WIFI_SSID = savedSSID;
    WIFI_PASSWORD = savedPass;
    extiMode = savedMode;
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
    Serial.print("GPIO5 interrupt mode: ");
    Serial.println(extiMode == RISING ? "RISING" : "FALLING");
    
    initWiFi();
    if (wifiConnected) {
      Serial.print("Auto-connected! IP: ");
      Serial.println(WiFi.localIP().toString());
      buildAPIURL();
    }
  }
  
  // Check button to enter WiFi config mode
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button pressed, entering WiFi config mode...");
      connectWiFiFromSerial();
    }
  }
}

void loop() {
  handleSerial();
  handleOLED();
  
  if (scanTriggered) {
    scanTriggered = false;
    Serial.println("in");
    detachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT));
    digitalWrite(SCAN_TRIGGER, LOW);
  }
  
  // Check button for re-configuration
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      connectWiFiFromSerial();
    }
  }
  
  delay(100);
}
