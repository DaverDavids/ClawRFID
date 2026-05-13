// =============================================================================
//  clawrfid.ino  —  ESP32-C3 | Dual MFRC522 (SPI) | WiFi UI
//  Libs: MFRC522, ArduinoOTA, ESPmDNS, Preferences
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
#include <MFRC522.h>
#include <Secrets.h>
#include "html.h"

// ── Configuration ──────────────────────────────────────────────────────────
#define HOSTNAME      "clawrfid"
#define AP_SSID        HOSTNAME
#define WIFI_TIMEOUT   5000    // ms per attempt

// SPI clock — RC522 supports up to 10 MHz; be explicit so we never get a
// slower platform default.
#define RFID_SPI_HZ    10000000UL

// ── Pins ───────────────────────────────────────────────────────────────────
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10

#define RFID1_SS   7
#define RFID1_RST  1

#define RFID2_SS   2
#define RFID2_RST  3

// ── Error-rate tracking (rolling 16-poll window, bitmask, zero division) ───
// Each bit = 1 success, 0 miss. Shift left every poll, OR in result.
// No arrays, no division in the poll path — just two register ops.
static uint16_t errMask1 = 0;
static uint16_t errMask2 = 0;

// Popcount for uint16 — compiler will inline/optimize to single instruction
// on most targets; we only call this from handleData() (HTTP path), never
// from the tight poll loop.
static inline uint8_t popcount16(uint16_t v) {
  v = v - ((v >> 1) & 0x5555u);
  v = (v & 0x3333u) + ((v >> 2) & 0x3333u);
  return (uint8_t)(((v + (v >> 4)) & 0x0F0Fu) * 0x0101u >> 8);
}

// ── Globals ────────────────────────────────────────────────────────────────
WebServer   server(80);
DNSServer   dns;
Preferences prefs;
MFRC522     rfid1(RFID1_SS, RFID1_RST);
MFRC522     rfid2(RFID2_SS, RFID2_RST);

bool     apMode      = false;
String   lastUID1    = "None";
String   lastUID2    = "None";
uint32_t lastRead1   = 0;   // millis() of last successful read, 0 = never
uint32_t lastRead2   = 0;

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

  // RF calibration kick
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
  // Antenna gain register value: 0x00–0x70 (3 bits × 16 = 7 steps).
  // Read once here (HTTP path), never in the poll loop.
  uint8_t gain1 = (rfid1.PCD_ReadRegister(MFRC522::RFCfgReg) >> 4) & 0x07;
  uint8_t gain2 = (rfid2.PCD_ReadRegister(MFRC522::RFCfgReg) >> 4) & 0x07;

  // Hits out of 16 polls → 0-16
  uint8_t hits1 = popcount16(errMask1);
  uint8_t hits2 = popcount16(errMask2);

  // ms since last read (0 if never seen)
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
//
//  1. Use REQA (RequestA) instead of WakeupA so we detect cards that are
//     already in IDLE state (fresh cards entering the field). WakeupA is
//     needed to wake HALT-ed cards but we always HALT after a successful
//     read, so on the very next poll we must use WakeupA. We alternate:
//     RequestA first; if that fails, try WakeupA. This covers both cases.
//
//  2. After every successful read — and after every failed attempt — we
//     explicitly HALT the PICC and stop crypto. Without this the RC522
//     stays in ACTIVE state talking to the last card, blocking detection
//     of any new card on subsequent polls.
//
//  3. SPI clock is set to 10 MHz (RFID_SPI_HZ) so frame transfers are as
//     short as possible.
//
//  4. No delay() anywhere in the poll path.
//
// Returns the colon-separated uppercase UID string, or "" if no card found.
// errMask is shifted and updated here — single OR, no branches added.

static inline void rfidIdle(MFRC522 &rfid) {
  rfid.PICC_HaltA();      // send HALT command — PICC enters HALT state
  rfid.PCD_StopCrypto1(); // clear MFCrypto1On bit — ready for next poll
}

String pollReader(MFRC522 &rfid, const char *label,
                  uint16_t &errMask, uint32_t &lastReadTs) {
  byte atqaBuf[2];
  byte atqaLen = sizeof(atqaBuf);

  // Try REQA first (detects cards in IDLE), then WUPA (detects HALT-ed cards).
  bool detected =
    (rfid.PICC_RequestA(atqaBuf, &atqaLen) == MFRC522::STATUS_OK) ||
    (rfid.PICC_WakeupA (atqaBuf, &atqaLen) == MFRC522::STATUS_OK);

  // Rolling 16-poll window: shift left, set LSB on hit.
  // Two ops, no branch added to the hot path.
  errMask = (errMask << 1) | (detected ? 1u : 0u);

  if (!detected) return "";

  if (!rfid.PICC_ReadCardSerial()) {
    rfidIdle(rfid);
    return "";
  }

  // Build UID string
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i) uid += ':';
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  lastReadTs = millis(); // timestamp of successful read

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

  // Explicit 10 MHz SPI clock — do not rely on platform default
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID1_SS);
  SPI.setFrequency(RFID_SPI_HZ);

  rfid1.PCD_Init();
  rfid1.PCD_SetAntennaGain(rfid1.RxGain_max);
  rfidIdle(rfid1);
  DPRINTLN("RFID1 ready");

  rfid2.PCD_Init();
  rfid2.PCD_SetAntennaGain(rfid2.RxGain_max);
  rfidIdle(rfid2);
  DPRINTLN("RFID2 ready");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  // ── Network services (non-blocking) ──────────────────────────────────────
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

  // ── Reader 1 ──────────────────────────────────────────────────────────────
  {
    String uid = pollReader(rfid1, "[RFID1]", errMask1, lastRead1);
    if (uid.length() && uid != lastUID1) {
      lastUID1 = uid;
      // TODO: application logic
    }
  }

  // ── Reader 2 ──────────────────────────────────────────────────────────────
  {
    String uid = pollReader(rfid2, "[RFID2]", errMask2, lastRead2);
    if (uid.length() && uid != lastUID2) {
      lastUID2 = uid;
      // TODO: application logic
    }
  }
}
