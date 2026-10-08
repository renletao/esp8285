/*******
 * 添加正在联网红灯闪烁，蜂鸣器播放“两声一组”的较长节奏
 * 联网成功红灯熄灭，绿灯常亮，蜂鸣器关闭
 * 联网失败三轮联网尝试全部失败后，红灯常亮，蜂鸣器播放“三声一组”的急促节奏
 * 数据正在上传蓝灯按约 150 ms 周期闪烁
 * 上传成功蓝灯熄灭，绿灯保持常亮，蜂鸣器响两声
 * 上传失败WiFi 仍在线时蓝灯常亮，蜂鸣器快速响十声；下一次有效扫码时清除蓝灯和提示音
 * 上电扫描附近 WiFi；联网最终失败后每 10 秒重新扫描并打印网络和信号强度
 * ***** */

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
#define RED_LED 15
#define SCAN_TRIGGER 12
#define SCAN_INTERRUPT 5
#define BUTTON_PIN 14

#define LED_ON LOW
#define LED_OFF HIGH
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW

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
String DEVICE_MAC = "";  // Cached once at startup for API reports
const char* API_PATH = "/api/externalinterface/addmaterialBoxScanningRecord2";

const int API_LAST_OCTET = 96;  // Last octet of API server IP

enum ReportToneMode {
  REPORT_TONE_NONE,
  REPORT_TONE_SUCCESS,
  REPORT_TONE_FAILURE
};

ReportToneMode reportToneMode = REPORT_TONE_NONE;
unsigned long reportToneStartedAt = 0;
bool uploadInProgress = false;
bool uploadFailed = false;
// WiFi 未连接时进一步区分“正在连接”和“多轮连接失败”。
bool wifiConnectFailed = false;

struct NearbyNetworkInfo {
  String ssid;
  int32_t rssi;
};

const int MAX_CACHED_NETWORKS = 32;
NearbyNetworkInfo nearbyNetworks[MAX_CACHED_NETWORKS];
int nearbyNetworkCount = 0;
int nearbyNetworkTotal = 0;
bool nearbyNetworkScanValid = false;
unsigned long lastFailedNetworkPrintAt = 0;
const unsigned long FAILED_NETWORK_PRINT_MS = 5000;//连网失败后每5秒扫描一次附近网络并打印

void updateStatusOutputs();
void startReportTone(bool success);
void stopReportTone();
void delayWithStatus(unsigned long durationMs);
void syncWiFiStatus();
void scanNearbyNetworks(const char* stage);
void scanNearbyNetworksAtStartup();
void printCachedNearbyNetworks(const char* stage);
void handleFailedNetworkLog();

// Build API URL based on WiFi IP (use first 3 octets from local IP)
bool buildAPIURL() {
  IPAddress localIP = WiFi.localIP();
  if (!localIP.isSet() || localIP[0] == 0) {
    API_URL = "";
    Serial.print("API地址未生成，本机IP无效：");
    Serial.println(localIP.toString());
    return false;
  }
  int API_PORT = (localIP[2] == 2) ? 92 : 84;
  API_URL = "http://";
  API_URL += localIP[0];
  API_URL += ".";
  API_URL += localIP[1];
  API_URL += ".";
  API_URL += localIP[2];
  API_URL += ".";
  API_URL += API_LAST_OCTET;  //12;//API_LAST_OCTET;
  API_URL += ":";
  API_URL += API_PORT;
  API_URL += API_PATH;

  Serial.print("API地址：");
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
const int EMPTY_SCAN_BACKOFF_AT = 10;             // 连续空扫几次开始退避
const unsigned long SCAN_TIMEOUT_MS = 5000;      // 触发后多久没收到数据就强制恢复
const unsigned int MAX_FRAME_LEN = 128;          // 单帧上限，防止缓冲区无限增长
const int SERIAL_BUDGET_PER_LOOP = 96;           // 每轮 loop 最多读多少字节
const unsigned long CONFIG_WAIT_MS = 15000;      // 配网等待输入的总超时
const unsigned long CONFIG_FRAME_IDLE_MS = 500;  // 配网命令的空闲成帧时间
const unsigned int MAX_CONFIG_LEN = 160;         // 配网命令长度上限
const unsigned long CONFIG_TRIG_LOW_MS = 1500;   // 配网期间触发信号拉低时长
const unsigned long CONFIG_TRIG_HIGH_MS = 300;   // 触发信号间歇时长，给模块恢复
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_MS = 500;         // 主循环里同步 WiFi 真实状态的间隔
const unsigned long RED_FLASH_MS = 300;
const unsigned long CONNECT_BEEP_ON_MS = 180;
const unsigned long CONNECT_BEEP_OFF_MS = 120;
const unsigned long CONNECT_BEEP_GAP_MS = 700;
const int CONNECT_BEEP_COUNT = 2;
const unsigned long FAILED_BEEP_ON_MS = 80;
const unsigned long FAILED_BEEP_OFF_MS = 80;
const unsigned long FAILED_BEEP_GAP_MS = 650;
const int FAILED_BEEP_COUNT = 3;
const unsigned long SUCCESS_BEEP_ON_MS = 120;
const unsigned long SUCCESS_BEEP_OFF_MS = 100;
const int SUCCESS_BEEP_COUNT = 2;
const unsigned long FAILURE_BEEP_ON_MS = 80;
const unsigned long FAILURE_BEEP_OFF_MS = 80;
const int FAILURE_BEEP_COUNT = 10;

void IRAM_ATTR onScanInterrupt() {
  scanTriggered = true;
//  interruptCount++;
}

// 非阻塞播放一组短音，返回当前时刻是否应该让蜂鸣器响。
bool isBuzzerSequenceOn(unsigned long now, unsigned long onMs,
                        unsigned long offMs, int count, unsigned long gapMs) {
  unsigned long sequenceMs = (unsigned long)count * onMs;
  if (count > 1) sequenceMs += (unsigned long)(count - 1) * offMs;
  unsigned long phase = now % (sequenceMs + gapMs);

  for (int i = 0; i < count; i++) {
    if (phase < onMs) return true;
    phase -= onMs;
    if (i < count - 1) {
      if (phase < offMs) return false;
      phase -= offMs;
    }
  }
  return false;
}

int rssiToQuality(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void cacheNearbyNetworks(int networkCount) {
  nearbyNetworkTotal = networkCount;
  nearbyNetworkCount = min(networkCount, MAX_CACHED_NETWORKS);
  nearbyNetworkScanValid = true;

  for (int i = 0; i < nearbyNetworkCount; i++) {
    nearbyNetworks[i].ssid = WiFi.SSID(i);
    nearbyNetworks[i].rssi = WiFi.RSSI(i);
  }
}

void printCachedNearbyNetworks(const char* stage) {
  Serial.print("[WiFi扫描][");
  Serial.print(stage);
  Serial.print("] ");

  if (!nearbyNetworkScanValid) {
    Serial.println("没有有效的扫描结果");
    return;
  }

  Serial.print(nearbyNetworkTotal);
  Serial.println(" 个网络");
  for (int i = 0; i < nearbyNetworkCount; i++) {
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(". SSID=\"");
    Serial.print(nearbyNetworks[i].ssid.length() > 0 ? nearbyNetworks[i].ssid : "<隐藏网络>");
    Serial.print("\", 信号强度=");
    Serial.print(nearbyNetworks[i].rssi);
    Serial.print(" dBm，质量=");
    Serial.print(rssiToQuality(nearbyNetworks[i].rssi));
    Serial.println("%");
  }

  if (nearbyNetworkTotal > nearbyNetworkCount) {
    Serial.print("  ... ");
    Serial.print(nearbyNetworkTotal - nearbyNetworkCount);
    Serial.println(" 个网络未显示");
  }
}

void scanNearbyNetworks(const char* stage) {
  Serial.print("[WiFi扫描][");
  Serial.print(stage);
  Serial.println("] 正在扫描附近的 WiFi...");
  WiFi.scanDelete();
  int result = WiFi.scanNetworks(true, true);
  unsigned long scanStartedAt = millis();

  while (result == WIFI_SCAN_RUNNING && millis() - scanStartedAt < 10000) {
    updateStatusOutputs();
    delay(20);
    result = WiFi.scanComplete();
  }

  if (result >= 0) {
    cacheNearbyNetworks(result);
    printCachedNearbyNetworks(stage);
    WiFi.scanDelete();
  } else if (result == WIFI_SCAN_RUNNING) {
    Serial.print("[WiFi扫描][");
    Serial.print(stage);
    Serial.println("] 扫描超时");
  } else {
    Serial.print("[WiFi扫描][");
    Serial.print(stage);
    Serial.println("] 扫描失败");
  }
}

// 从扫描结果中找出与已配置 SSID 同名且信号最强的 AP。
// 返回的 BSSID 必须复制出来，避免 scanDelete() 后指针失效。
bool findStrongestConfiguredNetwork(int32_t &bestRssi,
                                    int32_t &bestChannel,
                                    uint8_t bestBssid[6]) {
  bestRssi = -127;
  bestChannel = 0;
  bool found = false;

  WiFi.scanDelete();
  int result = WiFi.scanNetworks(true, true);
  unsigned long scanStartedAt = millis();
  while (result == WIFI_SCAN_RUNNING && millis() - scanStartedAt < 10000) {
    updateStatusOutputs();
    delay(20);
    result = WiFi.scanComplete();
  }

  if (result < 0) {
    WiFi.scanDelete();
    return false;
  }

  for (int i = 0; i < result; i++) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;

    uint8_t *bssid = WiFi.BSSID(i);
    int32_t channel = WiFi.channel(i);
    int32_t rssi = WiFi.RSSI(i);
    if (bssid == nullptr || channel <= 0) continue;

    if (!found || rssi > bestRssi) {
      found = true;
      bestRssi = rssi;
      bestChannel = channel;
      for (int byteIndex = 0; byteIndex < 6; byteIndex++) {
        bestBssid[byteIndex] = bssid[byteIndex];
      }
    }
  }

  WiFi.scanDelete();
  return found;
}

void scanNearbyNetworksAtStartup() {
  scanNearbyNetworks("启动");
}

void handleFailedNetworkLog() {
  if (wifiConnected || !wifiConnectFailed) {
    lastFailedNetworkPrintAt = 0;
    return;
  }

  unsigned long now = millis();
  if (lastFailedNetworkPrintAt == 0 ||
      now - lastFailedNetworkPrintAt >= FAILED_NETWORK_PRINT_MS) {
    lastFailedNetworkPrintAt = now;
    scanNearbyNetworks("连接失败");
  }
}

const char* wifiStatusName(int status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "空闲";
    case WL_NO_SSID_AVAIL:   return "找不到SSID";
    case WL_CONNECT_FAILED:  return "关联或认证失败";
    case WL_CONNECTION_LOST: return "连接丢失";
    case WL_WRONG_PASSWORD:  return "密码错误";
    case WL_DISCONNECTED:    return "仍在连接，已超时";
    default:                 return "未知状态";
  }
}

// WiFi Initialization
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // 掉线由 SDK 自动重连，主循环只负责跟踪状态
  wifiConnected = false;
  wifiConnectFailed = false;
  lastFailedNetworkPrintAt = 0;
  uploadInProgress = false;
  uploadFailed = false;
  stopReportTone();
  updateStatusOutputs();

  int32_t strongestRssi = -127;
  int32_t strongestChannel = 0;
  uint8_t strongestBssid[6] = {0};
  bool hasStrongestNetwork = findStrongestConfiguredNetwork(
      strongestRssi, strongestChannel, strongestBssid);

  for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
    WiFi.disconnect();
    delayWithStatus(100);  // 紧接着 begin 会偶发连不上，断开需要时间处理完
    if (hasStrongestNetwork) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD, strongestChannel, strongestBssid);
    } else {
      // 扫描不到目标 SSID 时保留原有连接方式，避免改变无扫描结果时的行为。
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      updateStatusOutputs();
      delay(20);
    }
    // 卡在 CONNECTING 时再等也没用，重新 begin 才能重置射频状态机
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP().isSet()) {
      wifiConnected = true;
      updateStatusOutputs();
      Serial.print("WiFi连接成功，信号强度=");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      if (attempt > 1) {
        Serial.print("WiFi在第");
        Serial.print(attempt);
        Serial.println("次尝试时连接成功");
      }
      return;
    }
    Serial.print("WiFi第");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(WIFI_MAX_ATTEMPTS);
    Serial.print("次尝试失败，状态=");
    Serial.print(WiFi.status());
    Serial.print("（");
    Serial.print(wifiStatusName(WiFi.status()));
    Serial.println("）");
  }
  wifiConnected = false;
  wifiConnectFailed = true;
  updateStatusOutputs();

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
    Serial.println("API地址未设置，跳过上报");
    return false;
  }
  
  const int MAX_RETRIES = 2;
  const int RETRY_DELAY_MS = 1000;
  
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    WiFiClient client;
    HTTPClient http;
    
    if (!http.begin(client, API_URL)) {
      Serial.println("HTTP初始化失败");
      http.end();
      if (attempt < MAX_RETRIES) {
        Serial.print("将在");
        Serial.print(RETRY_DELAY_MS);
        Serial.println("毫秒后重试...");
        delayWithStatus(RETRY_DELAY_MS);
        continue;
      }
      return false;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    String cleanData = cleanUTF8(data);
    String jsonPayload = "{\"device\":\"" + DEVICE_MAC +
                         "\",\"qrcode\":\"" + cleanData + "\"}";
    Serial.print("正在发送（第");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_RETRIES);
    Serial.print("次，共");
    Serial.print(MAX_RETRIES);
    Serial.print("次）：");
    Serial.println(jsonPayload);
    
    int httpCode = http.POST(jsonPayload);
    
    Serial.print("HTTP状态码：");
    Serial.println(httpCode);
    
    if (httpCode >= 200 && httpCode < 300) {
      String response = http.getString();
      Serial.print("API响应：");
      Serial.println(response);
      http.end();
      
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);
      if (error || !doc.containsKey("data")) {
        Serial.println("API响应无效或缺少data字段");
        return false;
      }

      float apiData = doc["data"].as<float>();
      Serial.print("API返回数据：");
      Serial.println(apiData);
      
      return true;
    } else {
      Serial.print("API请求失败（第");
      Serial.print(attempt);
      Serial.println("次）");
      http.end();
      
      if (attempt < MAX_RETRIES) {
        Serial.print("将在");
        Serial.print(RETRY_DELAY_MS);
        Serial.println("毫秒后重试...");
        delayWithStatus(RETRY_DELAY_MS);
      }
    }
  }
  
  Serial.println("所有重试均失败");
  return false;
}

void updateStatusOutputs() {
  unsigned long now = millis();

  if (!wifiConnected) {
    /* 连接中：红灯闪烁、蜂鸣器滴滴；连接最终失败：红灯常亮、蜂鸣器嘀嘀嘀。 */
    if (wifiConnectFailed) {
      digitalWrite(RED_LED, LED_ON);
      digitalWrite(ALARM_PIN,
                   isBuzzerSequenceOn(now, FAILED_BEEP_ON_MS, FAILED_BEEP_OFF_MS,
                                       FAILED_BEEP_COUNT, FAILED_BEEP_GAP_MS)
                       ? BUZZER_ON
                       : BUZZER_OFF);
    } else {
      digitalWrite(RED_LED, ((now / RED_FLASH_MS) % 2 == 0) ? LED_ON : LED_OFF);
      digitalWrite(ALARM_PIN,
                   isBuzzerSequenceOn(now, CONNECT_BEEP_ON_MS, CONNECT_BEEP_OFF_MS,
                                       CONNECT_BEEP_COUNT, CONNECT_BEEP_GAP_MS)
                       ? BUZZER_ON
                       : BUZZER_OFF);
    }
    digitalWrite(GREEN_LED, LED_OFF);
    digitalWrite(BLUE_LED, LED_OFF);
    return;
  }

  /* 联网成功：红灯熄灭、绿灯常亮。 */
  digitalWrite(RED_LED, LED_OFF);
  digitalWrite(GREEN_LED, LED_ON);

  if (uploadFailed) {
    digitalWrite(BLUE_LED, LED_ON);
  } else if (uploadInProgress) {
    digitalWrite(BLUE_LED, ((now / 150) % 2 == 0) ? LED_ON : LED_OFF);
  } else {
    digitalWrite(BLUE_LED, LED_OFF);
  }

  if (reportToneMode == REPORT_TONE_SUCCESS) {
    const unsigned long cycleMs = SUCCESS_BEEP_ON_MS + SUCCESS_BEEP_OFF_MS;
    const unsigned long elapsed = now - reportToneStartedAt;
    if (elapsed < cycleMs * SUCCESS_BEEP_COUNT) {
      digitalWrite(ALARM_PIN,
                   (elapsed % cycleMs < SUCCESS_BEEP_ON_MS) ? BUZZER_ON : BUZZER_OFF);
    } else {
      stopReportTone();
    }
  } else if (reportToneMode == REPORT_TONE_FAILURE) {
    unsigned long cycleMs = FAILURE_BEEP_ON_MS + FAILURE_BEEP_OFF_MS;
    unsigned long elapsed = now - reportToneStartedAt;
    if (elapsed < cycleMs * FAILURE_BEEP_COUNT) {
      digitalWrite(ALARM_PIN,
                   (elapsed % cycleMs < FAILURE_BEEP_ON_MS) ? BUZZER_ON : BUZZER_OFF);
    } else {
      stopReportTone();
    }
  } else {
    digitalWrite(ALARM_PIN, BUZZER_OFF);
  }
}

void startReportTone(bool success) {
  if (!wifiConnected) return;
  reportToneMode = success ? REPORT_TONE_SUCCESS : REPORT_TONE_FAILURE;
  reportToneStartedAt = millis();
  updateStatusOutputs();
}

void stopReportTone() {
  reportToneMode = REPORT_TONE_NONE;
  reportToneStartedAt = 0;
  digitalWrite(ALARM_PIN, BUZZER_OFF);
}

void delayWithStatus(unsigned long durationMs) {
  unsigned long startedAt = millis();
  while (millis() - startedAt < durationMs) {
    updateStatusOutputs();
    delay(20);
  }
}

void syncWiFiStatus() {
  bool connectedNow = (WiFi.status() == WL_CONNECTED && WiFi.localIP().isSet());
  if (connectedNow == wifiConnected) return;

  wifiConnected = connectedNow;
  uploadInProgress = false;
  uploadFailed = false;
  stopReportTone();
  if (wifiConnected) {
    wifiConnectFailed = false;
    Serial.print("WiFi已重新连接，信号强度=");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    buildAPIURL();
  } else {
    // 从已连接状态掉线后进入自动重连阶段，仍按“正在连接”提示。
    wifiConnectFailed = false;
    API_URL = "";
    Serial.println("WiFi连接已断开");
  }
  updateStatusOutputs();
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
  Serial.println("配置已保存到EEPROM");
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
  Serial.println("\n请输入WiFi配置（SSID+密码+GPIO5中断沿模式）：");
  Serial.println("中断沿模式：1=上升沿，0=下降沿");

  // 触发信号做成占空循环，不连续拉低，避免扫码模块被长按锁死（照明常亮不再响应）
  unsigned long waitStart = millis();
  unsigned long phaseStart = millis();
  bool trigLow = true;
  digitalWrite(SCAN_TRIGGER, LOW);
  while (!Serial.available()) {
    updateStatusOutputs();
    if (millis() - waitStart > CONFIG_WAIT_MS) {
      digitalWrite(SCAN_TRIGGER, HIGH);
      Serial.println("配置输入超时，返回待机状态");
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
  Serial.print("收到配置：'");
  Serial.print(config);
  Serial.println("'");

  int firstPlus = config.indexOf('+');
  int secondPlus = config.indexOf('+', firstPlus + 1);
  if (firstPlus > 0 && secondPlus > firstPlus + 1) {
    WIFI_SSID = config.substring(0, firstPlus);
    WIFI_PASSWORD = config.substring(firstPlus + 1, secondPlus);
    String extiMod = config.substring(secondPlus + 1);
    extiMod.trim();

    Serial.print("SSID："); Serial.println(WIFI_SSID);
    Serial.print("密码："); Serial.println(WIFI_PASSWORD);
    Serial.print("GPIO5中断沿模式："); Serial.println(extiMod);

    detachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT));
    if (extiMod == "1") {
      extiMode = RISING;
      attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, RISING);
      Serial.println("GPIO5中断沿：上升沿");
    } else {
      extiMode = FALLING;
      attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, FALLING);
      Serial.println("GPIO5中断沿：下降沿");
    }

    Serial.println("WiFi正在连接...");
    initWiFi();
    if (wifiConnected) {
      Serial.print("WiFi连接成功！IP地址：");
      Serial.println(WiFi.localIP().toString());
      if (buildAPIURL()) {
        saveConfigToEEPROM(WIFI_SSID, WIFI_PASSWORD, extiMode);
      } else {
        Serial.println("配置未保存（连接成功后IP立即失效）");
      }
    } else {
      Serial.println("WiFi连接失败，配置未保存");
    }
  } else {
    Serial.println("格式无效！正确格式：SSID+密码+GPIO5中断沿模式");
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
    Serial.print("串口数据组帧超时，准备上报：'");
    Serial.print(inputBuffer);
    Serial.println("'（长度：" + String(inputBuffer.length()) + "）");
    uploadInProgress = true;
    uploadFailed = false;
    updateStatusOutputs();
    bool success = sendToAPI(inputBuffer);
    syncWiFiStatus();
    uploadInProgress = false;
    if (success) {
      Serial.println("API请求发送成功");
      lastSentData = inputBuffer;
      apiSent = true;
      uploadFailed = false;
      startReportTone(true);
    } else {
      Serial.println("API请求失败");
      uploadFailed = wifiConnected;
      startReportTone(false);
    }
    updateStatusOutputs();
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
  // 尽早将高电平有效的蜂鸣器拉低，缩短复位后到应用接管 GPIO13 的窗口。
  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, BUZZER_OFF);

  Serial.begin(9600);

  /* 上电后立即进入等待联网提示：三色灯低电平点亮，蜂鸣器高电平响。 */
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(RED_LED, LED_OFF);
  digitalWrite(GREEN_LED, LED_OFF);
  digitalWrite(BLUE_LED, LED_OFF);
  digitalWrite(ALARM_PIN, BUZZER_OFF);
  updateStatusOutputs();

  /* 初始化阶段只打印一次 MAC 地址。 */
  WiFi.mode(WIFI_STA);
  DEVICE_MAC = WiFi.macAddress();
  Serial.print("WiFi MAC地址：");
  Serial.println(DEVICE_MAC);

  // 扫描采用异步接口；等待期间仍刷新灯和蜂鸣器，不改变后续联网流程。
  scanNearbyNetworksAtStartup();

  delayWithStatus(3000);
  
  // Send init hex: 44 43 4D 4F 4D 41 4E 55
  byte connectedData[] = {0x44, 0x43, 0x4D, 0x4F, 0x4D, 0x41, 0x4E, 0x55};
  Serial.write(connectedData, sizeof(connectedData));
  
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
    Serial.println("发现已保存的WiFi配置，正在自动连接...");
    WIFI_SSID = savedSSID;
    WIFI_PASSWORD = savedPass;
    extiMode = savedMode;
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
    Serial.print("GPIO5中断沿：");
    Serial.println(extiMode == RISING ? "上升沿" : "下降沿");
    
    initWiFi();
    if (wifiConnected) {
      Serial.print("自动连接成功！IP地址：");
      Serial.println(WiFi.localIP().toString());
      buildAPIURL();
    }
  }
  
  // Check button to enter WiFi config mode
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("检测到按键，进入WiFi配置模式...");
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
      /* 新一轮扫码开始，退出上一笔上传失败提示，蓝灯恢复熄灭。 */
      uploadFailed = false;
      stopReportTone();
      updateStatusOutputs();
      Serial.println("开始扫码");
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
      Serial.print("扫码超时次数：");
      Serial.print(emptyScanStreak);
      Serial.print("，暂停扫描");
      Serial.print(SCAN_BACKOFF_MS / 1000);
      Serial.println("秒");
    } else {
      scanCooldownUntil = millis() + SCAN_COOLDOWN_MS;
      Serial.println("扫码超时，未读到数据，正在恢复");
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
    syncWiFiStatus();
  }

  handleFailedNetworkLog();
  updateStatusOutputs();
  delay(20);
}
