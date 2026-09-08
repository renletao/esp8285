// #include <ESP8266WiFi.h>

// // OLED Enable/Disable (comment out to disable OLED)
// // #define ENABLE_OLED

// #ifdef ENABLE_OLED
// #include <Wire.h>
// #include <U8g2lib.h>
// // OLED Configuration: SCL=12, SDA=14 for ESP8266
// U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /*scl*/12, /*sda*/14, /* reset=*/ U8X8_PIN_NONE);
// #endif

// // Scan interval (milliseconds)
// const unsigned long SCAN_INTERVAL = 5000;  // Scan every 15 seconds
// unsigned long lastScanTime = 0;
// int totalNetworks = 0;  // Total number of networks found in last scan

// // Structure to hold network information
// struct NetworkInfo {
//   String ssid;
//   int32_t rssi;
// };

// // Top 4 strongest networks
// NetworkInfo topNetworks[4];

// // Comparison function for sorting by RSSI descending (strongest first)
// int compareByRSSI(const void *a, const void *b) {
//   NetworkInfo *na = (NetworkInfo *)a;
//   NetworkInfo *nb = (NetworkInfo *)b;
//   return nb->rssi - na->rssi;
// }

// // Perform WiFi scan and populate top 4 networks
// void performScan() {
//   Serial.println("Scanning WiFi networks...");

//   // Disconnect from any AP and set to station mode for scanning
//   WiFi.mode(WIFI_STA);
//   WiFi.disconnect();
//   delay(100);

//   int n = WiFi.scanNetworks();
//   Serial.print("Found ");
//   Serial.print(n);
//   Serial.println(" networks");

//   n = WiFi.scanComplete();
//   Serial.println(n);

//   totalNetworks = n;

//   if (n == 0) {
//     for (int i = 0; i < 4; i++) {
//       topNetworks[i].ssid = "---";
//       topNetworks[i].rssi = 0;
//     }
//     return;
//   }

//   // Collect all networks into array
//   NetworkInfo *all = new NetworkInfo[n];
//   for (int i = 0; i < n; i++) {
//     all[i].ssid = WiFi.SSID(i);
//     all[i].rssi = WiFi.RSSI(i);
//     if (all[i].ssid.length() == 0) {
//       all[i].ssid = "(hidden)";
//     }
//   }

//   // Sort by signal strength (strongest first)
//   qsort(all, n, sizeof(NetworkInfo), compareByRSSI);

//   // Copy top 4 (or fill remaining with "---")
//   int count = (n < 4) ? n : 4;
//   for (int i = 0; i < count; i++) {
//     topNetworks[i] = all[i];
//   }
//   for (int i = count; i < 4; i++) {
//     topNetworks[i].ssid = "---";
//     topNetworks[i].rssi = 0;
//   }

//   delete[] all;
// }

// // Print top 4 results to Serial
// void printResults() {
//   Serial.println("--- Top 4 Networks ---");
//   for (int i = 0; i < 4; i++) {
//     Serial.print("  ");
//     Serial.print(i + 1);
//     Serial.print(". ");
//     Serial.print(topNetworks[i].ssid);
//     Serial.print("  (");
//     Serial.print(topNetworks[i].rssi);
//     Serial.println(" dBm)");
//   }
//   Serial.println("----------------------");
// }

// #ifdef ENABLE_OLED
// // Display top 4 networks on OLED (128x64)
// void updateOLED() {
//   u8g2.clearBuffer();
//   u8g2.setFont(u8g2_font_ncenB08_tr);

//   // Title line
//   u8g2.setCursor(0, 8);
//   u8g2.print("WiFi Scanner");

//   // Total count on same line, right-aligned
//   u8g2.setCursor(82, 8);
//   u8g2.print("Total:");
//   u8g2.print(totalNetworks);

//   // Separator line
//   u8g2.drawHLine(0, 11, 128);

//   // Display top 4 networks
//   for (int i = 0; i < 4; i++) {
//     int y = 24 + i * 12;

//     // Index number
//     u8g2.setCursor(0, y);
//     u8g2.print(i + 1);
//     u8g2.print(".");

//     // SSID - truncate to fit (about 14 chars max for 82px width)
//     String displaySSID = topNetworks[i].ssid;
//     if (displaySSID.length() > 14) {
//       displaySSID = displaySSID.substring(0, 12) + "..";
//     }
//     u8g2.setCursor(14, y);
//     u8g2.print(displaySSID);

//     // RSSI value (right-aligned)
//     u8g2.setCursor(100, y);
//     u8g2.print(topNetworks[i].rssi);
//     u8g2.print("dB");
//   }

//   u8g2.sendBuffer();
// }
// #else
// void updateOLED() {}
// #endif

// void setup() {
//   Serial.begin(19200);
//   Serial.println();
//   Serial.println("====================================");
//   Serial.println("WiFi Signal Strength Scanner Started");
//   Serial.println("====================================");

// #ifdef ENABLE_OLED
//   u8g2.begin();
//   u8g2.clearBuffer();
//   u8g2.setFont(u8g2_font_ncenB08_tr);
//   u8g2.setCursor(0, 20);
//   u8g2.print("WiFi Scanner");
//   u8g2.setCursor(0, 36);
//   u8g2.print("Starting...");
//   u8g2.sendBuffer();
// #endif

//   // Set WiFi to station mode (required for scanning)
//   WiFi.mode(WIFI_STA);
//   WiFi.disconnect();
//   delay(100);

//   // Perform initial scan after a short delay
//   delay(2000);
//   performScan();
//   printResults();
//   lastScanTime = millis();

//   Serial.println("Ready! (scanning every 15s)");
// }

// void loop() {
//   // Periodically rescan
//   if (millis() - lastScanTime >= SCAN_INTERVAL) {
//     performScan();
//     printResults();
//     lastScanTime = millis();
//   }

//   updateOLED();
//   delay(100);
// }
