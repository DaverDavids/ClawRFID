// =============================================================================
//  clawrfid.ino  —  ESP32-C3 | Dual MFRC522 (SPI) | WiFi UI
//  Libs: MFRC522, ArduinoOTA, ESPmDNS, Preferences
// =============================================================================

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

// ── Debug ────────────────────────────────────────────────────────────────────
#define DEBUG 1
#if DEBUG
  #define DBG(x)   Serial.print(x)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

#define HOSTNAME        "clawrfid"
#define WIFI_TIMEOUT_MS  12000UL
#define RECONNECT_MS      5000UL

// ── Pins ─────────────────────────────────────────────────────────────────────
// Shared SPI bus
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10

// Reader 1
#define RFID1_SS   7
#define RFID1_RST  1

// Reader 2  —  adjust to match your wiring
#define RFID2_SS   5
#define RFID2_RST  4

// ── Globals ───────────────────────────────────────────────────────────────────
Preferences prefs;
WebServer   server(80);
DNSServer   dns;
MFRC522     rfid1(RFID1_SS, RFID1_RST);
MFRC522     rfid2(RFID2_SS, RFID2_RST);

bool   apMode  = false;
String lastUID1 = "None";
String lastUID2 = "None";

// ── WiFi ───────────────────────────────────────────────────────────────────
bool connectWiFi(const String &ssid, const String &psk) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(ssid.c_str(), psk.c_str());
  DBG("Connecting to "); DBGLN(ssid);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) {
    delay(250); DBG('.');
  }
  DBGLN();
  if (WiFi.status() == WL_CONNECTED) { DBG("IP: "); DBGLN(WiFi.localIP()); return true; }
  DBGLN("WiFi failed"); return false;
}

void startCaptivePortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(HOSTNAME);
  dns.start(53, "*", WiFi.softAPIP());
  DBG("AP IP: "); DBGLN(WiFi.softAPIP());
}

void startNetServices() {
  MDNS.end();
  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]()  { DBGLN("OTA start"); });
  ArduinoOTA.onError([](ota_error_t e) { DBG("OTA err "); DBGLN(e); });
  ArduinoOTA.begin();
  DBGLN("mDNS+OTA ready \u2192 " HOSTNAME ".local");
}

// ── Web routes ────────────────────────────────────────────────────────────────
void handleRoot() {
  server.send_P(200, "text/html", apMode ? WIFI_HTML : INDEX_HTML);
}

void handleData() {
  String j =
    "{\"uid1\":\"" + lastUID1 + "\"" +
    ",\"uid2\":\"" + lastUID2 + "\"}"
  ;
  server.send(200, "application/json", j);
}

void handleSetWifi() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing ssid"); return; }
  prefs.begin("wifi", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("psk",  server.arg("psk"));
  prefs.end();
  server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><p>Rebooting\u2026</p>");
  delay(1500);
  ESP.restart();
}

void setupServer() {
  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/data",    HTTP_GET,  handleData);
  server.on("/setwifi", HTTP_POST, handleSetWifi);
  server.onNotFound([]() {
    server.sendHeader("Location",
      String("http://") +
      (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "/");
    server.send(302);
  });
  server.begin();
}

// ── RFID helper ────────────────────────────────────────────────────────────────
String pollReader(MFRC522 &rfid, const char *label) {
  byte buf[2];
  byte bsz = sizeof(buf);
  if (rfid.PICC_WakeupA(buf, &bsz) != MFRC522::STATUS_OK) return "";
  if (!rfid.PICC_ReadCardSerial())                          return "";

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i) uid += ':';
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PCD_StopCrypto1();
  DBG(label); DBG(" UID: "); DBGLN(uid);
  return uid;
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(400);
#endif
  DBGLN("\n== " HOSTNAME " ==");

  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", MYSSID);
  String psk  = prefs.getString("psk",  MYPSK);
  prefs.end();

  if (connectWiFi(ssid, psk)) startNetServices();
  else                         startCaptivePortal();

  setupServer();

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID1_SS);

  rfid1.PCD_Init();
  rfid1.PCD_SetAntennaGain(rfid1.RxGain_max);
  DBGLN("RFID1 ready");

  rfid2.PCD_Init();
  rfid2.PCD_SetAntennaGain(rfid2.RxGain_max);
  DBGLN("RFID2 ready");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  static unsigned long lastReconnect = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastReconnect > RECONNECT_MS) {
    lastReconnect = millis();
    DBGLN("WiFi lost \u2014 reconnecting\u2026");
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", MYSSID);
    String psk  = prefs.getString("psk",  MYPSK);
    prefs.end();
    if (connectWiFi(ssid, psk)) startNetServices();
  }

  if (apMode) dns.processNextRequest();
  else        ArduinoOTA.handle();
  server.handleClient();

  // ── Reader 1
  {
    String uid = pollReader(rfid1, "[RFID1]");
    if (uid.length() && uid != lastUID1) {
      lastUID1 = uid;
      // TODO: application logic
    }
  }

  // ── Reader 2
  {
    String uid = pollReader(rfid2, "[RFID2]");
    if (uid.length() && uid != lastUID2) {
      lastUID2 = uid;
      // TODO: application logic
    }
  }
}
