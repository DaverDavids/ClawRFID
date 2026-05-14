// =============================================================================
//  clawrfid.ino  —  ESP32-C3 | Dual MFRC522 (SPI) | WiFi UI
//  Libs: MFRC522v2 (OSSLibraries), ArduinoOTA, ESPmDNS, Preferences
//
//  Library note: uses MFRC522v2 by OSSLibraries (not the legacy miguelbalboa
//  fork). The legacy fork uses PrintCanvas which was removed in ESP32 Arduino
//  Core 3.x. MFRC522v2 is a drop-in replacement with an identical public API.
//  Arduino IDE : search "MFRC522v2" in Library Manager, install by OSSLibraries.
//  PlatformIO  : lib_deps = OSSLibraries/Arduino_MFRC522v2
// =============================================================================

#define DEBUG 1
#if DEBUG
  #define DPRINT(x)   Serial.print(x)
  #define DPRINTLN(x) Serial.println(x)
#else
  #define DPRINT(x)
  #define DPRINTLN(x)
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPI.h>
#include <MFRC522v2.h>        // OSSLibraries — works with ESP32 Core 3.x
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <Secrets.h>
#include "html.h"

// ── Configuration ──────────────────────────────────────────────────────────
#define HOSTNAME      "clawrfid"
#define AP_SSID        HOSTNAME
#define WIFI_TIMEOUT   5000    // ms per attempt

// SPI clock — RC522 supports up to 10 MHz.
#define RFID_SPI_HZ    10000000UL

// ── Pins ───────────────────────────────────────────────────────────────────
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10

#define RFID1_SS   7
#define RFID1_RST  1

#define RFID2_SS   2
#define RFID2_RST  3

// ── MFRC522v2 driver objects ────────────────────────────────────────────────
// MFRC522v2 separates the pin/SPI driver from the protocol object.
MFRC522DriverPinSimple ss1Pin(RFID1_SS);
MFRC522DriverPinSimple ss2Pin(RFID2_SS);
MFRC522DriverSPI       driver1{ss1Pin};
MFRC522DriverSPI       driver2{ss2Pin};
MFRC522                rfid1{driver1};
MFRC522                rfid2{driver2};

// ── Error-rate tracking (rolling 16-poll window, bitmask) ──────────────────
static uint16_t errMask1 = 0;
static uint16_t errMask2 = 0;

static inline uint8_t popcount16(uint16_t v) {
  v = v - ((v >> 1) & 0x5555u);
  v = (v & 0x3333u) + ((v >> 2) & 0x3333u);
  return (uint8_t)(((v + (v >> 4)) & 0x0F0Fu) * 0x0101u >> 8);
}

// ── Globals ────────────────────────────────────────────────────────────────
WebServer   server(80);
DNSServer   dns;
Preferences prefs;

bool     apMode    = false;
String   lastUID1  = "None";
String   lastUID2  = "None";
uint32_t lastRead1 = 0;   // millis() of last successful read, 0 = never
uint32_t lastRead2 = 0;

// ── JSON helper ────────────────────────────────────────────────────────────
void sendJSON(int code, const String &json) {
  server.sendHeader("Connection",                  "keep-alive");
  server.sendHeader("Cache-Control",               "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", json);
}

// ── WiFi ───────────────────────────────────────────────────────────────────
bool connectWifi(const String &ssid, const String &psk) {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setHostname(HOSTNAME);

  WiFi.begin(ssid.c_str(), psk.c_str());
  delay(500);
  WiFi.disconnect(true);
  delay(200);

  for (int attempt = 0; attempt < 3; attempt++) {
    DPRINT("Connecting to "); DPRINT(ssid);
    DPRINT(" attempt "); DPRINTLN(attempt + 1);
    WiFi.begin(ssid.c_str(), psk.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
      delay(100); DPRINT(".");
    }
    DPRINTLN("");
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect(true);
    delay(500);
  }
  DPRINTLN("WiFi failed");
  return false;
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", WiFi.softAPIP());
  DPRINT("AP started: "); DPRINTLN(AP_SSID);
  DPRINT("AP IP: ");      DPRINTLN(WiFi.softAPIP());
}

// ── OTA ────────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]()  { DPRINTLN("OTA start"); });
  ArduinoOTA.onEnd([]()    { DPRINTLN("OTA done");  });
  ArduinoOTA.onError([](ota_error_t e) { DPRINT("OTA error: "); DPRINTLN(e); });
  ArduinoOTA.begin();
}

// ── Web routes ─────────────────────────────────────────────────────────────
void handleRoot() {
  server.send_P(200, "text/html", apMode ? PORTAL_HTML : INDEX_HTML);
}

void handleData() {
  // Antenna gain: read RFCfgReg via MFRC522v2 driver (HTTP path only)
  uint8_t gain1 = (driver1.readRegister(MFRC522::PCD_Register::RFCfgReg) >> 4) & 0x07;
  uint8_t gain2 = (driver2.readRegister(MFRC522::PCD_Register::RFCfgReg) >> 4) & 0x07;

  uint8_t hits1 = popcount16(errMask1);
  uint8_t hits2 = popcount16(errMask2);

  uint32_t age1 = lastRead1 ? (millis() - lastRead1) : 0xFFFFFFFFu;
  uint32_t age2 = lastRead2 ? (millis() - lastRead2) : 0xFFFFFFFFu;

  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"uid1\":\"%s\",\"uid2\":\"%s\""
    ",\"gain1\":%u,\"gain2\":%u"
    ",\"hits1\":%u,\"hits2\":%u"
    ",\"age1\":%lu,\"age2\":%lu}",
    lastUID1.c_str(), lastUID2.c_str(),
    gain1, gain2,
    hits1, hits2,
    (unsigned long)age1, (unsigned long)age2
  );
  sendJSON(200, buf);
}

void handleSaveWifi() {
  if (!server.hasArg("ssid")) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("psk",  server.arg("psk"));
  prefs.end();
  server.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#111;color:#ddd;"
    "text-align:center;padding:2rem'>"
    "<h2 style='color:#4caf50'>Saved!</h2><p>Rebooting to connect...</p>"
    "</body></html>");
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302);
  } else {
    server.send(404, "text/plain", "not found");
  }
}

void setupServer() {
  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/data",     HTTP_GET,  handleData);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.onNotFound(handleNotFound);
  server.begin();
  DPRINTLN("HTTP server started");
}

// ── RFID helpers ───────────────────────────────────────────────────────────
//
// Strategy for maximum poll rate / zero missed tags:
//  1. REQA first (IDLE cards), then WUPA (HALT-ed cards) — covers both states.
//  2. Always rfidIdle() after every attempt — keeps the reader ready for the
//     next poll instead of staying stuck in ACTIVE state.
//  3. 10 MHz SPI clock — shortest possible frame time.
//  4. No delay() in the poll path.
//  5. errMask shifted every poll: two ops, no branches, no overhead.

static inline void rfidIdle(MFRC522 &rfid) {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

String pollReader(MFRC522 &rfid, const char *label,
                  uint16_t &errMask, uint32_t &lastReadTs) {
  byte atqaBuf[2];
  byte atqaLen = sizeof(atqaBuf);

  bool detected =
    (rfid.PICC_RequestA(atqaBuf, &atqaLen) == MFRC522::StatusCode::STATUS_OK) ||
    (rfid.PICC_WakeupA (atqaBuf, &atqaLen) == MFRC522::StatusCode::STATUS_OK);

  errMask = (errMask << 1) | (detected ? 1u : 0u);

  if (!detected) return "";

  if (!rfid.PICC_ReadCardSerial()) {
    rfidIdle(rfid);
    return "";
  }

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i) uid += ':';
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  lastReadTs = millis();
  rfidIdle(rfid);

  DPRINT(label); DPRINT(" UID: "); DPRINTLN(uid);
  return uid;
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(200);
#endif
  DPRINTLN("\n\n=== " HOSTNAME " ===");

  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", MYSSID);
  String psk  = prefs.getString("psk",  MYPSK);
  prefs.end();

  if (!connectWifi(ssid, psk)) {
    DPRINTLN("WiFi failed — starting captive portal AP");
    startAP();
  } else {
    DPRINT("Connected! IP: "); DPRINTLN(WiFi.localIP());
    apMode = false;
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      DPRINTLN("mDNS: http://" HOSTNAME ".local");
    }
    setupOTA();
  }

  setupServer();

  // MFRC522v2 uses SPIClass directly; pass the bus + explicit clock
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI);
  SPI.setFrequency(RFID_SPI_HZ);

  rfid1.PCD_Init();
  rfid1.PCD_SetAntennaGain(MFRC522::RxGain::RxGain_max);
  rfidIdle(rfid1);
  DPRINTLN("RFID1 ready");

  rfid2.PCD_Init();
  rfid2.PCD_SetAntennaGain(MFRC522::RxGain::RxGain_max);
  rfidIdle(rfid2);
  DPRINTLN("RFID2 ready");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  if (!apMode) {
    ArduinoOTA.handle();

    static uint32_t lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        DPRINTLN("WiFi lost, reconnecting...");
        prefs.begin("wifi", true);
        String ssid = prefs.getString("ssid", MYSSID);
        String psk  = prefs.getString("psk",  MYPSK);
        prefs.end();
        if (!connectWifi(ssid, psk)) {
          DPRINTLN("Reconnect failed — fallback to AP");
          startAP();
          server.begin();
        }
      }
    }
  } else {
    dns.processNextRequest();
  }

  server.handleClient();

  // ── Reader 1 ────────────────────────────────────────────────────────────
  {
    String uid = pollReader(rfid1, "[RFID1]", errMask1, lastRead1);
    if (uid.length() && uid != lastUID1) {
      lastUID1 = uid;
      // TODO: application logic
    }
  }

  // ── Reader 2 ────────────────────────────────────────────────────────────
  {
    String uid = pollReader(rfid2, "[RFID2]", errMask2, lastRead2);
    if (uid.length() && uid != lastUID2) {
      lastUID2 = uid;
      // TODO: application logic
    }
  }
}
