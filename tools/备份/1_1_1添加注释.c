/*
 * ESP8266 scan data uploader / ESP8266 扫码数据上报程序
 * Comments follow a concise bilingual embedded-project style.
 * 注释采用简洁的中英双语嵌入式工程风格。
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#ifdef ENABLE_OLED
#include <Wire.h>
#include <U8g2lib.h>

U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0,/*scl*/12,/*sda*/14, /* reset=*/ U8X8_PIN_NONE);
#endif

// Wi-Fi network name used for connection.
// 用于连接路由器的 Wi-Fi 名称。
String WIFI_SSID = "";
// Password corresponding to WIFI_SSID.
// 与 Wi-Fi 名称对应的密码。
String WIFI_PASSWORD = "";

#define ALARM_PIN 13
#define GREEN_LED 4
#define BLUE_LED 2
#define SCAN_TRIGGER 12
#define SCAN_INTERRUPT 5
#define BUTTON_PIN 14

#define EEPROM_SIZE 512
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_MAGIC_VAL 0xAA
#define EEPROM_EXTIMODE_ADDR 1
#define EEPROM_SSID_ADDR 2
#define EEPROM_PASS_ADDR 34
#define EEPROM_MAX_SSID 32
#define EEPROM_MAX_PASS 64

// Returned data below this threshold is treated as an abnormal result.
// API 返回值低于此阈值时按异常结果处理。
const int OUTTIME = 1;
// Maximum wait time for one Wi-Fi connection attempt, in milliseconds.
// 单次 Wi-Fi 连接的最长等待时间，单位为毫秒。
const unsigned long WIFI_ATTEMPT_MS = 10000;
// Maximum number of connection attempts.
// Wi-Fi 最大连接尝试次数。
const int WIFI_MAX_ATTEMPTS = 3;

// Complete API URL generated after Wi-Fi obtains an IP address.
// Wi-Fi 获取 IP 后动态生成的完整 API 地址。
String API_URL = "";
// Fixed server endpoint path.
// 服务端固定接口路径。
const char* API_PATH = "/api/externalinterface/addMaterialBoxScanningRecord";

// Last octet of the API server address.
// API 服务器 IP 地址的最后一段。
const int API_LAST_OCTET = 96;

void buzzerBeep(unsigned long durationMs);

void buzzerBeepAlway(unsigned long durationMs);
void buzzerFail();

/**
 * @brief  Build the API URL from the local IP address.
 * @brief  根据本机 IP 地址拼接 API 请求地址。
 * @retval true  URL generated successfully.
 * @retval true  地址生成成功。
 */
bool buildAPIURL() {
  // Local IP assigned by the access point.
  // 路由器分配给设备的本机 IP 地址。
  if (!localIP.isSet() || localIP[0] == 0) {
    API_URL = "";
    Serial.print("API URL NOT built, local IP invalid: ");
    Serial.println(localIP.toString());
    return false;
  }
  // Select the server port according to the third IP octet.
  // 根据本机 IP 第三段选择服务端端口。
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

// Bytes collected from the scanner until the frame idle timeout expires.
// 扫描模块输入缓冲区，空闲超时后视为一帧数据。
String inputBuffer = "";
// Most recently uploaded scan data, used by the OLED display.
// 最近一次上报的数据，供 OLED 显示。
String lastSentData = "";
// Timestamp of the most recently received serial byte.
// 最近一次收到串口字节的时间戳。
unsigned long lastReceiveTime = 0;
// Serial idle interval that terminates one scan frame.
// 串口连续空闲超过此时间即认为一帧结束。
const unsigned long TIMEOUT_MS = 500;
// Cached link state used by the application logic.
// 程序缓存的 Wi-Fi 连接状态。
bool wifiConnected = false;
// Indicates whether the latest frame was uploaded successfully.
// 标记最近一帧数据是否上报成功。
bool apiSent = false;

// Set by the GPIO5 ISR and consumed in loop().
// 由 GPIO5 中断置位，在 loop() 中清除和处理。
volatile bool scanTriggered = false;
// Optional interrupt counter reserved for diagnostics.
// 预留的中断计数器，用于调试统计。
volatile unsigned long interruptCount = 0;
// Timestamp reserved for periodic debug output.
// 预留的调试输出时间戳。
unsigned long lastDebugPrint = 0;
// Current GPIO5 interrupt edge, falling edge by default.
// 当前 GPIO5 中断边沿，默认使用下降沿。
int extiMode = FALLING;

// Non-zero while the scanner is expected to send a frame.
// 扫描模块工作期间记录开始时间，0 表示未处于扫描状态。
unsigned long scanActiveSince = 0;
// Do not accept another trigger before this timestamp.
// 在该时间戳之前忽略新的扫描触发。
unsigned long scanCooldownUntil = 0;
// Number of consecutive scan timeouts without received data.
// 连续未收到数据的扫描超时次数。
int emptyScanStreak = 0;
// Minimum interval between two scan operations.
// 两次扫描之间的最小间隔。
const unsigned long SCAN_COOLDOWN_MS = 500;
// Backoff interval after repeated empty scans.
// 连续空扫描后的退避等待时间。
const unsigned long SCAN_BACKOFF_MS = 10000;
// Start backoff after this many consecutive empty scans.
// 连续空扫描达到此次数后进入退避。
const int EMPTY_SCAN_BACKOFF_AT = 3;
// Maximum time to wait for scanner serial data.
// 等待扫描模块串口数据的最长时间。
const unsigned long SCAN_TIMEOUT_MS = 5000;
// Maximum length accepted for one scan frame.
// 单帧扫码数据允许的最大长度。
const unsigned int MAX_FRAME_LEN = 128;
// Maximum serial bytes processed in one loop iteration.
// 每次 loop 最多处理的串口字节数。
const int SERIAL_BUDGET_PER_LOOP = 96;
// Maximum wait time for the first configuration byte.
// 等待配置首字节的最长时间。
const unsigned long CONFIG_WAIT_MS = 15000;
// Idle interval used to terminate the configuration frame.
// 配置帧连续空闲超过此时间即结束接收。
const unsigned long CONFIG_FRAME_IDLE_MS = 500;
// Maximum length of a serial configuration command.
// 串口配置命令允许的最大长度。
const unsigned int MAX_CONFIG_LEN = 160;
// Duration of the low phase on SCAN_TRIGGER during configuration.
// 配置期间 SCAN_TRIGGER 保持低电平的时间。
const unsigned long CONFIG_TRIG_LOW_MS = 1500;
// Duration of the high phase used to reset the scanner input.
// 配置期间用于复位扫描模块的高电平间隔。
const unsigned long CONFIG_TRIG_HIGH_MS = 300;
// Timestamp of the last Wi-Fi status check.
// 最近一次 Wi-Fi 状态检查的时间戳。
unsigned long lastWifiCheck = 0;
// Period of background Wi-Fi status checks.
// 后台检查 Wi-Fi 状态的周期。
const unsigned long WIFI_CHECK_MS = 5000;

/**
 * @brief  Record a scanner interrupt event.
 * @brief  记录扫描模块中断事件。
 * @note   Keep the ISR short; processing is performed in loop().
 * @note   中断函数保持简短，具体处理放在 loop() 中。
 */
void IRAM_ATTR onScanInterrupt() {
  scanTriggered = true;

}

/**
 * @brief  Convert a Wi-Fi status code to readable text.
 * @brief  将 Wi-Fi 状态码转换为可读文本。
 * @param  status Wi-Fi status code.
 * @param  status Wi-Fi 状态码。
 * @retval const char* Status description.
 * @retval const char* 状态描述字符串。
 */
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

/**
 * @brief  Initialize Wi-Fi and retry the connection.
 * @brief  初始化 Wi-Fi，并在失败时进行重试。
 */
void initWiFi() {
  // Select station mode before connecting to the access point.
  // 连接路由器前先设置为 STA（客户端）模式。
  WiFi.mode(WIFI_STA);
  // Let the SDK reconnect in the background when possible.
  // 启用 SDK 的后台自动重连能力。
  WiFi.setAutoReconnect(true);
  // Retry counter for Wi-Fi connection attempts.
  // Wi-Fi 连接尝试计数器。
  for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
    // Clear the previous connection state before retrying.
    // 每次重试前先清除上一次的连接状态。
    WiFi.disconnect();
    delay(100);
    // Start a connection attempt with the configured credentials.
    // 使用当前配置的账号密码发起连接。
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      // Start time of the current connection attempt.
    // 本次连接尝试的开始时间。
    // Poll the link state until connected or the attempt times out.
    // 持续检查连接状态，直到成功或本次尝试超时。
    while (WiFi.status() != WL_CONNECTED
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      delay(500);
    }

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

/**
 * @brief  Remove non-printable characters from input data.
 * @brief  清理输入数据中的不可打印字符。
 * @param  input Raw input string.
 * @param  input 原始输入字符串。
 * @retval String Cleaned string.
 * @retval String 清理后的字符串。
 */
String cleanUTF8(String input) {
  // Output string built from printable input characters.
  // 由可打印字符拼接出的输出字符串。
  String result = "";
  for (int i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c >= 32 && c <= 126) {
      result += c;
    }
  }
  return result;
}

/**
 * @brief  Upload one scan frame to the HTTP API.
 * @brief  将一帧扫码数据上传到 HTTP API。
 * @param  data Scan frame content.
 * @param  data 扫码帧内容。
 * @retval true Request completed; false request failed.
 * @retval true 请求完成；false 请求失败。
 */
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
    
    // Sanitized scan content used in the JSON payload.
    // 清理后的扫码内容，用于构造 JSON 请求体。
    String cleanData = cleanUTF8(data);
    // JSON body sent to the server.
    // 要发送给服务器的 JSON 请求体。
    String jsonPayload = "{\"qrcode\":\"" + cleanData + "\"}";
    Serial.print("Sending (attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_RETRIES);
    Serial.print("): ");
    Serial.println(jsonPayload);
    
    // Submit the JSON payload and save the HTTP result code.
    // 发送 JSON 请求并保存 HTTP 返回码。
    // HTTP status code returned by POST().
    // POST() 返回的 HTTP 状态码。
    int httpCode = http.POST(jsonPayload);
    
    Serial.print("HTTP Code: ");
    Serial.println(httpCode);
    
    if (httpCode > 0) {
      // Raw response body returned by the HTTP server.
      // HTTP 服务器返回的原始响应正文。
      String response = http.getString();
      Serial.print("API Response: ");
      Serial.println(response);
      http.end();
      
      // Parse the response body to obtain the server result.
      // 解析响应体，以获取服务器返回的数据。
          // Temporary JSON document used to parse the server response.
      // 用于解析服务器响应的临时 JSON 文档。
      DynamicJsonDocument doc(1024);
          // Temporary JSON document used to parse the server response.
      // 用于解析服务器响应的临时 JSON 文档。
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);
      if (!error && doc.containsKey("data")) {
        float apiData = doc["data"].as<float>();
        Serial.print("API Data: ");
        Serial.println(apiData);
        if ((apiData < OUTTIME)&&(apiData>=0)) {
          Serial.println("Invalid data, buzzer 3s...");
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

/**
 * @brief  Generate the upload-failure alarm.
 * @brief  生成上报失败提示音。
 */
void buzzerFail() {
  pinMode(ALARM_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(ALARM_PIN, HIGH);
    delay(100);
    digitalWrite(ALARM_PIN, LOW);
    delay(100);
  }
}

/**
 * @brief  Generate a periodic buzzer tone.
 * @brief  按固定周期驱动蜂鸣器。
 * @param  durationMs Tone duration in milliseconds.
 * @param  durationMs 蜂鸣持续时间，单位为毫秒。
 */
void buzzerBeep(unsigned long durationMs) {
  pinMode(ALARM_PIN, OUTPUT);
    // Start time of the current connection attempt.
    // 本次连接尝试的开始时间。
  while (millis() - start < durationMs) {
    digitalWrite(ALARM_PIN, HIGH);
    delay(500);
    digitalWrite(ALARM_PIN, LOW);
    delay(500);
  }
}

/**
 * @brief  Keep the buzzer active for the specified duration.
 * @brief  让蜂鸣器持续鸣叫指定时间。
 * @param  durationMs Tone duration in milliseconds.
 * @param  durationMs 蜂鸣持续时间，单位为毫秒。
 */
void buzzerBeepAlway(unsigned long durationMs) {
  pinMode(ALARM_PIN, OUTPUT);
    // Start time of the current connection attempt.
    // 本次连接尝试的开始时间。
  digitalWrite(ALARM_PIN, HIGH);
  while (millis() - start < durationMs) {
    delay(1000);

  }
  digitalWrite(ALARM_PIN, LOW);
}

/**
 * @brief  Save network and interrupt configuration to EEPROM.
 * @brief  将网络和中断配置保存到 EEPROM。
 * @param  ssid Wi-Fi name.
 * @param  ssid Wi-Fi 名称。
 * @param  pass Wi-Fi password.
 * @param  pass Wi-Fi 密码。
 * @param  mode GPIO5 interrupt edge.
 * @param  mode GPIO5 中断边沿。
 */
void saveConfigToEEPROM(String ssid, String pass, int mode) {
  // Open the EEPROM emulation session.
  // 开启 EEPROM 仿真读写会话。
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.write(EEPROM_EXTIMODE_ADDR, mode == RISING ? 1 : 0);

  for (int i = 0; i < EEPROM_MAX_SSID; i++) {
    EEPROM.write(EEPROM_SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  }
  for (int i = 0; i < EEPROM_MAX_PASS; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  // Commit the modified buffer to flash memory.
  // 将修改后的缓存提交到 Flash。
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Config saved to EEPROM");
}

/**
 * @brief  Load configuration from EEPROM and validate it.
 * @brief  从 EEPROM 读取并校验配置。
 * @retval true Valid configuration found.
 * @retval true 找到有效配置。
 * @retval false No usable configuration found.
 * @retval false 没有找到可用配置。
 */
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

/**
 * @brief  Receive and apply configuration from the serial port.
 * @brief  从串口接收并应用网络配置。
 * @note   Format: SSID+PASSWORD+GPIO5EXTIMOD.
 * @note   格式：SSID+PASSWORD+GPIO5EXTIMOD。
 */
void connectWiFiFromSerial() {
  while (Serial.available()) Serial.read();
  inputBuffer = "";
  Serial.println("\nEnter WiFi config (SSID+PASSWORD+GPIO5EXTIMOD): ");
  Serial.println("GPIO5EXTIMOD: 1=rising edge, 0=falling edge");

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

  digitalWrite(SCAN_TRIGGER, HIGH);

  // Complete configuration frame collected from UART.
  // 从串口收集到的完整配置帧。
  String config = "";
  unsigned long lastByte = millis();
  while (millis() - lastByte < CONFIG_FRAME_IDLE_MS) {
    if (Serial.available()) {
      // One byte read from the scanner UART stream.
    // 从扫描模块串口流中读取的一个字节。
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
  while (Serial.available()) Serial.read();

  config.trim();
  Serial.print("Received config: '");
  Serial.print(config);
  Serial.println("'");

  // Position of the first field separator.
  // 第一个字段分隔符“+”的位置。
  int firstPlus = config.indexOf('+');
  // Position of the second field separator.
  // 第二个字段分隔符“+”的位置。
  int secondPlus = config.indexOf('+', firstPlus + 1);
  if (firstPlus > 0 && secondPlus > firstPlus + 1) {
    WIFI_SSID = config.substring(0, firstPlus);
    WIFI_PASSWORD = config.substring(firstPlus + 1, secondPlus);
    // Text representation of the requested interrupt edge mode.
    // 串口配置中指定的中断边沿模式文本。
    String extiMod = config.substring(secondPlus + 1);
    extiMod.trim();

    Serial.print("SSID: "); Serial.println(WIFI_SSID);
    Serial.print("Password: "); Serial.println(WIFI_PASSWORD);
    Serial.print("GPIO5EXTIMOD: "); Serial.println(extiMod);

    // Disable the interrupt while the scanner is active.
    // 扫描工作期间暂时关闭中断，避免重复触发。
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

/**
 * @brief  Receive scanner bytes and submit a completed frame.
 * @brief  接收扫描数据，并在一帧结束后提交。
 */
void handleSerial() {
  // Limit the amount of serial work done in one loop pass.
  // 限制单次 loop 处理的串口工作量。
  // Per-loop byte budget preventing serial processing from blocking other tasks.
  // 单次 loop 的字节预算，防止串口处理阻塞其他任务。
  int budget = 0;
  while (Serial.available() > 0 && budget++ < SERIAL_BUDGET_PER_LOOP) {
    // One byte read from the scanner UART stream.
    // 从扫描模块串口流中读取的一个字节。
    char c = Serial.read();
    if (millis() - lastReceiveTime > TIMEOUT_MS) {
      inputBuffer = "";
    }
    if (inputBuffer.length() < MAX_FRAME_LEN) {
      inputBuffer += c;
    }
    lastReceiveTime = millis();

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
    // Idle timeout marks a complete frame; upload it now.
    // 空闲超时表示一帧数据接收完成，此处开始上报。
    // Upload result for the completed scan frame.
    // 当前完整扫码帧的上报结果。
    bool success = sendToAPI(inputBuffer);
    if (success) {
      Serial.println("API Request Sent");
      lastSentData = inputBuffer;
      apiSent = true;
    } else {
      Serial.println("API Request Failed");
      buzzerFail();
    }
    inputBuffer = "";
    digitalWrite(SCAN_TRIGGER, HIGH);
    // Re-enable the configured edge interrupt for the next scan.
    // 按当前边沿模式重新启用中断，等待下一次扫描。
    attachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT), onScanInterrupt, extiMode);
    scanTriggered = false;
    scanCooldownUntil = millis() + SCAN_COOLDOWN_MS;
  }
}

#ifdef ENABLE_OLED

/**
 * @brief  Refresh the optional OLED status screen.
 * @brief  刷新可选 OLED 状态界面。
 */
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

/**
 * @brief  Initialize the board and load saved configuration.
 * @brief  初始化开发板并加载已保存配置。
 */
void setup() {
  Serial.begin(9600);
  delay(3000);
  

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
  

  // EEPROM-loaded Wi-Fi credentials.
  // 从 EEPROM 读取的 Wi-Fi 名称和密码。
  String savedSSID, savedPass;
  // EEPROM-loaded GPIO5 interrupt edge mode.
  // 从 EEPROM 读取的 GPIO5 中断边沿模式。
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
  

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button pressed, entering WiFi config mode...");
      connectWiFiFromSerial();
    }
  }
}

/**
 * @brief  Execute periodic application tasks.
 * @brief  执行周期性的应用任务。
 */
void loop() {
  // Process incoming scanner data first.
  // 优先处理扫描模块输入数据。
  handleSerial();
  // Update the optional display.
  // 更新可选的 OLED 显示。
  handleOLED();
  
  if (scanTriggered) {
    scanTriggered = false;

    if (millis() >= scanCooldownUntil) {
      Serial.println("in");
      detachInterrupt(digitalPinToInterrupt(SCAN_INTERRUPT));
      // Pull the trigger low to request scanner output.
      // 拉低触发线，请求扫描模块输出数据。
      digitalWrite(SCAN_TRIGGER, LOW);
      scanActiveSince = millis();
    }
  }

  if (scanActiveSince != 0 && millis() - scanActiveSince > SCAN_TIMEOUT_MS) {
    scanActiveSince = 0;
    emptyScanStreak++;
    digitalWrite(SCAN_TRIGGER, HIGH);
    if (emptyScanStreak >= EMPTY_SCAN_BACKOFF_AT) {

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
    scanTriggered = false;
  }
  

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      connectWiFiFromSerial();
    }
  }
  

  if (millis() - lastWifiCheck > WIFI_CHECK_MS) {
    lastWifiCheck = millis();
    bool up = (WiFi.status() == WL_CONNECTED && WiFi.localIP().isSet());
    if (up != wifiConnected) {
      wifiConnected = up;
      digitalWrite(GREEN_LED, up ? LOW : HIGH);
      digitalWrite(BLUE_LED, up ? HIGH : LOW);
      if (up) {
        Serial.println("WiFi reconnected");
        buildAPIURL();
      } else {
        API_URL = "";
        Serial.println("WiFi lost");
      }
    }
  }

  delay(100);
}
