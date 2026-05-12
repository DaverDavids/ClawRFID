// =============================================================================
//  clawrfid.ino  —  ESP32-C3 | Dual MFRC522 (SPI)
//  Reads two RC522 RFID sensors simultaneously.
//  Lib: MFRC522
// =============================================================================

#include <SPI.h>
#include <MFRC522.h>

// ── Debug ────────────────────────────────────────────────────────────────────
#define DEBUG 1
#if DEBUG
  #define DBG(x)   Serial.print(x)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

// ── Pins ─────────────────────────────────────────────────────────────────────
// Shared SPI bus
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10

// Reader 1
#define RFID1_SS   7
#define RFID1_RST  1

// Reader 2
// Pick two free GPIO for SS and RST on your board.
// Adjust these defines to match your wiring.
#define RFID2_SS   5
#define RFID2_RST  4

// ── MFRC522 instances ────────────────────────────────────────────────────────
MFRC522 rfid1(RFID1_SS, RFID1_RST);
MFRC522 rfid2(RFID2_SS, RFID2_RST);

// ── Last-seen UIDs ───────────────────────────────────────────────────────────
String lastUID1 = "None";
String lastUID2 = "None";

// ── Helper: poll one reader, return its UID string (empty if no new card) ────
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
  DBGLN("\n== clawrfid ==");

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
  // ── Reader 1 ──
  {
    String uid = pollReader(rfid1, "[RFID1]");
    if (uid.length() && uid != lastUID1) {
      lastUID1 = uid;
      // TODO: add application logic here
    }
  }

  // ── Reader 2 ──
  {
    String uid = pollReader(rfid2, "[RFID2]");
    if (uid.length() && uid != lastUID2) {
      lastUID2 = uid;
      // TODO: add application logic here
    }
  }
}
