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
const unsigned long WIFI_ATTEMPT_MS = 10000;  // 单轮连接超时
const int WIFI_MAX_ATTEMPTS = 3;              // 超时后重新 begin 的轮数
// API Configuration
String API_URL = "";  // Dynamically generated
const char* API_PATH = "/api/externalinterface/addMaterialBoxScanningRecord";

const int API_LAST_OCTET = 96;  // Last octet of API server IP

void buzzerBeep(unsigned long durationMs);
// Buzzer beep at 500ms interval for given duration
void buzzerBeepAlway(unsigned long durationMs);
void buzzerFail();

// Build API URL based on WiFi IP (use first 3 octets from local IP)
bool buildAPIURL() {
  IPAddress localIP = WiFi.localIP();
  if (!localIP.isSet() || localIP[0] == 0) {
    API_URL = "";
    Serial.print("API URL NOT built, local IP invalid: ");
    Serial.println(localIP.toString());
    return false;
  }
  int API_PORT = (localIP[2] == 2) ? 92 : 90;
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
  return true;
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

unsigned long scanActiveSince = 0;               // 0 = 不在扫码状态
unsigned long scanCooldownUntil = 0;             // 该时刻之前收到的触发全部丢弃
int emptyScanStreak = 0;                         // 连续几次触发后没读到数据
const unsigned long SCAN_COOLDOWN_MS = 500;      // 撤销触发后的重触发冷却
const unsigned long SCAN_BACKOFF_MS = 10000;     // 连续空扫后的退避时长
const int EMPTY_SCAN_BACKOFF_AT = 3;             // 连续空扫几次开始退避
const unsigned long SCAN_TIMEOUT_MS = 5000;      // 触发后多久没收到数据就强制恢复
const unsigned int MAX_FRAME_LEN = 128;          // 单帧上限，防止缓冲区无限增长
const int SERIAL_BUDGET_PER_LOOP = 96;           // 每轮 loop 最多读多少字节
const unsigned long CONFIG_WAIT_MS = 15000;      // 配网等待输入的总超时
const unsigned long CONFIG_FRAME_IDLE_MS = 500;  // 配网命令的空闲成帧时间
const unsigned int MAX_CONFIG_LEN = 160;         // 配网命令长度上限
const unsigned long CONFIG_TRIG_LOW_MS = 1500;   // 配网期间触发信号拉低时长
const unsigned long CONFIG_TRIG_HIGH_MS = 300;   // 触发信号间歇时长，给模块恢复
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_MS = 5000;        // 主循环里同步 WiFi 真实状态的间隔

void IRAM_ATTR onScanInterrupt() {
  scanTriggered = true;
//  interruptCount++;
}

const char* wifiStatusName(int status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_CONNECT_FAILED:  return "assoc/auth failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_WRONG_PASSWORD:  return "wrong password";
    case WL_DISCONNECTED:    return "still connecting (timeout)";
    default:                 return "unknown";
  }
}

// WiFi Initialization
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // 掉线由 SDK 自动重连，主循环只负责跟踪状态
  for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
    WiFi.disconnect();
    delay(100);  // 紧接着 begin 会偶发连不上，断开需要时间处理完
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      delay(500);
    }
    // 卡在 CONNECTING 时再等也没用，重新 begin 才能重置射频状态机
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP().isSet()) {
      wifiConnected = true;
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(BLUE_LED, HIGH);
      if (attempt > 1) {
        Serial.print("WiFi connected on attempt ");
        Serial.println(attempt);
      }
      return;
    }
    Serial.print("WiFi attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(WIFI_MAX_ATTEMPTS);
    Serial.print(" failed, status=");
    Serial.print(WiFi.status());
    Serial.print(" (");
    Serial.print(wifiStatusName(WiFi.status()));
    Serial.println(")");
  }
  wifiConnected = false;
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BLUE_LED, LOW);
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
  if (API_URL.length() == 0) {
    Serial.println("API URL not set, skip sending");
    return false;
  }
  
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

// 上报失败提示：急促三短声，与正常（间歇3声）和异常（长鸣3秒）都能明显区分
void buzzerFail() {
  pinMode(ALARM_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(ALARM_PIN, HIGH);
    delay(100);
    digitalWrite(ALARM_PIN, LOW);
    delay(100);
  }
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
  while (Serial.available()) Serial.read();  // 丢掉进入配网前的残留字节
  inputBuffer = "";                          // 半截条码不能留到配网结束后被当成整帧上报
  Serial.println("\nEnter WiFi config (SSID+PASSWORD+GPIO5EXTIMOD): ");
  Serial.println("GPIO5EXTIMOD: 1=rising edge, 0=falling edge");

  // 触发信号做成占空循环，不连续拉低，避免扫码模块被长按锁死（照明常亮不再响应）
  unsigned long waitStart = millis();
  unsigned long phaseStart = millis();
  bool trigLow = true;
  digitalWrite(SCAN_TRIGGER, LOW);
  while (!Serial.available()) {
    if (millis() - waitStart > CONFIG_WAIT_MS) {
      digitalWrite(SCAN_TRIGGER, HIGH);
      Serial.println("Config timeout, back to standby");
      return;
    }
    if (millis() - phaseStart >= (trigLow ? CONFIG_TRIG_LOW_MS : CONFIG_TRIG_HIGH_MS)) {
      trigLow = !trigLow;
      digitalWrite(SCAN_TRIGGER, trigLow ? LOW : HIGH);
      phaseStart = millis();
    }
    delay(10);
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

  // 收到第一个字节就撤销触发，否则扫码模块会持续重读并灌满串口
  digitalWrite(SCAN_TRIGGER, HIGH);

  // 按空闲超时成帧：CR / LF / CRLF / 无终止符都能正确收尾，且长度有上限
  String config = "";
  unsigned long lastByte = millis();
  while (millis() - lastByte < CONFIG_FRAME_IDLE_MS) {
    if (Serial.available()) {
      char c = Serial.read();
      lastByte = millis();
      if (c == '\r' || c == '\n') {
        if (config.length() > 0) break;
      } else if (config.length() < MAX_CONFIG_LEN) {
        config += c;
      }
    } else {
      delay(1);
    }
  }
  while (Serial.available()) Serial.read();  // 丢掉重复读出的多余帧

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

    Serial.println("WiFi connecting...");
    initWiFi();
    if (wifiConnected) {
      Serial.print("WiFi connected! IP: ");
      Serial.println(WiFi.localIP().toString());
      if (buildAPIURL()) {
        saveConfigToEEPROM(WIFI_SSID, WIFI_PASSWORD, extiMode);
      } else {
        Serial.println("Config NOT saved (IP lost right after connecting)");
      }
    } else {
      Serial.println("WiFi connection failed, config NOT saved");
    }
  } else {
    Serial.println("Invalid format! Use: SSID+PASSWORD+GPIO5EXTIMOD");
  }
  digitalWrite(SCAN_TRIGGER, HIGH);
}

// Serial Receive Handling
void handleSerial() {
  int budget = 0;
  while (Serial.available() > 0 && budget++ < SERIAL_BUDGET_PER_LOOP) {
    char c = Serial.read();
    if (millis() - lastReceiveTime > TIMEOUT_MS) {
      inputBuffer = "";
    }
    if (inputBuffer.length() < MAX_FRAME_LEN) {
      inputBuffer += c;
    }
    lastReceiveTime = millis();
    // 已经读到数据，立刻撤掉触发信号，否则扫码模块会持续重读并灌满串口
    if (scanActiveSince != 0) {
      digitalWrite(SCAN_TRIGGER, HIGH);
      scanActiveSince = 0;
      emptyScanStreak = 0;
    }
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
      buzzerFail();  // 上报没成功必须让现场听见，否则会当成扫上了
    }
    inputBuffer = "";
    digitalWrite(SCAN_TRIGGER, HIGH);
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
    scanTriggered = false;  // 丢掉重挂中断瞬间产生的触发
    scanCooldownUntil = millis() + SCAN_COOLDOWN_MS;
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
    // 冷却期内的触发一律丢弃：撤销触发信号时扫码模块状态脚的跳变会立刻自触发
    if (millis() >= scanCooldownUntil) {
      Serial.println("in");
      detachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT));
      digitalWrite(SCAN_TRIGGER, LOW);
      scanActiveSince = millis();
    }
  }

  // 触发后一直没读到数据：恢复 GPIO12 和中断，避免永久假死
  if (scanActiveSince != 0 && millis() - scanActiveSince > SCAN_TIMEOUT_MS) {
    scanActiveSince = 0;
    emptyScanStreak++;
    digitalWrite(SCAN_TRIGGER, HIGH);
    if (emptyScanStreak >= EMPTY_SCAN_BACKOFF_AT) {
      // 连续空扫说明 GPIO5 在自激或现场没有码，拉长冷却让模块真正休息
      scanCooldownUntil = millis() + SCAN_BACKOFF_MS;
      Serial.print("Scan timeout x");
      Serial.print(emptyScanStreak);
      Serial.print(", backing off ");
      Serial.print(SCAN_BACKOFF_MS / 1000);
      Serial.println("s");
    } else {
      scanCooldownUntil = millis() + SCAN_COOLDOWN_MS;
      Serial.println("Scan timeout, nothing read, recovering");
    }
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
    scanTriggered = false;  // 丢掉重挂中断瞬间产生的触发
  }
  
  // Check button for re-configuration
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      connectWiFiFromSerial();
    }
  }
  
  // 跟踪 WiFi 真实状态：SDK 负责自动重连，这里只同步标志、灯和 API 地址
  if (millis() - lastWifiCheck > WIFI_CHECK_MS) {
    lastWifiCheck = millis();
    bool up = (WiFi.status() == WL_CONNECTED && WiFi.localIP().isSet());
    if (up != wifiConnected) {
      wifiConnected = up;
      digitalWrite(GREEN_LED, up ? LOW : HIGH);
      digitalWrite(BLUE_LED, up ? HIGH : LOW);
      if (up) {
        Serial.println("WiFi reconnected");
        buildAPIURL();  // 重连后 IP 可能变了，地址要重新拼
      } else {
        API_URL = "";
        Serial.println("WiFi lost");
      }
    }
  }

  delay(100);
}
