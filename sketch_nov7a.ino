// ============================================================
// TRICORDER v25.1
// v23.1: новая клавиатура 10×4
// v24.0: OLED Arcade
// v25.0: battery saver, IR favorites, passive WiFi survey
// v25.1: local tiny-chat AP and safe menu header layout
//        classic arcade games and Minesweeper
// ============================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <FFat.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_sleep.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include "tiny_chat_intent.h"
#include "tiny_chat_model.h"

#define HAVE_QR 1
#ifdef HAVE_QR
  #include "qrcode.h"
#endif

#define SW 128
#define SH 64
#define OLED_RST -1
#define SDA_PIN 8
#define SCL_PIN 9
#define BTN_L 2
#define BTN_R 3
#define BTN_M 4
#define IR_PIN 0
#define BATT_ADC 1
#define BATT_CAL_DEF 2.02f
#define BATT_MIN 2.70f
#define BATT_MAX 4.35f

Adafruit_SSD1306 display(SW, SH, &Wire, OLED_RST);
IRsend irSend(IR_PIN);
Preferences prefs;
WebServer chatWeb(80);
bool chatServerRunning = false;
bool chatPortalError = false;
String chatApPassword = "";

struct Btn;
struct IrDevice;
struct IrFavorite;
struct WifiAp;
void haltScan();

enum ScanMode { SCAN_OFF, SCAN_ON };
ScanMode scanMode = SCAN_OFF;
uint32_t pingResult = 0, pingAttempts = 0, pingOk = 0;
bool pingDone = false;
float speedKbps = 0;
int speedCode = 0;
bool speedDone = false;
bool deauthRunning = false, cloneRunning = false, spamRunning = false, snifferRunning = false;
uint32_t lastRadioActiveMs = 0;

// ---- Battery cal ----
float battCal = BATT_CAL_DEF;
void loadBattCal() {
  prefs.begin("batcal", true);
  float c = prefs.getFloat("cal", BATT_CAL_DEF);
  prefs.end();
  battCal = (c < 1.0f || c > 5.0f) ? BATT_CAL_DEF : c;
}
void saveBattCal() { prefs.begin("batcal", false); prefs.putFloat("cal", battCal); prefs.end(); }

// ---- Credentials ----
#define MAX_CREDS 10
struct Cred { char ssid[33]; char pass[65]; };
Cred creds[MAX_CREDS];
int credCount = 0;

void loadCreds() {
  prefs.begin("pwdb", true);
  credCount = constrain(prefs.getInt("n", 0), 0, MAX_CREDS);
  for (int i = 0; i < credCount; i++) {
    String ks = "s" + String(i), kp = "p" + String(i);
    String s = prefs.getString(ks.c_str(), ""), p = prefs.getString(kp.c_str(), "");
    strncpy(creds[i].ssid, s.c_str(), 32); creds[i].ssid[32] = 0;
    strncpy(creds[i].pass, p.c_str(), 64); creds[i].pass[64] = 0;
  }
  prefs.end();
}
void saveCredsToNVS() {
  prefs.begin("pwdb", false);
  prefs.putInt("n", credCount);
  for (int i = 0; i < credCount; i++) {
    String ks = "s" + String(i), kp = "p" + String(i);
    prefs.putString(ks.c_str(), creds[i].ssid);
    prefs.putString(kp.c_str(), creds[i].pass);
  }
  for (int i = credCount; i < MAX_CREDS; i++) {
    String ks = "s" + String(i), kp = "p" + String(i);
    prefs.remove(ks.c_str()); prefs.remove(kp.c_str());
  }
  prefs.end();
}
int findCred(const char* s) { for (int i = 0; i < credCount; i++) if (!strcmp(creds[i].ssid, s)) return i; return -1; }
bool addCred(const char* s, const char* p) {
  int i = findCred(s);
  if (i >= 0) { strncpy(creds[i].pass, p, 64); creds[i].pass[64] = 0; }
  else {
    if (credCount >= MAX_CREDS) return false;
    strncpy(creds[credCount].ssid, s, 32); creds[credCount].ssid[32] = 0;
    strncpy(creds[credCount].pass, p, 64); creds[credCount].pass[64] = 0;
    credCount++;
  }
  saveCredsToNVS(); return true;
}
bool delCred(int i) {
  if (i < 0 || i >= credCount) return false;
  for (int k = i; k < credCount - 1; k++) creds[k] = creds[k + 1];
  credCount--; saveCredsToNVS(); return true;
}

// ---- IR база ----
enum IrProto : uint8_t { P_NEC=0, P_SAMSUNG, P_SONY12, P_SONY15, P_PANASONIC, P_RC5, P_JVC };
struct IrDevice { const char* name; IrProto proto; uint32_t keys[14]; };
#define K14(p) { p, 0,0,0,0,0,0,0,0,0,0,0,0,0 }
#define KFULL(p,vp,vm,mu,cp,cm,up,dn,lt,rt,ok,mn,bk,in) { p,vp,vm,mu,cp,cm,up,dn,lt,rt,ok,mn,bk,in }

const IrDevice irProj[] = {
  { "Asus",        P_NEC,       K14(0x10C8E11E) },
  { "ViewSonic",   P_NEC,       K14(0xF483E817) },
  { "BenQ",        P_NEC,       K14(0x000C40BF) },
  { "Epson",       P_NEC,       K14(0x83556F90) },
  { "Optoma",      P_NEC,       K14(0xCD327E81) },
  { "InFocus",     P_NEC,       K14(0x4E87E817) },
  { "RIF6",        P_NEC,       K14(0x00005DA2) },
  { "DBPOWER",     P_NEC,       K14(0x0000FD02) },
  { "Acer",        P_NEC,       K14(0x1010E01F) },
  { "Dell",        P_NEC,       K14(0x1010E01F) },
  { "Sony Proj",   P_SONY12,    K14(0xA90) },
  { "Hitachi",     P_NEC,       K14(0x00C7A25D) },
  { "JVC Proj",    P_JVC,       K14(0xC5A2) },
  { "Mitsubishi",  P_NEC,       K14(0x000042BD) },
  { "NEC",         P_NEC,       K14(0x00FF00FF) },
  { "Panasonic P", P_PANASONIC, K14(0x0100BCBD) },
  { "Philips P",   P_NEC,       K14(0x18E7B04F) },
  { "Sharp",       P_NEC,       K14(0x5110EC13) },
  { "Toshiba",     P_NEC,       K14(0x02FD48B7) },
  { "Vivitek",     P_NEC,       K14(0x000C40BF) },
  { "3M",          P_NEC,       K14(0x1CE350AF) },
  { "Canon",       P_NEC,       K14(0x7E8150AF) },
  { "Ricoh",       P_NEC,       K14(0x1CE350AF) },
  { "Xiaomi",      P_NEC,       K14(0x20DF10EF) },
  { "Polaroid",    P_NEC,       K14(0x20DF10EF) },
};
const int irProjCount = sizeof(irProj) / sizeof(irProj[0]);

const IrDevice irTv[] = {
  { "LG 04FB",  P_NEC,  KFULL(0x04FB08F7,
                              0x04FB40BF, 0x04FBC03F, 0x04FB906F,
                              0x04FB00FF, 0x04FB807F,
                              0x04FB02FD, 0x04FB827D,
                              0x04FBE01F, 0x04FB609F, 0x04FB22DD,
                              0x04FBC23D, 0x04FB14EB, 0x04FBD02F) },
  { "LG 20DF",  P_NEC,  KFULL(0x20DF10EF,
                              0x20DF40BF, 0x20DFC03F, 0x20DF906F,
                              0x20DF00FF, 0x20DF807F,
                              0x20DF02FD, 0x20DF827D,
                              0x20DFE01F, 0x20DF609F, 0x20DF22DD,
                              0x20DFC23D, 0x20DF14EB, 0x20DFD02F) },
  { "Samsung",  P_SAMSUNG, KFULL(0xE0E040BF,
                                 0xE0E0E01F, 0xE0E0D02F, 0xE0E0F00F,
                                 0xE0E048B7, 0xE0E008F7,
                                 0xE0E006F9, 0xE0E08679,
                                 0xE0E0A659, 0xE0E046B9, 0xE0E016E9,
                                 0xE0E058A7, 0xE0E0B44B, 0xE0E0807F) },
  { "Sony",     P_SONY12, KFULL(0xA90,
                                0x490, 0xC90, 0x290,
                                0x090, 0x890,
                                0x2F0, 0xAF0,
                                0x2D0, 0xCD0, 0x650,
                                0x070, 0x63C, 0x2B0) },
  { "Panasonic",P_PANASONIC, KFULL(0x0100BCBD,
                                   0x0100BC00, 0x0100BCC0, 0x0100BCC1,
                                   0x0100BC04, 0x0100BCC4,
                                   0x0100BC52, 0x0100BCD2,
                                   0x0100BC50, 0x0100BCD0, 0x0100BC53,
                                   0x0100BC4E, 0x0100BC4F, 0x0100BC10) },
  { "Philips",  P_NEC,   K14(0x10EFC03F) },
  { "Toshiba",  P_NEC,   K14(0x02FD48B7) },
  { "Sharp",    P_NEC,   K14(0x5110EC13) },
  { "Hisense",  P_SAMSUNG, K14(0xE0E040BF) },
  { "TCL",      P_SAMSUNG, K14(0xE0E040BF) },
  { "Xiaomi",   P_NEC,   K14(0x20DF10EF) },
  { "Vizio",    P_NEC,   K14(0x20DF10EF) },
  { "Grundig",  P_NEC,   K14(0x000B24DB) },
  { "JVC",      P_JVC,   K14(0xC5A2) },
  { "Polaroid", P_NEC,   K14(0x20DF10EF) },
  { "RCA",      P_SAMSUNG, K14(0xE0E040BF) },
};
const int irTvCount = sizeof(irTv) / sizeof(irTv[0]);

int irProjIdx = 0;
int irTvIdx = 0;
int irKeyIdx = 0;
int irKeyDevice = 0;
bool irKeyIsTv = false;
bool irSent = false;
uint32_t irSentAt = 0;
int irAutoIdx = 0;
bool irAutoRunning = false;
uint32_t irAutoLast = 0;
const char* keyNames[14] = { "PW","V+","V-","MU","C+","C-","UP","DN","<",">","OK","MN","BK","IN" };
const int keyCount = 14;
#define MAX_IR_FAVORITES 8
#define IR_FAVORITE_EMPTY 0xFF
struct IrFavorite { uint8_t isTv, devIdx, keyIdx; };
static_assert(sizeof(IrFavorite) == 3, "IR favorite storage layout changed");
IrFavorite irFavorites[MAX_IR_FAVORITES];
int irFavoriteIdx = 0;
int irFavoriteFeedback = 0;
uint32_t irFavoriteFeedbackAt = 0;

void clearIrFavorite(int i) {
  irFavorites[i].isTv = 0;
  irFavorites[i].devIdx = 0;
  irFavorites[i].keyIdx = IR_FAVORITE_EMPTY;
}
void loadIrFavorites() {
  for (int i = 0; i < MAX_IR_FAVORITES; i++) clearIrFavorite(i);
  prefs.begin("irfav", true);
  if (prefs.getBytesLength("data") == sizeof(irFavorites))
    prefs.getBytes("data", irFavorites, sizeof(irFavorites));
  prefs.end();
  for (int i = 0; i < MAX_IR_FAVORITES; i++) {
    IrFavorite &f = irFavorites[i];
    int count = f.isTv ? irTvCount : irProjCount;
    if (f.keyIdx >= keyCount || f.isTv > 1 || f.devIdx >= count ||
        (f.keyIdx < keyCount && (f.isTv ? irTv[f.devIdx].keys[f.keyIdx] : irProj[f.devIdx].keys[f.keyIdx]) == 0))
      clearIrFavorite(i);
  }
}
bool saveIrFavorites() {
  prefs.begin("irfav", false);
  size_t written = prefs.putBytes("data", irFavorites, sizeof(irFavorites));
  prefs.end();
  return written == sizeof(irFavorites);
}
// Returns 1 when added, 2 if already saved, and 0 when empty/full/storage failed.
int addIrFavorite(bool isTv, uint8_t devIdx, uint8_t keyIdx) {
  int deviceCount = isTv ? irTvCount : irProjCount;
  if (keyIdx >= keyCount || devIdx >= deviceCount) return 0;
  const IrDevice &dev = isTv ? irTv[devIdx] : irProj[devIdx];
  if (!dev.keys[keyIdx]) return 0;
  int empty = -1;
  for (int i = 0; i < MAX_IR_FAVORITES; i++) {
    IrFavorite &f = irFavorites[i];
    if (f.keyIdx == keyIdx && f.isTv == (isTv ? 1 : 0) && f.devIdx == devIdx) return 2;
    if (empty < 0 && f.keyIdx == IR_FAVORITE_EMPTY) empty = i;
  }
  if (empty < 0) return 0;
  irFavorites[empty] = { (uint8_t)(isTv ? 1 : 0), devIdx, keyIdx };
  if (!saveIrFavorites()) { clearIrFavorite(empty); return 0; }
  return 1;
}
bool removeIrFavorite(int i) {
  if (i < 0 || i >= MAX_IR_FAVORITES || irFavorites[i].keyIdx == IR_FAVORITE_EMPTY) return false;
  IrFavorite old = irFavorites[i];
  clearIrFavorite(i);
  if (saveIrFavorites()) return true;
  irFavorites[i] = old;
  return false;
}
void setIrNavigationOnRelease(bool onRelease);

void sendIR(const IrDevice& dev, int key) {
  uint32_t code = dev.keys[key];
  if (!code) return;
  switch (dev.proto) {
    case P_NEC:       irSend.sendNEC(code, 32, 1); break;
    case P_SAMSUNG:   irSend.sendSAMSUNG(code, 32, 1); break;
    case P_SONY12:    irSend.sendSony(code, 12, 1); break;
    case P_SONY15:    irSend.sendSony(code, 15, 1); break;
    case P_PANASONIC: irSend.sendPanasonic(0x4004, code, 48, 1); break;
    case P_RC5:       irSend.sendRC5(code, 12, 1); break;
    case P_JVC:       irSend.sendJVC(code, 16, 0); break;
  }
  irSent = true;
  irSentAt = millis();
}

// ---- Macros ----
#define MAX_MACROS 5
#define MAX_STEPS 10
struct MacroStep { uint8_t isTv; uint8_t devIdx; uint8_t keyIdx; };
struct Macro { uint8_t nSteps; MacroStep steps[MAX_STEPS]; };
Macro macros[MAX_MACROS];
int macroSelIdx = 0;
int macroEditIdx = 0;
bool macroRecording = false;
bool macroPlaying = false;
int macroPlayIdx = 0;
uint32_t macroLastStep = 0;

void loadMacros() {
  prefs.begin("macro", true);
  if (prefs.isKey("data")) prefs.getBytes("data", macros, sizeof(macros));
  else memset(macros, 0, sizeof(macros));
  prefs.end();
}
void saveMacros() {
  prefs.begin("macro", false);
  prefs.putBytes("data", macros, sizeof(macros));
  prefs.end();
}
void startMacroPlay(int mIdx) {
  if (macros[mIdx].nSteps == 0) return;
  macroPlaying = true;
  macroPlayIdx = 0;
  macroLastStep = millis() - 500;
}
void stepMacroPlay() {
  if (!macroPlaying) return;
  if (millis() - macroLastStep < 400) return;
  macroLastStep = millis();
  Macro &m = macros[macroSelIdx];
  if (macroPlayIdx >= m.nSteps) { macroPlaying = false; return; }
  MacroStep &s = m.steps[macroPlayIdx++];
  const IrDevice* dev = s.isTv ? &irTv[s.devIdx] : &irProj[s.devIdx];
  if (s.keyIdx < 14 && dev->keys[s.keyIdx]) sendIR(*dev, s.keyIdx);
  if (macroPlayIdx >= m.nSteps) macroPlaying = false;
}

// ---- NTP ----
bool ntpSynced = false;
int clockHour = 0, clockMin = 0;
uint32_t lastClockUpdate = 0;
enum NtpState { NTP_IDLE, NTP_CONNECTING, NTP_SYNCING };
NtpState ntpState = NTP_IDLE;
uint32_t ntpAttemptStart = 0;
uint32_t lastNtpAttempt = 0;
int ntpAttemptCred = 0;

void ntpTrigger() { ntpSynced = false; lastNtpAttempt = 0; ntpState = NTP_IDLE; }
void updateClock() {
  if (!ntpSynced || millis() - lastClockUpdate < 1000) return;
  lastClockUpdate = millis();
  time_t n; struct tm t; time(&n); localtime_r(&n, &t);
  clockHour = t.tm_hour; clockMin = t.tm_min;
}
void tickNtp() {
  uint32_t now = millis();
  if (ntpState == NTP_IDLE) {
    if (ntpSynced || credCount == 0) return;
    if (lastNtpAttempt && now - lastNtpAttempt < 60000) return;
    ntpState = NTP_CONNECTING; ntpAttemptStart = now; lastNtpAttempt = now;
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    WiFi.begin(creds[ntpAttemptCred].ssid, creds[ntpAttemptCred].pass);
  } else if (ntpState == NTP_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
      ntpState = NTP_SYNCING; ntpAttemptStart = now;
    } else if (now - ntpAttemptStart > 12000) {
      WiFi.disconnect(); WiFi.mode(WIFI_OFF);
      ntpAttemptCred = (ntpAttemptCred + 1) % credCount; ntpState = NTP_IDLE;
    }
  } else {
    if (time(nullptr) > 1700000000) {
      ntpSynced = true; WiFi.disconnect(); WiFi.mode(WIFI_OFF);
      ntpState = NTP_IDLE; updateClock();
    } else if (now - ntpAttemptStart > 8000) {
      WiFi.disconnect(); WiFi.mode(WIFI_OFF);
      ntpAttemptCred = (ntpAttemptCred + 1) % credCount; ntpState = NTP_IDLE;
    }
  }
}

// ---- Battery ----
#define BG_POINTS 96
int8_t battGraph[BG_POINTS];
int battGraphIdx = 0, battGraphCount = 0;
uint32_t lastBattSample = 0;
bool battFirstDone = false, fatReady = false, battValid = false;
float battVoltage = 0.0f;
int battPct = -1;
uint32_t lastBattMs = 0, lastBtnEventMs = 0;
#define LOW_BATTERY_WARN_PCT 15
#define DISPLAY_SLEEP_MS 90000UL
#define AUTO_DEEP_SLEEP_MS 120000UL
bool displaySleeping = false;

float readBatteryVoltage() {
  const int N = 9;
  uint32_t v[N];
  analogReadMilliVolts(BATT_ADC); delayMicroseconds(500);
  for (int i = 0; i < N; i++) { v[i] = analogReadMilliVolts(BATT_ADC); delay(1); }
  for (int i = 1; i < N; i++) { uint32_t k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
  static float ema = -1;
  float med = v[N / 2];
  ema = (ema < 0) ? med : ema * 0.9f + med * 0.1f;
  return ema * battCal / 1000.0f;
}
int battPercentFromV(float v) {
  if (v >= 4.15f) return 100;
  if (v >= 4.00f) return (int)(90 + (v - 4.00f) / 0.15f * 10);
  if (v >= 3.85f) return (int)(75 + (v - 3.85f) / 0.15f * 15);
  if (v >= 3.70f) return (int)(55 + (v - 3.70f) / 0.15f * 20);
  if (v >= 3.60f) return (int)(35 + (v - 3.60f) / 0.10f * 20);
  if (v >= 3.50f) return (int)(15 + (v - 3.50f) / 0.10f * 20);
  if (v >= 3.40f) return (int)(5 + (v - 3.40f) / 0.10f * 10);
  if (v >= 3.30f) return (int)((v - 3.30f) / 0.10f * 5);
  return 0;
}
#define BATT_FILE_HEADER_SIZE 9
#define BATT_FILE_SIZE (BATT_FILE_HEADER_SIZE + BG_POINTS + 4)

uint32_t battGraphCrc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}
bool validBattGraphSamples(const int8_t* samples) {
  for (int i = 0; i < BG_POINTS; i++)
    if (samples[i] < -1 || samples[i] > 100) return false;
  return true;
}
bool readBattGraphFile(const char* path, bool allowLegacy, bool* wasLegacy = nullptr) {
  File f = FFat.open(path, "r");
  if (!f) return false;
  size_t size = f.size();
  if (size == BATT_FILE_SIZE) {
    uint8_t data[BATT_FILE_SIZE];
    bool ok = (f.read(data, sizeof(data)) == sizeof(data));
    f.close();
    if (!ok || memcmp(data, "BTG2", 4) != 0 || data[4] != 2) return false;
    uint32_t storedCrc = (uint32_t)data[BATT_FILE_SIZE - 4] |
                         ((uint32_t)data[BATT_FILE_SIZE - 3] << 8) |
                         ((uint32_t)data[BATT_FILE_SIZE - 2] << 16) |
                         ((uint32_t)data[BATT_FILE_SIZE - 1] << 24);
    if (battGraphCrc32(data, BATT_FILE_SIZE - 4) != storedCrc) return false;
    uint16_t idx = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
    uint16_t count = (uint16_t)data[7] | ((uint16_t)data[8] << 8);
    int8_t samples[BG_POINTS];
    memcpy(samples, data + BATT_FILE_HEADER_SIZE, BG_POINTS);
    if (idx >= BG_POINTS || count > BG_POINTS || !validBattGraphSamples(samples)) return false;
    memcpy(battGraph, samples, BG_POINTS);
    battGraphIdx = idx;
    battGraphCount = count;
    if (wasLegacy) *wasLegacy = false;
    return true;
  }
  // v24 legacy layout on ESP32: 96 samples followed by two 32-bit ints.
  if (allowLegacy && size == BG_POINTS + 8) {
    int32_t idx = 0, count = 0;
    int8_t samples[BG_POINTS];
    bool ok = (f.read((uint8_t*)samples, BG_POINTS) == BG_POINTS) &&
              (f.read((uint8_t*)&idx, sizeof(idx)) == sizeof(idx)) &&
              (f.read((uint8_t*)&count, sizeof(count)) == sizeof(count));
    f.close();
    if (!ok || idx < 0 || idx >= BG_POINTS || count < 0 || count > BG_POINTS ||
        !validBattGraphSamples(samples)) return false;
    memcpy(battGraph, samples, BG_POINTS);
    battGraphIdx = idx;
    battGraphCount = count;
    if (wasLegacy) *wasLegacy = true;
    return true;
  }
  f.close();
  return false;
}
void saveBattGraph() {
  if (!fatReady) return;
  uint8_t data[BATT_FILE_SIZE] = { 'B', 'T', 'G', '2', 2, 0, 0, 0, 0 };
  uint16_t idx = (uint16_t)constrain(battGraphIdx, 0, BG_POINTS - 1);
  uint16_t count = (uint16_t)constrain(battGraphCount, 0, BG_POINTS);
  data[5] = idx & 0xFF; data[6] = idx >> 8;
  data[7] = count & 0xFF; data[8] = count >> 8;
  memcpy(data + BATT_FILE_HEADER_SIZE, battGraph, BG_POINTS);
  uint32_t crc = battGraphCrc32(data, BATT_FILE_SIZE - 4);
  data[BATT_FILE_SIZE - 4] = crc & 0xFF;
  data[BATT_FILE_SIZE - 3] = (crc >> 8) & 0xFF;
  data[BATT_FILE_SIZE - 2] = (crc >> 16) & 0xFF;
  data[BATT_FILE_SIZE - 1] = (crc >> 24) & 0xFF;

  File f = FFat.open("/batt.new", "w");
  if (!f) return;
  size_t written = f.write(data, sizeof(data));
  f.flush();
  f.close();
  if (written != sizeof(data) || !readBattGraphFile("/batt.new", false)) {
    FFat.remove("/batt.new");
    return;
  }

  // The fully written and CRC-checked .new file remains recoverable while replacing the old one.
  if (FFat.exists("/batt.bin") && !FFat.remove("/batt.bin")) return;
  if (!FFat.rename("/batt.new", "/batt.bin")) return;
  if (FFat.exists("/batt.tmp")) FFat.remove("/batt.tmp");
  if (FFat.exists("/batt.bak")) FFat.remove("/batt.bak");
}
void loadBattGraph() {
  if (!fatReady) return;
  memset(battGraph, 0, sizeof(battGraph));
  battGraphIdx = 0; battGraphCount = 0;
  bool legacy = false;
  if (readBattGraphFile("/batt.bin", false)) return;
  if (readBattGraphFile("/batt.new", false)) {
    if (FFat.exists("/batt.bin")) FFat.remove("/batt.bin");
    FFat.rename("/batt.new", "/batt.bin");
    return;
  }
  if (readBattGraphFile("/batt.tmp", false)) return; // legacy recovery file from earlier builds
  if (readBattGraphFile("/batt.bak", false)) return;
  if (readBattGraphFile("/batt.bin", true, &legacy) && legacy) saveBattGraph();
}
void clearBattGraph() { memset(battGraph, 0, sizeof(battGraph)); battGraphIdx = 0; battGraphCount = 0; saveBattGraph(); }
void updateBattery() {
  uint32_t now = millis();
  uint32_t readInterval = displaySleeping ? 5000UL : 500UL;
  if (now - lastBattMs < readInterval || now - lastBtnEventMs < 150) return;
  lastBattMs = now;
  bool radioBusy = (scanMode == SCAN_ON) || (ntpState != NTP_IDLE) || chatServerRunning ||
                   deauthRunning || cloneRunning || spamRunning || snifferRunning;
  if (radioBusy) { lastRadioActiveMs = now; return; }
  if (lastRadioActiveMs && now - lastRadioActiveMs < 3000) return;
  static float prevV = 0;
  static uint32_t prevVms = 0;
  float v = readBatteryVoltage();
  if (v < BATT_MIN || v > BATT_MAX) { battValid = false; battPct = -1; }
  else {
    battValid = true;
    int np = constrain(battPercentFromV(v), 0, 100);
    bool sag = (prevV > 0 && now - prevVms < 5000 && (prevV - v) > 0.15f);
    if (battPct < 0 || np > battPct) battPct = np;
    else if (np < battPct && abs(np - battPct) >= 3 && !sag) battPct = np;
  }
  battVoltage = v; prevV = v; prevVms = now;
  uint32_t th = battFirstDone ? 300000 : 30000;
  if (now - lastBattSample >= th) {
    lastBattSample = now; battFirstDone = true;
    battGraph[battGraphIdx] = battValid ? (int8_t)constrain((int)(v * 20), 0, 100) : -1;
    battGraphIdx = (battGraphIdx + 1) % BG_POINTS;
    if (battGraphCount < BG_POINTS) battGraphCount++;
    saveBattGraph();
  }
}
void drawBatteryIcon(int x, int y) {
  display.drawRect(x, y, 12, 6, SSD1306_WHITE);
  display.drawRect(x + 12, y + 1, 2, 4, SSD1306_WHITE);
  if (!battValid) { display.drawLine(x + 1, y + 5, x + 10, y + 1, SSD1306_WHITE); return; }
  int fp = map(battPct, 0, 100, 0, 10);
  if (fp > 0) display.fillRect(x + 1, y + 1, fp, 4, SSD1306_WHITE);
  if (battPct <= LOW_BATTERY_WARN_PCT) {
    display.drawFastVLine(x + 8, y + 1, 3, SSD1306_WHITE);
    display.drawPixel(x + 8, y + 5, SSD1306_WHITE);
  }
}

// ---- Offline mini neural-network chat portal ----
const char chatHtml[] PROGMEM = R"CHATHTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tricorder Tiny RNN</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#101522;color:#e7eefc;font:16px system-ui,sans-serif;display:grid;min-height:100vh;place-items:center}
main{width:min(680px,100%);height:min(760px,100vh);padding:18px;display:flex;flex-direction:column;gap:12px}h1{font-size:1.2rem;margin:0}small{color:#9eacc5}.log{flex:1;overflow:auto;background:#171f30;border:1px solid #2b3850;border-radius:14px;padding:12px;display:flex;flex-direction:column;gap:10px}
.msg{max-width:90%;padding:10px 12px;border-radius:12px;white-space:pre-wrap;line-height:1.4}.bot{align-self:flex-start;background:#25324a}.user{align-self:flex-end;background:#245e73}.row{display:flex;gap:8px}input{min-width:0;flex:1;background:#171f30;color:white;border:1px solid #3b4b67;border-radius:10px;padding:12px;font:inherit}button{border:0;border-radius:10px;padding:0 18px;background:#67d5c3;color:#071a20;font-weight:700;font:inherit}
</style></head><body><main><h1>Tricorder · Tiny Chat</h1><small>Офлайн-гибрид: маленький классификатор задаёт тему, а посимвольная RNN генерирует ответ. Это эксперимент, не полноценная LLM; ответы ограничены обучающими примерами.</small>
<div id="log" class="log"><div class="msg bot">Привет! Я генерирую ответы офлайн на маленькой RNN. Могу ошибаться.</div></div><form id="form" class="row"><input id="text" maxlength="120" autocomplete="off" placeholder="Напишите сообщение…"><button>Отправить</button></form></main>
<script>const log=document.querySelector('#log'),input=document.querySelector('#text');function add(t,c){const d=document.createElement('div');d.className='msg '+c;d.textContent=t;log.append(d);log.scrollTop=log.scrollHeight}document.querySelector('#form').addEventListener('submit',async e=>{e.preventDefault();const q=input.value.trim();if(!q)return;input.value='';add(q,'user');try{const r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:q.toLocaleLowerCase()});add(await r.text(),'bot')}catch(_){add('Связь с устройством потеряна. Подключитесь снова к Tricorder-Chat.','bot')}});</script></body></html>
)CHATHTML";

uint32_t chatFnv1a(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; i++) { hash ^= bytes[i]; hash *= 16777619UL; }
  return hash;
}
void chatAddToken(const uint8_t* token, size_t length, float* features) {
  if (!length) return;
  features[chatFnv1a(token, length) & (CHAT_FEATURES - 1)] = 1.0f;
  for (size_t i = 0; i + 2 < length; i++)
    features[chatFnv1a(token + i, 3) & (CHAT_FEATURES - 1)] = 1.0f;
}
void chatBuildFeatures(const String& text, float* features) {
  memset(features, 0, sizeof(float) * CHAT_FEATURES);
  uint8_t token[121], previousToken[121]; size_t tokenLength = 0, previousLength = 0;
  const uint8_t* raw = (const uint8_t*)text.c_str();
  size_t length = text.length();
  for (size_t i = 0; i <= length;) {
    uint8_t b = i < length ? raw[i] : (uint8_t)' ';
    // Match Python str.lower() for ASCII and common Russian Cyrillic uppercase.
    if (i < length && b == 0xD0 && i + 1 < length && raw[i + 1] == 0x81) {
      token[tokenLength++] = 0xD1; token[tokenLength++] = 0x91; i += 2; continue;
    }
    if (i < length && b == 0xD0 && i + 1 < length && raw[i + 1] >= 0x90 && raw[i + 1] <= 0xAF) {
      token[tokenLength++] = 0xD0; token[tokenLength++] = raw[i + 1] + 0x20; i += 2; continue;
    }
    if (b >= 'A' && b <= 'Z') b += 32;
    bool word = b >= 128 || b == '_' || (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z');
    if (word && i < length && tokenLength < sizeof(token)) token[tokenLength++] = b;
    else if (tokenLength) {
      chatAddToken(token, tokenLength, features);
      if (previousLength) {
        uint8_t pair[243];
        memcpy(pair, previousToken, previousLength); pair[previousLength] = ' ';
        memcpy(pair + previousLength + 1, token, tokenLength);
        features[chatFnv1a(pair, previousLength + 1 + tokenLength) & (CHAT_FEATURES - 1)] = 1.0f;
      }
      memcpy(previousToken, token, tokenLength); previousLength = tokenLength; tokenLength = 0;
    }
    i++;
  }
}
int classifyChatIntent(const String& text) {
  float input[CHAT_FEATURES];
  float hidden1[CHAT_H1], hidden2[CHAT_H2];
  chatBuildFeatures(text, input);
  for (int h = 0; h < CHAT_H1; h++) {
    float sum = chatB1[h];
    for (int i = 0; i < CHAT_FEATURES; i++)
      if (input[i] != 0.0f) sum += input[i] * (float)chatW1Q[i * CHAT_H1 + h] * chatScale1;
    hidden1[h] = sum > 0.0f ? sum : 0.0f;
  }
  for (int h = 0; h < CHAT_H2; h++) {
    float sum = chatB2[h];
    for (int i = 0; i < CHAT_H1; i++) sum += hidden1[i] * (float)chatW2Q[i * CHAT_H2 + h] * chatScale2;
    hidden2[h] = sum > 0.0f ? sum : 0.0f;
  }
  int best = 0; float bestScore = -1000000.0f, secondScore = -1000000.0f;
  for (int o = 0; o < CHAT_CLASSES; o++) {
    float score = chatB3[o];
    for (int i = 0; i < CHAT_H2; i++) score += hidden2[i] * (float)chatW3Q[i * CHAT_CLASSES + o] * chatScale3;
    if (score > bestScore) { secondScore = bestScore; bestScore = score; best = o; }
    else if (score > secondScore) secondScore = score;
  }
  // Avoid presenting a confident-looking answer for an ambiguous/out-of-domain prompt.
  return (bestScore - secondScore < 0.12f) ? -1 : best;
}

// A compact intent router supplies a topic tag to a prompt-conditioned character RNN.
// The 96-state RNN predicts one Unicode character at a time; replies are not looked up.
// This hybrid is an experimental generator, not a capable general-purpose LLM.
int chatCharId(uint32_t cp) {
  for (int i = 0; i < CHAT_VOCAB; i++) if (chatChars[i] == cp) return i;
  for (int i = 0; i < CHAT_VOCAB; i++) if (chatChars[i] == '?') return i;
  return 0;
}
uint32_t chatReadCodepoint(const uint8_t* bytes, size_t length, size_t& pos) {
  if (pos >= length) return 0;
  uint8_t a = bytes[pos++];
  if (a < 0x80) return a;
  if ((a & 0xE0) == 0xC0 && pos < length) return ((uint32_t)(a & 0x1F) << 6) | (bytes[pos++] & 0x3F);
  if ((a & 0xF0) == 0xE0 && pos + 1 < length) {
    uint8_t b = bytes[pos++], c = bytes[pos++];
    return ((uint32_t)(a & 0x0F) << 12) | ((uint32_t)(b & 0x3F) << 6) | (c & 0x3F);
  }
  if ((a & 0xF8) == 0xF0 && pos + 2 < length) { pos += 3; return '?'; }
  return '?';
}
uint32_t chatLowerCodepoint(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
  if (cp == 0x0401) return 0x0451;
  return cp;
}
void chatRnnStep(int inputId, float* state, float* logits) {
  float next[CHAT_HIDDEN];
  for (int h = 0; h < CHAT_HIDDEN; h++) {
    float sum = chatBh[h] + (float)chatWxhQ[inputId * CHAT_HIDDEN + h] * chatScaleX;
    for (int j = 0; j < CHAT_HIDDEN; j++)
      sum += state[j] * (float)chatWhhQ[j * CHAT_HIDDEN + h] * chatScaleH;
    next[h] = tanhf(sum);
  }
  memcpy(state, next, sizeof(next));
  for (int o = 0; o < CHAT_VOCAB; o++) {
    float sum = chatBy[o];
    for (int h = 0; h < CHAT_HIDDEN; h++)
      sum += state[h] * (float)chatWhyQ[h * CHAT_VOCAB + o] * chatScaleY;
    logits[o] = sum;
  }
}
void chatFeedText(const char* text, float* state, float* logits) {
  const uint8_t* bytes = (const uint8_t*)text; size_t length = strlen(text), pos = 0;
  while (pos < length) chatRnnStep(chatCharId(chatReadCodepoint(bytes, length, pos)), state, logits);
}
void chatAppendCodepoint(String& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}
uint32_t chatSampleChar(const float* logits) {
  int best = 0;
  for (int i = 1; i < CHAT_VOCAB; i++) if (logits[i] > logits[best]) best = i;
  return chatChars[best]; // greedy decoding avoids garbled output on this tiny training set
}
const char* const chatClassTags[CHAT_CLASSES] = {
  "приветствие", "батарея", "время", "обзор_wifi", "каналы_wifi", "ик", "игры",
  "энергосбережение", "устройство", "диагностика_сети", "макрос", "сохраненные_сети",
  "помощь", "благодарность", "шутка", "прощание", "вне_тем"
};
String tinyChatReply(const String& text) {
  float state[CHAT_HIDDEN] = {0}; float logits[CHAT_VOCAB];
  chatFeedText("Вопрос: ", state, logits);
  const uint8_t* bytes = (const uint8_t*)text.c_str(); size_t length = text.length(), pos = 0;
  while (pos < length) {
    uint32_t cp = chatLowerCodepoint(chatReadCodepoint(bytes, length, pos));
    chatRnnStep(chatCharId(cp), state, logits);
  }
  int topic = classifyChatIntent(text);
  if (topic < 0 || topic >= CHAT_CLASSES) topic = CHAT_CLASSES - 1;
  chatFeedText("\nТема: ", state, logits);
  chatFeedText(chatClassTags[topic], state, logits);
  chatFeedText("\nОтвет: ", state, logits);
  String answer;
  for (int i = 0; i < 180; i++) {
    uint32_t cp = chatSampleChar(logits);
    if (cp == '\n' || cp == '\r') break;
    chatAppendCodepoint(answer, cp);
    chatRnnStep(chatCharId(cp), state, logits);
  }
  answer.trim();
  if (answer.length() == 0) return "Я не смог сформировать ответ. Попробуй спросить о функциях устройства.";
  return answer;
}
void handleChatPage() {
  chatWeb.send_P(200, "text/html; charset=utf-8", chatHtml);
}
void handleChatMessage() {
  String message = chatWeb.arg("plain");
  message.trim();
  if (message.length() == 0) { chatWeb.send(400, "text/plain; charset=utf-8", "Введите сообщение."); return; }
  if (message.length() > 120) { chatWeb.send(413, "text/plain; charset=utf-8", "Слишком длинное сообщение (максимум 120 символов)."); return; }
  message.toLowerCase();
  chatWeb.send(200, "text/plain; charset=utf-8", tinyChatReply(message));
}
void configureChatRoutes() {
  chatWeb.on("/", HTTP_GET, handleChatPage);
  chatWeb.on("/api/chat", HTTP_POST, handleChatMessage);
  chatWeb.onNotFound(handleChatPage);
}
bool startChatPortal() {
  if (chatServerRunning) return true;
  haltScan();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, ip, mask)) { WiFi.mode(WIFI_OFF); return false; }
  chatApPassword = String((uint32_t)(esp_random() % 90000000UL) + 10000000UL);
  if (!WiFi.softAP("Tricorder-Chat", chatApPassword.c_str())) { WiFi.mode(WIFI_OFF); return false; }
  chatWeb.begin();
  chatServerRunning = true;
  return true;
}
void stopChatPortal() {
  if (!chatServerRunning) return;
  chatServerRunning = false;
  chatWeb.close();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}
void drawChatPortal() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Tiny RNN Chat");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (chatPortalError) {
    display.setCursor(2, 24); display.print("Could not start AP");
    display.setCursor(2, 56); display.print("hM=back");
  } else {
    display.setCursor(2, 14); display.print("WiFi Tricorder-Chat");
    display.setCursor(2, 25); display.print("Pass "); display.print(chatApPassword);
    display.setCursor(2, 36); display.print("192.168.4.1");
    display.setCursor(2, 56); display.print("Connect; hM=stop");
  }
  display.display();
}

// ---- Buttons ----
const uint16_t DEBOUNCE_MS = 15;
const uint16_t LONG_PRESS_MS = 700;
struct Btn {
  uint8_t pin;
  bool lastRaw, stableState;
  uint32_t changeAt, pressStart;
  bool longFired, fireOnPress, shortEvent, longEvent;
};
Btn btnLeft  = { BTN_L, HIGH, HIGH, 0, 0, false, true, false, false };
Btn btnRight = { BTN_R, HIGH, HIGH, 0, 0, false, true, false, false };
Btn btnSelect = { BTN_M, HIGH, HIGH, 0, 0, false, false, false, false };

void pollButton(Btn &b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();
  if (raw != b.lastRaw) { b.lastRaw = raw; b.changeAt = now; }
  if (now - b.changeAt >= DEBOUNCE_MS && raw != b.stableState) {
    b.stableState = raw;
    if (raw == LOW) { b.pressStart = now; b.longFired = false; if (b.fireOnPress) b.shortEvent = true; }
    else if (!b.longFired && !b.fireOnPress) b.shortEvent = true;
  }
  if (b.stableState == LOW && !b.longFired && now - b.pressStart >= LONG_PRESS_MS) {
    b.longFired = true; b.longEvent = true; b.shortEvent = false;
  }
  if (b.shortEvent || b.longEvent) lastBtnEventMs = now;
}
bool takeShort(Btn &b) { if (b.shortEvent) { b.shortEvent = false; return true; } return false; }
bool takeLong(Btn &b) { if (b.longEvent) { b.longEvent = false; return true; } return false; }
void suppressLong(Btn &b) { b.longFired = true; b.longEvent = false; b.shortEvent = false; }
void suppressAll() { suppressLong(btnLeft); suppressLong(btnRight); suppressLong(btnSelect); }
void setIrNavigationOnRelease(bool onRelease) {
  btnLeft.fireOnPress = !onRelease;
  btnRight.fireOnPress = !onRelease;
}
bool anyBtnEvent() { return btnLeft.shortEvent || btnLeft.longEvent || btnRight.shortEvent || btnRight.longEvent || btnSelect.shortEvent || btnSelect.longEvent; }

// ---- Input / keyboard ----
String* inputTarget = nullptr;
String inputLabel = "", inputContext = "";
int inputMaxLen = 64;
enum InputAction { INPUT_NONE, INPUT_TEST_PASS, INPUT_ADD_SSID, INPUT_ADD_PASS };
int inputAction = INPUT_NONE;
String tempSsid = "", tempPass = "";

int kbdCat = 0;
int kbdIdx = 0;
const char* kbdCatNames[3] = { "abc", "ABC", "123" };
const char* kbdLayouts[3] = {
  "qwertyuiopasdfghjklmzxcvbn.@_-0123456789",
  "QWERTYUIOPASDFGHJKLMZXCVBN.@_-0123456789",
  "1234567890!@#$%^&*()-_=+[]{}<>.,:;/|?~`*",
};

void startInput(const char* lb, String* t, int ml, int a, const char* c = "") {
  inputLabel = lb; inputContext = c; inputTarget = t; inputMaxLen = ml; inputAction = a;
  kbdCat = 0; kbdIdx = 0;
}

// ---- Menus ----
const char* mainItems[] = { "WiFi","Proj","TV","Tools","Security","Games","System" };
const char* wifiItems[] = { "Scan","Channels","Analyze","Test","Attacks","Map" };
const char* analyzeItems[] = { "Hidden","Twins","Signal" };
const char* testItems[] = { "Ping","Speed" };
const char* attackItems[] = { "Clone","Spam" };
const char* toolsItems[] = { "Random","PassGen","Battery","BattCal","Clock","Macro","IR Fav","AI Chat" };
const char* secItems[] = { "Passwords","Sniffer" };
// Classic arcade collection plus Minesweeper.
const char* gamesItems[] = { "Snake","Tetris","Arkanoid","Pong","Space Inv","Asteroids","Pac Maze","Frogger","Doodle Jump","Flappy","Lunar Lander","Galaga","Breakout","Missile Cmd","Racing","Minesweeper" };
const char* systemItems[] = { "Info","Space" };

struct MenuLevel { const char* title; const char** items; int count; int index; int parent; };
MenuLevel menus[] = {
  { "MAIN",     mainItems,    7, 0, -1 },
  { "WiFi",     wifiItems,    6, 0,  0 },
  { "Analyze",  analyzeItems, 3, 0,  1 },
  { "Test",     testItems,    2, 0,  1 },
  { "Attacks",  attackItems,  2, 0,  1 },
  { "Tools",    toolsItems,   8, 0,  0 },
  { "Security", secItems,     2, 0,  0 },
  { "Games",    gamesItems,   16, 0, 0 },
  { "System",   systemItems,  2, 0,  0 },
};
int currentMenuIdx = 0;

enum Screen {
  SCR_MENU,
  SCR_WIFI_SCAN, SCR_WIFI_CHANNELS, SCR_WIFI_DETAIL, SCR_WIFI_MAP_SEL, SCR_WIFI_MAP,
  SCR_ANALYZE_HIDDEN, SCR_ANALYZE_TWINS, SCR_ANALYZE_SIGNAL,
  SCR_TEST_SAVED_LIST, SCR_TEST_SELECT_NET, SCR_INPUT,
  SCR_TEST_PING, SCR_TEST_SPEED,
  SCR_PWD_LIST, SCR_PWD_DETAIL, SCR_PWD_DELETE_CONFIRM, SCR_PWD_HELP, SCR_PWD_QR,
  SCR_ATTACK_DEAUTH_SEL, SCR_ATTACK_DEAUTH_RUN,
  SCR_ATTACK_CLONE_SEL, SCR_ATTACK_CLONE_RUN, SCR_ATTACK_SPAM_RUN,
  SCR_IR_POWER, SCR_IR_TV, SCR_IR_KEYS, SCR_IR_AUTO, SCR_IR_FAVORITES,
  SCR_TOOLS_RANDOM, SCR_TOOLS_PASSWORD, SCR_TOOLS_BATTERY, SCR_TOOLS_BATTCAL, SCR_TOOLS_CLOCK, SCR_TOOLS_CHAT,
  SCR_TOOLS_MACRO, SCR_TOOLS_MACRO_EDIT,
  SCR_ARCADE,
  SCR_SNIFFER, SCR_INFO, SCR_SPACE
};
Screen currentScreen = SCR_MENU;
int testParentMenu = 0;
int pwdListIdx = 0, pwdDelIdx = 0;

// =========================================================
// UI primitives
// =========================================================
int drawCenteredText(const char* t, int y, int xs, uint8_t sz) {
  display.setTextSize(sz);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(t, 0, 0, &x1, &y1, &w, &h);
  int x = (SW - w) / 2 + xs;
  if (x + (int)w < 0 || x > SW) return x;
  display.setCursor(x, y); display.print(t); return x;
}
int listStart(int sel, int cnt, int rows) {
  if (cnt <= 0) return 0;
  int s = sel - rows / 2;
  if (s < 0) s = 0;
  int ms = cnt - rows;
  if (ms < 0) ms = 0;
  return s > ms ? ms : s;
}
void drawMenuChrome() {
  MenuLevel &m = menus[currentMenuIdx];
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print(m.title);

  char counter[8];
  snprintf(counter, sizeof(counter), "%d/%d", m.index + 1, m.count);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(counter, 0, 0, &bx, &by, &bw, &bh);
  int counterX = SW - 14 - 4 - (int)bw;
  display.setCursor(counterX, 0); display.print(counter);

  if (ntpSynced) {
    char clockText[6];
    snprintf(clockText, sizeof(clockText), "%02d:%02d", clockHour, clockMin);
    int16_t tx, ty; uint16_t tw, th;
    display.getTextBounds(clockText, 0, 0, &tx, &ty, &tw, &th);
    int clockX = counterX - 6 - (int)tw;
    int16_t titleX, titleY; uint16_t titleW, titleH;
    display.getTextBounds(m.title, 0, 0, &titleX, &titleY, &titleW, &titleH);
    if (clockX >= 2 + (int)titleW + 4) {
      display.setCursor(clockX, 0); display.print(clockText);
    }
  }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
}
void drawCurrentMenu() {
  display.clearDisplay();
  MenuLevel &m = menus[currentMenuIdx];
  drawMenuChrome();
  int rows = (m.count < 3) ? m.count : 3;
  int first = listStart(m.index, m.count, rows);
  int top = 13 + (3 - rows) * 7;
  for (int row = 0; row < rows; row++) {
    int idx = first + row, y = top + row * 14;
    bool selected = idx == m.index;
    if (selected) {
      display.fillRect(3, y, SW - 6, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(7, y + 2); display.print("> ");
    } else display.setTextColor(SSD1306_WHITE);
    drawCenteredText(m.items[idx], y + 2, selected ? 4 : 0, 1);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (battValid && battPct <= LOW_BATTERY_WARN_PCT) {
    display.print("LOW BATTERY "); display.print(battPct); display.print("%");
  } else if (currentMenuIdx == 0) display.print("L/R move   M=OPEN");
  else display.print("L/R move M=OPEN hM<");
  display.display();
}
void startAnim(int dir, int ni) {
  (void)dir;
  menus[currentMenuIdx].index = ni;
  drawCurrentMenu();
}
// =========================================================
// WiFi
// =========================================================
#define MAX_APS 20
#define RSSI_HISTORY_POINTS 12
struct WifiAp {
  char ssid[33]; int rssi; int channel; char bssid[18]; int enc; bool hidden; uint8_t bssidRaw[6];
  int8_t rssiHistory[RSSI_HISTORY_POINTS]; uint8_t rssiCount; uint8_t rssiNext;
};
void pushRssiSample(WifiAp& ap, int rssi) {
  ap.rssiHistory[ap.rssiNext] = (int8_t)constrain(rssi, -127, 0);
  ap.rssiNext = (ap.rssiNext + 1) % RSSI_HISTORY_POINTS;
  if (ap.rssiCount < RSSI_HISTORY_POINTS) ap.rssiCount++;
}
WifiAp wifiAps[MAX_APS];
int wifiApCount = 0;
int scanCh = 1, wifiScrollIdx = 0;
int cmpWifiRssi(const void* a, const void* b) { return ((const WifiAp*)b)->rssi - ((const WifiAp*)a)->rssi; }
void sortWifiByRssi() { qsort(wifiAps, wifiApCount, sizeof(WifiAp), cmpWifiRssi); }
int findApByBssid(uint8_t* bssid) {
  for (int i = 0; i < wifiApCount; i++)
    if (!memcmp(wifiAps[i].bssidRaw, bssid, 6)) return i;
  return -1;
}
void startScan() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  wifiApCount = 0; wifiScrollIdx = 0; scanMode = SCAN_ON; scanCh = 1;
  WiFi.scanNetworks(true, true, false, 300, scanCh);
}
void haltScan() {
  if (scanMode == SCAN_ON) { WiFi.scanDelete(); WiFi.mode(WIFI_OFF); sortWifiByRssi(); scanMode = SCAN_OFF; }
}
bool tickScan() {
  if (scanMode != SCAN_ON) return false;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true, true, false, 300, scanCh); return false; }
  for (int i = 0; i < n; i++) {
    String b = WiFi.BSSIDstr(i);
    int ex = -1;
    for (int j = 0; j < wifiApCount; j++) if (!strcmp(wifiAps[j].bssid, b.c_str())) { ex = j; break; }
    if (ex >= 0) {
      wifiAps[ex].rssi = WiFi.RSSI(i);
      pushRssiSample(wifiAps[ex], wifiAps[ex].rssi);
    }
    else if (wifiApCount < MAX_APS) {
      WifiAp &a = wifiAps[wifiApCount++];
      memset(&a, 0, sizeof(a));
      String s = WiFi.SSID(i); a.hidden = (s.length() == 0); if (a.hidden) s = "(hidden)";
      strncpy(a.ssid, s.c_str(), 32); a.ssid[32] = 0;
      strncpy(a.bssid, b.c_str(), 17); a.bssid[17] = 0;
      WiFi.BSSID(i, a.bssidRaw);
      a.rssi = WiFi.RSSI(i); a.channel = WiFi.channel(i); a.enc = WiFi.encryptionType(i);
      pushRssiSample(a, a.rssi);
    }
  }
  WiFi.scanDelete(); scanCh++; if (scanCh > 13) scanCh = 1;
  WiFi.scanNetworks(true, true, false, 300, scanCh);
  return true;
}
void drawWifiListGeneric(const char* title, bool compact) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print(title); display.print(wifiApCount);
  if (scanMode == SCAN_ON) { display.print(" ch"); display.print(scanCh); } else display.print(" done");
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (wifiApCount == 0) { display.setCursor(20, 28); display.print(scanMode == SCAN_ON ? "Searching..." : "No networks"); display.display(); return; }
  wifiScrollIdx = constrain(wifiScrollIdx, 0, wifiApCount - 1);
  int rows = compact ? 2 : 3;
  int start = listStart(wifiScrollIdx, wifiApCount, rows);
  int vis = min(rows, wifiApCount - start);
  for (int i = 0; i < vis; i++) {
    int idx = start + i;
    int y = 12 + i * (compact ? 20 : 14), y2 = y + 8;
    bool sel = (idx == wifiScrollIdx);
    if (sel) { display.fillRect(0, y - 2, SW, compact ? 18 : 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    char nm[22]; strncpy(nm, wifiAps[idx].ssid, 20); nm[20] = 0;
    display.setCursor(2, y); display.print(nm);
    display.setCursor(2, y2); display.print("Ch"); display.print(wifiAps[idx].channel);
    int bars = constrain(map(wifiAps[idx].rssi, -90, -40, 1, 5), 0, 5);
    for (int b = 0; b < 5; b++) {
      int bh = b + 1, bx = 28 + b * 5, by = y2 + 7 - bh;
      if (b < bars) display.fillRect(bx, by, 4, bh, sel ? SSD1306_BLACK : SSD1306_WHITE);
      else display.drawRect(bx, by, 4, bh, sel ? SSD1306_BLACK : SSD1306_WHITE);
    }
    display.setCursor(58, y2); display.print(wifiAps[idx].rssi); display.print("dBm");
    display.setTextColor(SSD1306_WHITE);
  }
  if (start > 0) display.fillTriangle(SW - 6, 13, SW - 2, 13, SW - 4, 10, SSD1306_WHITE);
  if (start < wifiApCount - rows) display.fillTriangle(SW - 6, 52, SW - 2, 52, SW - 4, 49, SSD1306_WHITE);
  display.setCursor(2, 56);
  display.print(scanMode == SCAN_ON ? "L/R sel  MID=stop" : "L/R sel  MID=ok");
  display.display();
}
void drawWifiScan() { drawWifiListGeneric("WiFi ", true); }
void drawWifiDetail() {
  if (wifiApCount == 0) return;
  wifiScrollIdx = min(wifiScrollIdx, wifiApCount - 1);
  WifiAp &a = wifiAps[wifiScrollIdx];
  int ss = 0;
  for (int i = 0; i < wifiApCount; i++) if (!strcmp(wifiAps[i].ssid, a.ssid)) ss++;
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("WiFi "); display.print(wifiScrollIdx + 1); display.print("/"); display.print(wifiApCount);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 14); display.print("SSID:");
  char s[20]; strncpy(s, a.ssid, 18); s[18] = 0; display.print(s);
  if (a.hidden) display.print(" (h)");
  display.setCursor(2, 24); display.print("BSSID:"); display.print(a.bssid);
  display.setCursor(2, 34); display.print("Ch:"); display.print(a.channel);
  display.setCursor(60, 34); display.print("RSSI:"); display.print(a.rssi);
  const char* ec[] = {"OPEN","WEP","WPA","WPA2","WPA/WPA2","WPA2-E","WPA3","WPA2/WPA3","?"};
  display.setCursor(2, 44); display.print("Enc:"); display.print(ec[a.enc < 9 ? a.enc : 8]);
  if (ss > 1) { display.setCursor(2, 54); display.print("APs:"); display.print(ss); }
  display.display();
}
int channelLoadEstimate(int centerChannel) {
  // Relative 2.4 GHz congestion estimate from nearby AP count and RSSI; not airtime utilization.
  const uint8_t overlapWeight[5] = {5, 4, 3, 2, 1};
  int load = 0;
  for (int i = 0; i < wifiApCount; i++) {
    int delta = abs(wifiAps[i].channel - centerChannel);
    if (delta > 4) continue;
    int signalWeight = constrain(map(wifiAps[i].rssi, -100, -40, 1, 5), 1, 5);
    load += overlapWeight[delta] * signalWeight;
  }
  return load;
}
int findBestChannel() {
  const int candidates[] = {1, 6, 11};
  int best = candidates[0];
  int bestLoad = channelLoadEstimate(best);
  for (int i = 1; i < 3; i++) {
    int load = channelLoadEstimate(candidates[i]);
    if (load < bestLoad) { bestLoad = load; best = candidates[i]; }
  }
  return best;
}
void drawWifiChannels() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Load est 2.4G");
  if (wifiApCount > 0) { int b = findBestChannel(); display.setCursor(SW - 42, 0); display.print("B:"); display.print(b); }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int per[14] = {0};
  for (int c = 1; c <= 13; c++) per[c] = channelLoadEstimate(c);
  int mx = 1;
  for (int c = 1; c <= 13; c++) if (per[c] > mx) mx = per[c];
  int bCh = (wifiApCount > 0) ? findBestChannel() : -1;
  for (int c = 1; c <= 13; c++) {
    int bx = 3 + (c - 1) * 9;
    int bh = per[c] ? max(3, per[c] * 30 / mx) : 1;
    int by = 42 - bh;
    if (per[c]) display.fillRect(bx, by, 7, bh, SSD1306_WHITE);
    else display.drawRect(bx, 41, 7, 1, SSD1306_WHITE);
    if (c == bCh) { display.fillRect(bx - 1, 44, 9, 8, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(bx + (c < 10 ? 1 : 0), 44); display.print(c);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (scanMode == SCAN_ON) { display.print("ch"); display.print(scanCh); display.print(" scan"); }
  else { display.print(wifiApCount); display.print(" APs est"); }
  display.display();
}
void drawAnalyzeHidden() {
  int hc = 0;
  for (int i = 0; i < wifiApCount; i++) if (wifiAps[i].hidden) hc++;
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print("Hidden "); display.print(hc); display.print("/"); display.print(wifiApCount);
  if (scanMode == SCAN_ON) { display.print(" ch"); display.print(scanCh); } else display.print(" done");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int sh = 0, y = 14;
  for (int i = 0; i < wifiApCount && sh < 4; i++) {
    if (!wifiAps[i].hidden) continue;
    display.setCursor(2, y); display.print(wifiAps[i].bssid);
    display.setCursor(105, y); display.print(wifiAps[i].rssi);
    y += 10; sh++;
  }
  if (hc == 0) { display.setCursor(2, 30); display.print(scanMode == SCAN_ON ? "Scanning..." : "No hidden"); }
  display.display();
}
void drawAnalyzeTwins() {
  bool used[MAX_APS] = {false};
  int g = 0, cy = 14;
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Twins");
  if (scanMode == SCAN_ON) { display.print(" ch"); display.print(scanCh); } else display.print(" done");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  for (int i = 0; i < wifiApCount && cy < 56; i++) {
    if (used[i]) continue;
    int c = 0;
    for (int j = i; j < wifiApCount; j++) if (!strcmp(wifiAps[i].ssid, wifiAps[j].ssid)) { used[j] = true; c++; }
    if (c > 1) {
      char s[18]; strncpy(s, wifiAps[i].ssid, 16); s[16] = 0;
      display.setCursor(2, cy); display.print(s);
      display.setCursor(100, cy); display.print(c); display.print("x");
      cy += 10; g++;
    }
  }
  if (g == 0) { display.setCursor(2, 30); display.print(scanMode == SCAN_ON ? "Scanning..." : "No twins"); }
  display.display();
}
void drawAnalyzeSignal() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Signal ");
  if (wifiApCount > 0) { wifiScrollIdx = constrain(wifiScrollIdx, 0, wifiApCount - 1); display.print(wifiScrollIdx + 1); display.print("/"); }
  display.print(wifiApCount);
  if (scanMode == SCAN_ON) { display.setCursor(72, 0); display.print("ch"); display.print(scanCh); }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (wifiApCount == 0) {
    display.setCursor(2, 26); display.print(scanMode == SCAN_ON ? "Collecting APs..." : "No networks");
  } else {
    WifiAp &ap = wifiAps[wifiScrollIdx];
    char name[20]; strncpy(name, ap.ssid, 18); name[18] = 0;
    display.setCursor(2, 13); display.print(name);
    display.setCursor(2, 23); display.print("RSSI "); display.print(ap.rssi); display.print(" dBm ch"); display.print(ap.channel);
    if (ap.rssiCount > 0) {
      int first = (ap.rssiCount == RSSI_HISTORY_POINTS) ? ap.rssiNext : 0;
      int rssiMin = 0, rssiMax = -127;
      for (int i = 0; i < ap.rssiCount; i++) {
        int v = ap.rssiHistory[(first + i) % RSSI_HISTORY_POINTS];
        if (i == 0 || v < rssiMin) rssiMin = v;
        if (v > rssiMax) rssiMax = v;
      }
      display.setCursor(2, 32); display.print("min "); display.print(rssiMin); display.print(" max "); display.print(rssiMax);
      const int gx = 4, gy = 41, gw = 120, gh = 10;
      display.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, SSD1306_WHITE);
      int px = -1, py = -1;
      for (int i = 0; i < ap.rssiCount; i++) {
        int v = ap.rssiHistory[(first + i) % RSSI_HISTORY_POINTS];
        int x = gx + (ap.rssiCount <= 1 ? gw / 2 : (i * (gw - 1)) / (ap.rssiCount - 1));
        int y = gy + gh - constrain(map(v, -100, -35, 0, gh), 0, gh);
        if (px >= 0) display.drawLine(px, py, x, y, SSD1306_WHITE);
        else display.drawPixel(x, y, SSD1306_WHITE);
        px = x; py = y;
      }
    } else { display.setCursor(2, 36); display.print("Waiting for samples"); }
  }
  display.setCursor(2, 56); display.print("M=CSV L/R=AP hM=out");
  display.display();
}
void printCsvField(const char* value) {
  Serial.print('"');
  for (const char* p = value; *p; p++) {
    if (*p == '"') Serial.print('"');
    if (*p == '\r') Serial.print(F("\\r"));
    else if (*p == '\n') Serial.print(F("\\n"));
    else Serial.print(*p);
  }
  Serial.print('"');
}
void exportWifiSurveyCsv() {
  Serial.println(F("ssid,bssid,channel,rssi_dbm,samples,rssi_01,rssi_02,rssi_03,rssi_04,rssi_05,rssi_06,rssi_07,rssi_08,rssi_09,rssi_10,rssi_11,rssi_12"));
  for (int i = 0; i < wifiApCount; i++) {
    WifiAp &ap = wifiAps[i];
    printCsvField(ap.ssid); Serial.print(','); printCsvField(ap.bssid);
    Serial.print(','); Serial.print(ap.channel);
    Serial.print(','); Serial.print(ap.rssi);
    Serial.print(','); Serial.print(ap.rssiCount);
    int first = (ap.rssiCount == RSSI_HISTORY_POINTS) ? ap.rssiNext : 0;
    for (int j = 0; j < RSSI_HISTORY_POINTS; j++) {
      Serial.print(',');
      if (j < ap.rssiCount) Serial.print((int)ap.rssiHistory[(first + j) % RSSI_HISTORY_POINTS]);
    }
    Serial.println();
  }
}

// ---- WiFi Map ----
#define MAX_MAP_PTS 50
struct MapPoint { uint32_t t; int16_t rssi; uint8_t ch; };
MapPoint mapPts[MAX_MAP_PTS];
int mapPtCount = 0, mapScrollIdx = 0, mapTargetIdx = 0;
String mapSsid = "";
void loadMap() {
  prefs.begin("wmap", true);
  mapPtCount = constrain(prefs.getInt("n", 0), 0, MAX_MAP_PTS);
  if (mapPtCount > 0) prefs.getBytes("pts", mapPts, mapPtCount * sizeof(MapPoint));
  mapSsid = prefs.getString("ssid", "");
  prefs.end();
}
void saveMap() {
  prefs.begin("wmap", false);
  prefs.putInt("n", mapPtCount);
  if (mapPtCount > 0) prefs.putBytes("pts", mapPts, mapPtCount * sizeof(MapPoint));
  prefs.putString("ssid", mapSsid);
  prefs.end();
}
void clearMap() { mapPtCount = 0; mapScrollIdx = 0; saveMap(); }
void drawWifiMapSelect() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Map target");
  if (scanMode == SCAN_ON) { display.print(" ch"); display.print(scanCh); } else display.print(" done");
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (wifiApCount == 0) { display.setCursor(2, 30); display.print("Scanning..."); display.display(); return; }
  mapTargetIdx = constrain(mapTargetIdx, 0, wifiApCount - 1);
  int st = listStart(mapTargetIdx, wifiApCount, 3);
  int vi = min(3, wifiApCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    char nm[22]; strncpy(nm, wifiAps[idx].ssid, 20); nm[20] = 0;
    if (idx == mapTargetIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(nm);
    display.setCursor(100, y); display.print(wifiAps[idx].rssi);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56); display.print("L/R M=hL out");
  display.display();
}
void drawWifiMap() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  char s[10]; strncpy(s, mapSsid.c_str(), 8); s[8] = 0;
  display.print("Map:"); display.print(s);
  display.setCursor(80, 0); display.print(mapPtCount); display.print("/"); display.print(MAX_MAP_PTS);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (mapPtCount == 0) {
    display.setCursor(2, 28); display.print("No points yet");
    display.setCursor(2, 44); display.print("M = log here");
    display.setCursor(2, 56); display.print("hM=back");
    display.display(); return;
  }
  mapScrollIdx = constrain(mapScrollIdx, 0, mapPtCount - 1);
  int st = listStart(mapScrollIdx, mapPtCount, 4);
  int vis = min(4, mapPtCount - st);
  for (int i = 0; i < vis; i++) {
    int idx = st + i, y = 12 + i * 10;
    bool sel = (idx == mapScrollIdx);
    if (sel) { display.fillRect(0, y - 1, SW, 9, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print("#"); display.print(idx + 1);
    display.setCursor(28, y); display.print(mapPts[idx].rssi); display.print("dBm");
    display.setCursor(70, y); display.print("ch"); display.print(mapPts[idx].ch);
    display.setCursor(100, y); display.print(mapPts[idx].t); display.print("s");
  }
  display.setTextColor(SSD1306_WHITE);
  if (st > 0) display.fillTriangle(SW - 6, 13, SW - 2, 13, SW - 4, 10, SSD1306_WHITE);
  if (st < mapPtCount - 4) display.fillTriangle(SW - 6, 52, SW - 2, 52, SW - 4, 49, SSD1306_WHITE);
  display.setCursor(2, 56); display.print("M=log hL=clr hM=out");
  display.display();
}
void logMapPoint() {
  if (mapPtCount >= MAX_MAP_PTS) return;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false, false, 300, wifiAps[mapTargetIdx].channel);
  int live = -127;
  for (int i = 0; i < n; i++) if (!strcmp(WiFi.BSSIDstr(i).c_str(), wifiAps[mapTargetIdx].bssid)) { live = WiFi.RSSI(i); break; }
  WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
  if (live == -127) return;
  MapPoint &p = mapPts[mapPtCount++];
  p.t = millis() / 1000;
  p.rssi = live;
  p.ch = wifiAps[mapTargetIdx].channel;
  saveMap();
  mapScrollIdx = mapPtCount - 1;
}

// =========================================================
// Ping / Speed — cooperative state machine
// =========================================================
enum NetTestState : uint8_t {
  NET_TEST_IDLE, NET_TEST_WIFI, NET_TEST_PING, NET_TEST_SPEED_CONNECT,
  NET_TEST_SPEED_HEADERS, NET_TEST_SPEED_BODY
};
NetTestState netTestState = NET_TEST_IDLE;
WiFiClient netTestClient;
int pendingTestType = 0;
String pendingSSID = "", pendingPass = "";
uint32_t netTestStartedMs = 0, netTestNextMs = 0, netTestLastDataMs = 0;
uint32_t netTestBodyBytes = 0, netTestBodyStartedMs = 0;
bool netTestIsSpeed = false;
char netTestStatusLine[32] = {0};
uint8_t netTestStatusPos = 0;
uint32_t netTestHeaderTail = 0;
uint8_t netTestHeaderWindow[4] = {0};

void drawTestPing();
void drawTestSpeed();
void startNetworkTest(bool speed) {
  netTestClient.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  netTestIsSpeed = speed;
  netTestStartedMs = millis();
  netTestNextMs = netTestStartedMs;
  netTestLastDataMs = netTestStartedMs;
  netTestBodyBytes = 0;
  netTestBodyStartedMs = 0;
  netTestStatusPos = 0;
  netTestHeaderTail = 0;
  memset(netTestStatusLine, 0, sizeof(netTestStatusLine));
  memset(netTestHeaderWindow, 0, sizeof(netTestHeaderWindow));
  if (speed) {
    speedKbps = 0;
    speedCode = 0;
    speedDone = false;
  } else {
    pingAttempts = 0;
    pingOk = 0;
    pingResult = 0;
    pingDone = false;
  }
  netTestState = NET_TEST_WIFI;
  if (pendingPass.length() > 0) WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());
  else WiFi.begin(pendingSSID.c_str());
  if (speed) drawTestSpeed(); else drawTestPing();
}
void stopNetworkTest() {
  netTestClient.stop();
  if (netTestState != NET_TEST_IDLE) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  netTestState = NET_TEST_IDLE;
}
void finishNetworkTest() {
  netTestClient.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  netTestState = NET_TEST_IDLE;
  if (netTestIsSpeed) {
    speedDone = true;
    if (currentScreen == SCR_TEST_SPEED) drawTestSpeed();
  } else {
    pingDone = true;
    if (currentScreen == SCR_TEST_PING) drawTestPing();
  }
}
void drawKeyboard() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print(inputLabel);
  display.print(" ");
  display.print(kbdCatNames[kbdCat]);
  display.print(" ");
  String v = inputTarget ? *inputTarget : String("");
  int showLen = 10;
  if ((int)v.length() > showLen) v = v.substring(v.length() - showLen);
  display.print(v);
  display.print("_");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  const char* chars = kbdLayouts[kbdCat];
  for (int i = 0; i < 40; i++) {
    int col = i % 10, row = i / 10;
    int x = col * 12 + 3;
    int y = 12 + row * 10;
    bool sel = (kbdIdx == i);
    if (sel) { display.fillRect(x - 2, y - 1, 11, 10, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(x, y); display.print(chars[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (inputTarget && inputTarget->length() > 0) display.print("hL=del hR=cat hM=ok");
  else display.print("hR=cat hM=esc");
  display.display();
}

void drawTestSavedList() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Saved "); display.print(credCount);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (credCount == 0) { display.setCursor(2, 28); display.print("Empty"); display.setCursor(2, 56); display.print("hM=back"); display.display(); return; }
  pwdListIdx = constrain(pwdListIdx, 0, credCount - 1);
  int st = listStart(pwdListIdx, credCount, 3);
  int vi = min(3, credCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    char nm[22]; strncpy(nm, creds[idx].ssid, 20); nm[20] = 0;
    if (idx == pwdListIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(nm);
  }
  display.setTextColor(SSD1306_WHITE);
  if (st > 0) display.fillTriangle(SW - 6, 12, SW - 2, 12, SW - 4, 9, SSD1306_WHITE);
  if (st < credCount - 3) display.fillTriangle(SW - 6, 54, SW - 2, 54, SW - 4, 51, SSD1306_WHITE);
  display.setCursor(2, 56); display.print("M=run hM=scan new");
  display.display();
}
void launchTest() {
  if (pendingTestType == 0) {
    currentScreen = SCR_TEST_PING;
    startNetworkTest(false);
  } else {
    currentScreen = SCR_TEST_SPEED;
    startNetworkTest(true);
  }
}
void startTestSetup(int type) {
  pendingTestType = type; testParentMenu = currentMenuIdx;
  pendingSSID = ""; pendingPass = ""; pwdListIdx = 0;
  if (credCount > 0) { currentScreen = SCR_TEST_SAVED_LIST; drawTestSavedList(); }
  else { currentScreen = SCR_TEST_SELECT_NET; startScan(); drawWifiListGeneric("Nets ", true); }
}
void drawTestPing() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Ping 8.8.8.8:53");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (!pingDone) {
    display.setCursor(2, 22);
    display.print(netTestState == NET_TEST_WIFI ? "Connecting WiFi..." : "Probe ");
    if (netTestState != NET_TEST_WIFI) { display.print(pingAttempts); display.print("/5"); }
    display.setCursor(2, 38); display.print("OK: "); display.print(pingOk);
    display.display(); return;
  }
  display.setCursor(2, 16); display.print("Attempts:"); display.print(pingAttempts);
  display.setCursor(2, 28); display.print("OK:"); display.print(pingOk);
  if (pingOk > 0) { display.setCursor(2, 40); display.print("Avg:"); display.print(pingResult / max(1UL, pingOk)); display.print(" ms"); }
  else { display.setCursor(2, 40); display.print("All failed"); }
  display.setCursor(2, 56); display.print("M=rerun hL=new hM=out");
  display.display();
}
void drawTestSpeed() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Speed Test (100KB)");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (!speedDone) {
    display.setCursor(2, 24);
    if (netTestState == NET_TEST_WIFI) display.print("Connecting WiFi...");
    else if (netTestState == NET_TEST_SPEED_CONNECT) display.print("Connecting server...");
    else { display.print("Received "); display.print(netTestBodyBytes / 1000); display.print(" KB"); }
    display.display(); return;
  }
  display.setCursor(2, 16); display.print("Speed:");
  display.setTextSize(2); display.setCursor(2, 28); display.print(speedKbps, 1); display.print(" kbps");
  display.setTextSize(1); display.setCursor(2, 48); display.print("HTTP: "); display.print(speedCode);
  display.setCursor(2, 56); display.print("M=rerun hL=new hM=out");
  display.display();
}
void tickNetworkTest() {
  if (netTestState == NET_TEST_IDLE) return;
  uint32_t now = millis();
  if (now - netTestStartedMs > 25000) {
    if (netTestIsSpeed && speedCode == 0) speedCode = -3;
    finishNetworkTest();
    return;
  }

  switch (netTestState) {
    case NET_TEST_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        addCred(pendingSSID.c_str(), pendingPass.c_str());
        netTestLastDataMs = now;
        if (netTestIsSpeed) netTestState = NET_TEST_SPEED_CONNECT;
        else netTestState = NET_TEST_PING;
        netTestNextMs = now;
      } else if (now - netTestStartedMs >= 12000) {
        if (netTestIsSpeed) speedCode = -3;
        finishNetworkTest();
        return;
      }
      break;

    case NET_TEST_PING:
      if (pingAttempts >= 5) { finishNetworkTest(); return; }
      if ((int32_t)(now - netTestNextMs) >= 0) {
        pingAttempts++;
        WiFiClient probe;
        uint32_t started = millis();
        // Bounded connect keeps the UI responsive even when DNS/network is down.
        if (probe.connect("8.8.8.8", 53, 250)) {
          pingResult += millis() - started;
          pingOk++;
          probe.stop();
        }
        netTestNextMs = millis() + 200;
      }
      break;

    case NET_TEST_SPEED_CONNECT:
      if ((int32_t)(now - netTestNextMs) >= 0) {
        if (!netTestClient.connect("httpbin.org", 80, 500)) {
          speedCode = -2;
          finishNetworkTest();
          return;
        }
        netTestClient.print(F("GET /bytes/100000 HTTP/1.0\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n"));
        netTestLastDataMs = millis();
        netTestState = NET_TEST_SPEED_HEADERS;
      }
      break;

    case NET_TEST_SPEED_HEADERS:
    case NET_TEST_SPEED_BODY: {
      int budget = 256; // Limit work per loop pass so buttons/display keep running.
      while (budget-- > 0 && netTestClient.available() > 0) {
        int value = netTestClient.read();
        if (value < 0) break;
        uint8_t ch = (uint8_t)value;
        netTestLastDataMs = millis();
        if (netTestState == NET_TEST_SPEED_HEADERS) {
          if (netTestStatusPos < sizeof(netTestStatusLine) - 1) {
            netTestStatusLine[netTestStatusPos++] = (char)ch;
          }
          if (ch == '\n' && speedCode == 0) {
            int code = 0;
            if (sscanf(netTestStatusLine, "HTTP/%*s %d", &code) == 1) speedCode = code;
          }
          netTestHeaderWindow[netTestHeaderTail % 4] = ch;
          netTestHeaderTail++;
          if (netTestHeaderTail >= 4 &&
              netTestHeaderWindow[(netTestHeaderTail - 4) % 4] == '\r' &&
              netTestHeaderWindow[(netTestHeaderTail - 3) % 4] == '\n' &&
              netTestHeaderWindow[(netTestHeaderTail - 2) % 4] == '\r' &&
              netTestHeaderWindow[(netTestHeaderTail - 1) % 4] == '\n') {
            netTestState = NET_TEST_SPEED_BODY;
            netTestBodyStartedMs = millis();
          }
        } else {
          netTestBodyBytes++;
          if (netTestBodyBytes >= 100000) break;
        }
      }
      if (netTestState == NET_TEST_SPEED_BODY && netTestBodyBytes >= 100000) {
        uint32_t elapsed = millis() - netTestBodyStartedMs;
        if (elapsed > 0 && speedCode >= 200 && speedCode < 300)
          speedKbps = (netTestBodyBytes * 8.0f) / (elapsed / 1000.0f) / 1000.0f;
        finishNetworkTest();
        return;
      }
      if ((!netTestClient.connected() && netTestClient.available() == 0) ||
          (now - netTestLastDataMs > 2500)) {
        if (netTestBodyBytes > 0 && netTestBodyStartedMs > 0) {
          uint32_t elapsed = millis() - netTestBodyStartedMs;
          if (elapsed > 0 && speedCode >= 200 && speedCode < 300)
            speedKbps = (netTestBodyBytes * 8.0f) / (elapsed / 1000.0f) / 1000.0f;
        }
        if (speedCode == 0) speedCode = -1;
        finishNetworkTest();
        return;
      }
      break;
    }
    default: break;
  }

  static uint32_t lastDrawMs = 0;
  if (now - lastDrawMs >= 400) {
    lastDrawMs = now;
    if (netTestIsSpeed) drawTestSpeed(); else drawTestPing();
  }
}

// =========================================================
// Passwords
// =========================================================
void drawPwdList() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Passwords "); display.print(credCount); display.print("/"); display.print(MAX_CREDS);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (credCount == 0) { display.setCursor(2, 28); display.print("Empty"); display.setCursor(2, 56); display.print("hR=add hM=out"); display.display(); return; }
  pwdListIdx = constrain(pwdListIdx, 0, credCount - 1);
  int st = listStart(pwdListIdx, credCount, 3);
  int vi = min(3, credCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    char nm[22]; strncpy(nm, creds[idx].ssid, 20); nm[20] = 0;
    if (idx == pwdListIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(nm);
  }
  display.setTextColor(SSD1306_WHITE);
  if (st > 0) display.fillTriangle(SW - 6, 12, SW - 2, 12, SW - 4, 9, SSD1306_WHITE);
  if (st < credCount - 3) display.fillTriangle(SW - 6, 54, SW - 2, 54, SW - 4, 51, SSD1306_WHITE);
  display.setCursor(2, 56); display.print("M=view hR=+ hL=? hM=out");
  display.display();
}
void drawPwdDetail() {
  if (credCount == 0) { currentScreen = SCR_PWD_LIST; drawPwdList(); return; }
  pwdListIdx = constrain(pwdListIdx, 0, credCount - 1);
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Pwd "); display.print(pwdListIdx + 1); display.print("/"); display.print(credCount);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 14); display.print("SSID:");
  char s[20]; strncpy(s, creds[pwdListIdx].ssid, 18); s[18] = 0; display.print(s);
  display.setCursor(2, 28); display.print("Pass:");
  display.setCursor(2, 40);
  char p[20]; strncpy(p, creds[pwdListIdx].pass, 18); p[18] = 0;
  display.print(strlen(p) == 0 ? "(open)" : p);
  display.setCursor(2, 56); display.print("M=out hL=del hR=QR");
  display.display();
}
void drawPwdDeleteConfirm() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Delete password?");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 16); display.print("SSID:");
  char s[22]; strncpy(s, creds[pwdDelIdx].ssid, 20); s[20] = 0;
  display.setCursor(2, 26); display.print(s);
  display.setCursor(2, 40); display.print("MID = cancel");
  display.setCursor(2, 52); display.print("hold MID = DELETE");
  display.display();
}
void drawPwdHelp() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Passwords help");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 14); display.print("L / R      - select");
  display.setCursor(2, 22); display.print("MID        - open");
  display.setCursor(2, 30); display.print("hold MID   - back");
  display.setCursor(2, 38); display.print("in detail:");
  display.setCursor(2, 46); display.print("  hL=del hR=QR");
  display.setCursor(2, 56); display.print("press any to exit");
  display.display();
}
String escWifiQR(const String& s) {
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
    out += c;
  }
  return out;
}
void drawPwdQR() {
  if (credCount == 0) { currentScreen = SCR_PWD_LIST; drawPwdList(); return; }
  pwdListIdx = constrain(pwdListIdx, 0, credCount - 1);
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("QR "); display.print(pwdListIdx + 1); display.print("/"); display.print(credCount);
  display.setCursor(SW - 26, 0); display.print("back");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  String ssid = creds[pwdListIdx].ssid;
  String pass = creds[pwdListIdx].pass;
  String qr = (pass.length() == 0)
      ? "WIFI:S:" + escWifiQR(ssid) + ";;"
      : "WIFI:S:" + escWifiQR(ssid) + ";P:" + escWifiQR(pass) + ";;";
  #ifdef HAVE_QR
    QRCode qrcode;
    uint8_t buf[512];
    int ver = 0;
    for (ver = 1; ver <= 8; ver++) {
      if (qrcode_initText(&qrcode, buf, ver, ECC_LOW, qr.c_str()) == 0) break;
    }
    int qsize = (ver > 8) ? 0 : qrcode.size;
    int scale = (ver > 8) ? 0 : min(2, max(1, 52 / qsize));
    if (ver > 8 || scale < 2) {
      display.setTextSize(1); display.setCursor(2, 14);
      display.print(ver > 8 ? "Too long for QR" : "QR too small");
      display.setTextSize(2); display.setCursor(2, 26);
      char s[22]; strncpy(s, ssid.c_str(), 20); s[20] = 0; display.print(s);
      display.setTextSize(1); display.setCursor(2, 44); display.print("Pass:");
      display.setCursor(30, 44);
      char p[22]; strncpy(p, pass.c_str(), 20); p[20] = 0;
      display.print(pass.length() == 0 ? "(open)" : p);
      display.display(); return;
    }
    int qx = (SW - qsize * scale) / 2;
    int qy = 12 + (52 - qsize * scale) / 2;
    display.fillRect(qx - 4, qy - 4, qsize * scale + 8, qsize * scale + 8, SSD1306_WHITE);
    for (int y = 0; y < qsize; y++)
      for (int x = 0; x < qsize; x++)
        if (qrcode_getModule(&qrcode, x, y))
          display.fillRect(qx + x * scale, qy + y * scale, scale, scale, SSD1306_BLACK);
  #else
    display.setTextSize(2);
    display.setCursor(2, 14);
    char s[22]; strncpy(s, ssid.c_str(), 20); s[20] = 0; display.print(s);
    display.setTextSize(1); display.setCursor(2, 34); display.print("Pass:");
    display.setTextSize(2); display.setCursor(2, 44);
    char p[22]; strncpy(p, pass.c_str(), 20); p[20] = 0;
    display.print(pass.length() == 0 ? "(open)" : p);
  #endif
  display.display();
}

// =========================================================
// IR экраны
// =========================================================
void drawIrProj() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Proj ");
  display.print(irProjIdx + 1); display.print("/"); display.print(irProjCount);
  if (macroRecording) { display.setCursor(60, 0); display.print("REC"); }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int st = listStart(irProjIdx, irProjCount, 3);
  int vi = min(3, irProjCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    if (idx == irProjIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(irProj[idx].name);
  }
  display.setTextColor(SSD1306_WHITE);
  if (st > 0) display.fillTriangle(SW - 6, 12, SW - 2, 12, SW - 4, 9, SSD1306_WHITE);
  if (st < irProjCount - 3) display.fillTriangle(SW - 6, 54, SW - 2, 54, SW - 4, 51, SSD1306_WHITE);
  display.setCursor(2, 56);
  display.print(macroRecording ? "L/R M=add hL=cancel" : "L/R M=keys hM=auto");
  display.display();
}
void drawIrTv() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("TV ");
  display.print(irTvIdx + 1); display.print("/"); display.print(irTvCount);
  if (macroRecording) { display.setCursor(60, 0); display.print("REC"); }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int st = listStart(irTvIdx, irTvCount, 3);
  int vi = min(3, irTvCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    if (idx == irTvIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(irTv[idx].name);
  }
  display.setTextColor(SSD1306_WHITE);
  if (st > 0) display.fillTriangle(SW - 6, 12, SW - 2, 12, SW - 4, 9, SSD1306_WHITE);
  if (st < irTvCount - 3) display.fillTriangle(SW - 6, 54, SW - 2, 54, SW - 4, 51, SSD1306_WHITE);
  display.setCursor(2, 56);
  display.print(macroRecording ? "L/R M=add hL=cancel" : "L/R M=keys hM=auto");
  display.display();
}
void drawIrKeys() {
  const IrDevice* dev = irKeyIsTv ? &irTv[irKeyDevice] : &irProj[irKeyDevice];
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print(dev->name);
  if (macroRecording) { display.setCursor(60, 0); display.print("REC"); }
  else { display.setCursor(SW - 34, 0); display.print(irKeyIdx + 1); display.print("/"); display.print(keyCount); }
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  for (int i = 0; i < 16; i++) {
    int col = i % 4, row = i / 4;
    int x = 2 + col * 31;
    int y = 12 + row * 11;
    bool sel = (i == irKeyIdx && i < keyCount);
    bool has = (i < keyCount) && (dev->keys[i] != 0);
    if (sel) display.fillRect(x, y, 31, 10, SSD1306_WHITE);
    else display.drawRect(x, y, 31, 10, SSD1306_WHITE);
    if (i < keyCount) {
      display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
      if (has) {
        int16_t x1, y1; uint16_t w, h;
        display.getTextBounds(keyNames[i], 0, 0, &x1, &y1, &w, &h);
        display.setCursor(x + (31 - w) / 2, y + 1);
        display.print(keyNames[i]);
      } else { display.setCursor(x + 9, y + 1); display.print("--"); }
    }
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (macroRecording) display.print("M=add step hL=cancel");
  else if (irFavoriteFeedback && millis() - irFavoriteFeedbackAt < 1500) {
    if (irFavoriteFeedback == 1) display.print("FAVORITE SAVED");
    else if (irFavoriteFeedback == 2) display.print("ALREADY SAVED");
    else if (irFavoriteFeedback == 4) display.print("NO IR CODE");
    else display.print("FAVORITES FULL");
  }
  else if (irSent && millis() - irSentAt < 800) display.print("SENT!");
  else display.print("M=send holdR=fav");
  display.display();
}
void drawIrAuto() {
  int total = irProjCount + irTvCount;
  if (irAutoIdx >= total) irAutoIdx = 0;
  const char* name; const char* cat;
  if (irAutoIdx < irProjCount) { name = irProj[irAutoIdx].name; cat = "Proj"; }
  else { name = irTv[irAutoIdx - irProjCount].name; cat = "TV"; }
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (irAutoRunning) { display.print("Auto "); display.print(irAutoIdx + 1); display.print("/"); display.print(total); }
  else display.print("IR Auto Detect");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 22); display.print("Trying ("); display.print(cat); display.print("):");
  display.setTextSize(2); display.setCursor(2, 34); display.print(name);
  display.setTextSize(1); display.setCursor(2, 56);
  display.print(irAutoRunning ? "MID=stop" : "MID=start  hM=out");
  display.display();
}
const IrDevice* getIrFavoriteDevice(const IrFavorite& f) {
  if (f.keyIdx >= keyCount || f.isTv > 1) return nullptr;
  if (f.isTv) return f.devIdx < irTvCount ? &irTv[f.devIdx] : nullptr;
  return f.devIdx < irProjCount ? &irProj[f.devIdx] : nullptr;
}
void drawIrFavorites() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("IR Favorites ");
  display.print(irFavoriteIdx + 1); display.print("/"); display.print(MAX_IR_FAVORITES);
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int start = listStart(irFavoriteIdx, MAX_IR_FAVORITES, 4);
  for (int row = 0; row < 4; row++) {
    int idx = start + row;
    int y = 13 + row * 10;
    bool selected = idx == irFavoriteIdx;
    if (selected) display.fillRect(0, y - 1, SW, 9, SSD1306_WHITE);
    display.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(2, y); display.print(idx + 1); display.print(": ");
    const IrDevice* dev = getIrFavoriteDevice(irFavorites[idx]);
    if (!dev) display.print("(empty)");
    else {
      char name[10]; strncpy(name, dev->name, 8); name[8] = 0;
      display.print(name); display.print(" "); display.print(keyNames[irFavorites[idx].keyIdx]);
    }
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56); display.print("M=send hL=del hM=out");
  display.display();
}
void stepIrAuto() {
  if (!irAutoRunning || millis() - irAutoLast < 1500) return;
  irAutoLast = millis();
  const IrDevice* dev = nullptr;
  if (irAutoIdx < irProjCount) dev = &irProj[irAutoIdx];
  else if (irAutoIdx < irProjCount + irTvCount) dev = &irTv[irAutoIdx - irProjCount];
  if (dev) sendIR(*dev, 0);
  drawIrAuto();
  if (++irAutoIdx >= irProjCount + irTvCount) { irAutoIdx = 0; irAutoRunning = false; drawIrAuto(); }
}

// =========================================================
// Macro
// =========================================================
void drawMacroSel() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Macros "); display.print(macroSelIdx + 1); display.print("/"); display.print(MAX_MACROS);
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  for (int i = 0; i < MAX_MACROS; i++) {
    int y = 14 + i * 8;
    bool sel = (i == macroSelIdx);
    if (sel) display.fillRect(0, y - 1, SW, 9, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(2, y);
    display.print("M"); display.print(i + 1); display.print(": ");
    if (macros[i].nSteps == 0) display.print("(empty)");
    else { display.print(macros[i].nSteps); display.print(" steps"); }
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (macroPlaying) display.print("M=stop PLAYING");
  else if (macros[macroSelIdx].nSteps == 0) display.print("M=edit hL=out");
  else display.print("M=play hM=edit hL=out");
  display.display();
}
void drawMacroEdit() {
  Macro &m = macros[macroSelIdx];
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print("Edit M"); display.print(macroSelIdx + 1);
  display.print(" ["); display.print(m.nSteps); display.print("/"); display.print(MAX_STEPS); display.print("]");
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int totalRows = m.nSteps + 3;
  if (macroEditIdx >= totalRows) macroEditIdx = totalRows - 1;
  if (macroEditIdx < 0) macroEditIdx = 0;
  int rows = 4;
  int start = listStart(macroEditIdx, totalRows, rows);
  for (int i = 0; i < rows && start + i < totalRows; i++) {
    int idx = start + i;
    int y = 12 + i * 10;
    bool sel = (idx == macroEditIdx);
    if (sel) display.fillRect(0, y - 1, SW, 9, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(2, y);
    if (idx < m.nSteps) {
      MacroStep &s = m.steps[idx];
      display.print(idx + 1); display.print(".");
      display.print(s.isTv ? "TV " : "PJ ");
      const IrDevice* dev = s.isTv ? &irTv[s.devIdx] : &irProj[s.devIdx];
      char nm[10]; strncpy(nm, dev->name, 8); nm[8] = 0;
      display.print(nm);
      display.print(" ");
      display.print(keyNames[s.keyIdx]);
    } else if (idx == m.nSteps) display.print("+ Add TV step");
    else if (idx == m.nSteps + 1) display.print("+ Add Proj step");
    else display.print("> PLAY ALL");
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (macroPlaying) display.print("M=stop hM=back");
  else if (macroEditIdx < m.nSteps) display.print("M=send hL=del hM=back");
  else display.print("M=ok hM=back");
  display.display();
}

// =========================================================
// Tools
// =========================================================
int rndMode = 0, rndValue = 0;
float battCalTarget = 0.0f;
const char* rndModes[] = { "Coin","d6","d20","1-100" };
void drawToolsRandom() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Random: "); display.print(rndModes[rndMode]);
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setTextSize(3); display.setCursor(40, 24);
  if (rndMode == 0) display.print(rndValue ? "T" : "H");
  else { char b[4]; snprintf(b, sizeof(b), "%d", rndValue); display.print(b); }
  display.setTextSize(1); display.setCursor(2, 56); display.print("L/R mode  M=roll");
  display.display();
}
int pwLen = 16;
String pwCurrent = "";
const int pwLens[] = { 8, 12, 16, 24, 32 };
const char PW_CHARS[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%^&*()-_=+";
const int PW_CHARS_LEN = sizeof(PW_CHARS) - 1;
void genPassword() {
  pwCurrent = "";
  uint8_t buf[32];
  esp_fill_random(buf, sizeof(buf));
  for (int i = 0; i < pwLen; i++) pwCurrent += PW_CHARS[buf[i % 32] % PW_CHARS_LEN];
}
void drawToolsPassword() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Gen Len:"); display.print(pwLen);
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  int y = 16;
  for (int i = 0; i < (int)pwCurrent.length(); i += 18) {
    String ch = pwCurrent.substring(i, i + 18);
    display.setCursor(2, y); display.print(ch);
    y += 10; if (y > 54) break;
  }
  display.setCursor(2, 56); display.print("L/R len M=gen");
  display.display();
}
void drawToolsBattery() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (battValid) { display.print("Batt "); display.print(battPct); display.print("% "); display.print(battVoltage, 2); display.print("V"); }
  else display.print("Batt USB");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (battGraphCount == 0) {
    display.setCursor(20, 30); display.print("No data yet");
    display.setCursor(2, 56);
    if (battValid && battPct <= LOW_BATTERY_WARN_PCT) display.print("LOW! hL=clear");
    else display.print("hL=clear");
    display.display(); return;
  }
  int gx = 4, gy = 14, gw = 120, gh = 32;
  display.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, SSD1306_WHITE);
  int px = -1, py = -1;
  for (int i = 0; i < battGraphCount; i++) {
    int v = battGraph[i];
    int x = gx + (i * gw) / (BG_POINTS - 1);
    if (v < 0) { display.drawLine(x, gy + gh - 1, x, gy + gh - 4, SSD1306_WHITE); px = -1; continue; }
    int y = gy + gh - constrain(map(v, 66, 84, 0, gh), 0, gh);
    if (px >= 0) display.drawLine(px, py, x, y, SSD1306_WHITE);
    else display.drawPixel(x, y, SSD1306_WHITE);
    px = x; py = y;
  }
  display.setCursor(2, 48); display.print(battGraphCount); display.print(" pts (5min)");
  display.setCursor(2, 56);
  if (battValid && battPct <= LOW_BATTERY_WARN_PCT) display.print("LOW! hL=clear");
  else display.print("hL=clear");
  display.display();
}
void drawToolsBattCal() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Battery Cal");
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (!battValid) {
    display.setCursor(20, 24); display.print("No battery");
    display.setCursor(2, 38); display.print("Connect battery");
    display.setCursor(2, 54); display.print("hM = back");
    display.display(); return;
  }
  display.setTextSize(2); display.setCursor(2, 13); display.print(battVoltage, 2); display.print("V");
  display.setTextSize(1);
  display.setCursor(2, 34); display.print("Real "); display.print(battCalTarget, 2); display.print("V");
  float d = battCalTarget - battVoltage;
  display.setCursor(78, 34); display.print("d"); if (d >= 0) display.print("+"); display.print(d, 2);
  display.setCursor(2, 44); display.print("CAL "); display.print(battCal, 3);
  display.setCursor(2, 54);
  display.print((fabs(d) < 0.02f) ? "OK! M=save" : "L/R.01 hL/R.1 M=ok");
  display.display();
}
void drawToolsClock() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Clock");
  drawBatteryIcon(SW - 14, 1);
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (ntpSynced) {
    display.setTextSize(3);
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", clockHour, clockMin);
    display.setCursor(34, 22); display.print(b);
    display.setTextSize(1); display.setCursor(2, 54); display.print("UTC+3  M=resync");
  } else {
    display.setCursor(20, 22);
    if (ntpState == NTP_CONNECTING) display.print("Connecting...");
    else if (ntpState == NTP_SYNCING) display.print("Syncing...");
    else if (credCount == 0) display.print("No WiFi creds");
    else display.print("Not synced");
    display.setCursor(2, 40); display.print("Cred "); display.print(ntpAttemptCred + 1); display.print("/"); display.print(credCount);
    display.setCursor(2, 52); display.print("M = retry sync");
  }
  display.display();
}

// CLASSIC ARCADE COLLECTION — optimized for the 128x64 OLED and three buttons.
const char* arcadeNames[16] = {"Snake","Tetris","Arkanoid","Pong","Space Inv","Asteroids","Pac Maze","Frogger","Doodle Jump","Flappy","Lunar Lander","Galaga","Breakout","Missile Cmd","Racing","Minesweeper"};
uint8_t arcadeId=0, arcadeLives=3, arcadeGrid[16][16], arcadeMines[8][8], arcadeOpened[8][8], arcadeFlags[8][8];
int32_t arcadeScore=0, arcadeBest=0;
bool arcadeOver=false, arcadeWon=false, arcadePaused=false;
const uint32_t ARCADE_FRAME_MS = 40; // Slow Pong to 25 simulation frames per second.
uint32_t arcadeLastTick=0, arcadeLastDraw=0, arcadeFrameAt=0, aShotUntil=0;
int aX=0,aY=0,aVX=0,aVY=0,aPlayerX=0,aPlayerY=0,aEnemyX=0,aEnemyY=0,aAux=0,aCursor=0;
float aBallX=0,aBallY=0,aBallVX=0,aBallVY=0;
int8_t aXs[32],aYs[32];
uint8_t aActive[32];
uint8_t aSnakeX[96],aSnakeY[96],aSnakeLen=0,aSnakeDir=1;
uint8_t aTetType=0,aTetRot=0;
uint8_t aMineCount=10,aMineCursor=27;
char aMaze[7][16];
void arcadeSpawnSnakeFood();

const uint16_t tetShapes[7][4] = {
 {0x0F00,0x2222,0x00F0,0x4444}, {0x6600,0x6600,0x6600,0x6600},
 {0x4E00,0x8C80,0x00E4,0x4C40}, {0x6C00,0x8C40,0x06C0,0x4620},
 {0x2E00,0x44C0,0x0E80,0xC440}, {0x8E00,0x6440,0x0E20,0x44C0},
 {0x8C40,0x6E00,0x4C40,0x0E60}
};
bool arcadeTetFits(int x,int y,int rot) {
  for(int cy=0;cy<4;cy++) for(int cx=0;cx<4;cx++) if(tetShapes[aTetType][rot]&(0x8000>>(cy*4+cx))) {
    int bx=x+cx,by=y+cy; if(bx<0||bx>=10||by>=14)return false; if(by>=0&&arcadeGrid[by][bx])return false;
  }
  return true;
}
void arcadeSaveBest(){if(arcadeScore<=arcadeBest)return;arcadeBest=arcadeScore;char k[5];snprintf(k,sizeof(k),"g%u",arcadeId);prefs.begin("classic",false);prefs.putInt(k,arcadeBest);prefs.end();}
void arcadeFinish(bool won=false){if(arcadeOver)return;arcadeOver=true;arcadeWon=won;arcadeSaveBest();}
void arcadeClearArrays(){memset(arcadeGrid,0,sizeof(arcadeGrid));memset(arcadeMines,0,sizeof(arcadeMines));memset(arcadeOpened,0,sizeof(arcadeOpened));memset(arcadeFlags,0,sizeof(arcadeFlags));memset(aActive,0,sizeof(aActive));}
void arcadeStart(uint8_t id){
  arcadeId=id%16;arcadeScore=0;arcadeLives=3;arcadeOver=false;arcadeWon=false;arcadePaused=false;arcadeLastTick=millis();arcadeFrameAt=millis();arcadeLastDraw=0;
  aX=0;aY=0;aVX=0;aVY=0;aAux=0;aCursor=0;aShotUntil=0;aPlayerX=56;aPlayerY=0;aEnemyX=0;aEnemyY=0;aBallX=64;aBallY=48;aBallVX=1.5f;aBallVY=-1.7f;aTetType=random(7);aTetRot=0;aMineCount=10;aMineCursor=27;aSnakeLen=3;aSnakeDir=1;arcadeClearArrays();
  char k[5];snprintf(k,sizeof(k),"g%u",arcadeId);prefs.begin("classic",true);arcadeBest=prefs.getInt(k,0);prefs.end();
  if(arcadeId==0){arcadeLives=1;aSnakeX[0]=8;aSnakeY[0]=3;aSnakeX[1]=7;aSnakeY[1]=3;aSnakeX[2]=6;aSnakeY[2]=3;arcadeSpawnSnakeFood();}
  if(arcadeId==1){arcadeLives=1;aTetType=random(7);aPlayerX=3;aPlayerY=0;}
  if(arcadeId==2||arcadeId==12){int cols=arcadeId==2?10:8,rows=arcadeId==2?4:3;for(int y=0;y<rows;y++)for(int x=0;x<cols;x++)arcadeGrid[y][x]=(arcadeId==2&&y==0)?2:1;aPlayerX=52;aAux=0;aBallX=64;aBallY=52;aBallVX=1.3f;aBallVY=-1.7f;}
  if(arcadeId==3){arcadeLives=1;aPlayerY=28;aEnemyY=28;aAux=0;}
  if(arcadeId==4){arcadeLives=1;for(int y=0;y<3;y++)for(int x=0;x<6;x++)arcadeGrid[y][x]=1;aX=18;aY=14;aVX=1;aPlayerX=56;}
  if(arcadeId==5){aPlayerX=60;for(int i=0;i<7;i++){aXs[i]=random(5,120);aYs[i]=random(12,56);aActive[i]=1;}}
  if(arcadeId==6){const char* rows[7]={"###############","#.............#","#.###.###.###.#","#.............#","#.###.#.#.###.#","#.............#","###############"};for(int y=0;y<7;y++)strncpy(aMaze[y],rows[y],15);aMaze[3][1]=' ';aPlayerX=1;aPlayerY=1;aEnemyX=13;aEnemyY=5;}
  if(arcadeId==7){aPlayerX=3;aPlayerY=5;for(int y=0;y<5;y++){aXs[y]=random(8);aVX=(y%2)?-1:1;arcadeGrid[y][0]=random(8);}}
  if(arcadeId==8){arcadeLives=1;aPlayerX=60;aPlayerY=42;aVY=-4;for(int i=0;i<7;i++){aXs[i]=random(8,110);aYs[i]=54-i*8;aActive[i]=1;}}
  if(arcadeId==9){aPlayerY=30;aVY=0;aEnemyX=97;aEnemyY=random(15,37);}
  if(arcadeId==10){aPlayerX=58;aPlayerY=20;aVY=0;aVX=0;arcadeLives=80;}
  if(arcadeId==11){arcadeLives=1;for(int i=0;i<6;i++){aXs[i]=18+i*17;aYs[i]=17+(i%2)*8;aActive[i]=1;}aPlayerX=56;}
  if(arcadeId==13){for(int i=0;i<3;i++)aActive[i]=1;for(int i=0;i<8;i++){aXs[i]=random(8,120);aYs[i]=random(-30,20);aActive[i]=1;}aCursor=64;aAux=0;}
  if(arcadeId==14){aPlayerX=3;for(int i=0;i<4;i++){aXs[i]=random(1,6);aYs[i]=-i*12;aActive[i]=1;}}
  // Mines are placed on the first reveal so the opening cell is always safe.
}
void arcadeDrawHeader(){display.setTextSize(1);display.setTextColor(SSD1306_WHITE);display.fillRect(0,0,SW,10,SSD1306_WHITE);display.setTextColor(SSD1306_BLACK);display.setCursor(2,1);display.print(arcadeNames[arcadeId]);display.setCursor(72,1);display.print("S:");display.print(arcadeScore);display.setCursor(108,1);if(arcadeId==10){display.print("F");display.print(arcadeLives);}else if(arcadeId==15){int flags=0;for(int y=0;y<8;y++)for(int x=0;x<8;x++)flags+=arcadeFlags[y][x];display.print("M");display.print(10-flags);}else{display.print("L");display.print(arcadeLives);}display.setTextColor(SSD1306_WHITE);display.drawLine(0,10,SW-1,10,SSD1306_WHITE);}
void arcadeDrawSnake(){display.drawRect(5,12,114,43,SSD1306_WHITE);for(int x=12;x<116;x+=14)display.drawPixel(x,14,SSD1306_WHITE);for(int i=aSnakeLen-1;i>=0;i--){int px=7+aSnakeX[i]*7,py=13+aSnakeY[i]*6;if(i==0){display.fillRoundRect(px-1,py-1,7,7,2,SSD1306_WHITE);display.drawPixel(px+1,py+1,SSD1306_BLACK);display.drawPixel(px+4,py+1,SSD1306_BLACK);}else{display.fillRoundRect(px,py,5,5,1,SSD1306_WHITE);if(i%3==0)display.drawPixel(px+2,py+2,SSD1306_BLACK);}}display.drawCircle(10+aX*7,16+aY*6,4,SSD1306_WHITE);display.fillCircle(10+aX*7,16+aY*6,2,SSD1306_WHITE);display.drawPixel(10+aX*7-1,16+aY*6-1,SSD1306_BLACK);display.setCursor(3,58);display.print("L/R turn   M pause");}
void arcadeDrawTetris(){display.fillRect(7,12,52,44,SSD1306_BLACK);for(int x=0;x<=10;x++)display.drawFastVLine(8+x*5,13,42,SSD1306_BLACK);for(int y=0;y<=14;y++)display.drawFastHLine(8,13+y*3,50,SSD1306_BLACK);for(int y=0;y<14;y++)for(int x=0;x<10;x++)if(arcadeGrid[y][x]){int px=8+x*5,py=13+y*3;display.fillRect(px,py,4,2,SSD1306_WHITE);display.drawPixel(px,py,SSD1306_BLACK);}if(!arcadeOver)for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(tetShapes[aTetType][aTetRot]&(0x8000>>(y*4+x))){int px=8+(aPlayerX+x)*5,py=13+(aPlayerY+y)*3;display.fillRect(px,py,4,2,SSD1306_WHITE);display.drawPixel(px,py,SSD1306_BLACK);}display.drawRect(7,12,52,44,SSD1306_WHITE);display.drawLine(64,12,64,55,SSD1306_WHITE);display.setCursor(70,16);display.print("L/R move");display.setCursor(70,27);display.print("M rotate");display.setCursor(70,38);display.print("hL pause");display.setCursor(70,49);display.print("hR drop");}
void arcadeDrawBricks(){int cols=arcadeId==2?10:8,cell=arcadeId==2?11:14,rows=arcadeId==2?4:3;for(int y=0;y<rows;y++)for(int x=0;x<cols;x++)if(arcadeGrid[y][x]){int px=3+x*cell,py=13+y*6;display.fillRoundRect(px,py,cell-2,5,1,SSD1306_WHITE);display.drawFastHLine(px+1,py+1,cell-4,SSD1306_BLACK);if(arcadeGrid[y][x]>1){display.drawPixel(px+2,py+3,SSD1306_BLACK);display.drawPixel(px+cell-5,py+3,SSD1306_BLACK);}}display.drawFastHLine(2,43,124,SSD1306_WHITE);display.fillRoundRect(aPlayerX,55,24,5,2,SSD1306_WHITE);display.drawFastHLine(aPlayerX+4,56,16,SSD1306_BLACK);if(aAux){display.drawCircle((int)aBallX,(int)aBallY,4,SSD1306_WHITE);display.fillCircle((int)aBallX,(int)aBallY,2,SSD1306_WHITE);}else display.drawCircle((int)aBallX,(int)aBallY,3,SSD1306_WHITE);display.setCursor(3,46);display.print("L/R move   M launch");}
void arcadeDrawPong(){display.drawRoundRect(2,12,124,44,5,SSD1306_WHITE);for(int y=16;y<52;y+=7)display.drawFastVLine(64,y,4,SSD1306_WHITE);display.drawLine(64,12,64,55,SSD1306_WHITE);display.fillRoundRect(6,aPlayerY,4,13,2,SSD1306_WHITE);display.drawLine(7,aPlayerY+3,7,aPlayerY+9,SSD1306_BLACK);display.fillRoundRect(118,aEnemyY,4,13,2,SSD1306_WHITE);display.drawLine(119,aEnemyY+3,119,aEnemyY+9,SSD1306_BLACK);display.fillCircle((int)aBallX,(int)aBallY,3,SSD1306_WHITE);display.drawPixel((int)aBallX-1,(int)aBallY-1,SSD1306_BLACK);display.fillRoundRect(43,13,38,10,2,SSD1306_BLACK);display.setCursor(49,14);display.print(arcadeScore);display.print(" : ");display.print(aEnemyX);display.setCursor(2,58);display.print("L/R move     M serve");}
void arcadeDrawInvaders(){for(int sx=5;sx<124;sx+=29)display.drawPixel(sx,13+(sx%3)*3,SSD1306_WHITE);for(int y=0;y<3;y++)for(int x=0;x<6;x++)if(arcadeGrid[y][x]){int px=8+(int)aX+x*17,py=14+(int)aY+y*9;display.fillRect(px+2,py,6,2,SSD1306_WHITE);display.fillRect(px,py+2,10,3,SSD1306_WHITE);display.drawPixel(px+2,py+5,SSD1306_WHITE);display.drawPixel(px+7,py+5,SSD1306_WHITE);display.drawPixel(px+3,py+2,SSD1306_BLACK);display.drawPixel(px+7,py+2,SSD1306_BLACK);}display.drawFastHLine(2,53,124,SSD1306_WHITE);display.fillTriangle(aPlayerX,55,aPlayerX-6,63,aPlayerX+6,63,SSD1306_WHITE);display.drawFastHLine(aPlayerX-2,61,5,SSD1306_BLACK);if(aAux>0){display.drawLine(aCursor,aEnemyY,aCursor,aEnemyY+7,SSD1306_WHITE);display.drawPixel(aCursor,aEnemyY+8,SSD1306_WHITE);}display.setCursor(23,56);display.print("L/R move  M fire");}
void arcadeDrawAsteroids(){for(int sx=8;sx<125;sx+=23)display.drawPixel(sx,14+(sx*7%34),SSD1306_WHITE);for(int i=0;i<7;i++)if(aActive[i]){int r=4+(i%2),x=aXs[i],y=aYs[i];display.drawCircle(x,y,r,SSD1306_WHITE);display.drawLine(x-r+1,y-1,x-1,y-r+1,SSD1306_WHITE);display.drawLine(x+1,y+r-1,x+r-1,y+1,SSD1306_WHITE);display.drawPixel(x-1,y-1,SSD1306_WHITE);display.drawPixel(x+2,y+1,SSD1306_WHITE);}display.fillTriangle(aPlayerX,52,aPlayerX-6,63,aPlayerX+6,63,SSD1306_WHITE);display.drawLine(aPlayerX-2,59,aPlayerX+2,59,SSD1306_BLACK);if(aAux>0){display.drawLine(aCursor,aEnemyY-7,aCursor,aEnemyY,SSD1306_WHITE);display.drawPixel(aCursor,aEnemyY-8,SSD1306_WHITE);}display.setCursor(18,56);display.print("L/R move M fire");}
void arcadeDrawMaze(){for(int y=0;y<7;y++)for(int x=0;x<15;x++){int px=4+x*8,py=12+y*6;if(aMaze[y][x]=='#'){display.fillRoundRect(px,py,8,6,2,SSD1306_WHITE);display.drawPixel(px+1,py+1,SSD1306_BLACK);display.drawPixel(px+6,py+4,SSD1306_BLACK);}else if(aMaze[y][x]=='.'){display.drawPixel(px+4,py+3,SSD1306_WHITE);display.drawPixel(px+3,py+3,SSD1306_WHITE);}}int px=8+aPlayerX*8,py=15+aPlayerY*6;display.fillCircle(px,py,3,SSD1306_WHITE);display.fillTriangle(px,py,px+4,py-2,px+4,py+2,SSD1306_BLACK);display.drawPixel(px+1,py-1,SSD1306_WHITE);display.fillCircle(8+aEnemyX*8,15+aEnemyY*6,3,SSD1306_WHITE);display.fillRect(8+aEnemyX*8-2,15+aEnemyY*6,5,2,SSD1306_BLACK);display.drawPixel(8+aEnemyX*8-1,15+aEnemyY*6,SSD1306_WHITE);display.drawPixel(8+aEnemyX*8+1,15+aEnemyY*6,SSD1306_WHITE);display.setCursor(1,57);display.print("tap L/R=X hold L/R=Y");}
void arcadeDrawFrogger(){for(int lane=0;lane<6;lane++){int y=13+lane*7;if(lane==0||lane==5){display.fillRect(3,y,122,7,SSD1306_WHITE);for(int x=5;x<124;x+=12)display.drawPixel(x,y+3,SSD1306_BLACK);}else{display.fillRect(3,y,122,7,SSD1306_BLACK);display.drawFastHLine(3,y,122,SSD1306_WHITE);display.drawFastHLine(3,y+6,122,SSD1306_WHITE);for(int x=0;x<8;x++)if(arcadeGrid[lane-1][0]&(1<<x)){int tx=5+x*16;display.fillRoundRect(tx,y+1,12,5,1,SSD1306_WHITE);display.drawFastVLine(tx+3,y+2,3,SSD1306_BLACK);display.drawFastVLine(tx+8,y+2,3,SSD1306_BLACK);}}}int fx=7+aPlayerX*16,fy=14+aPlayerY*7;display.fillRoundRect(fx,fy,10,6,2,SSD1306_WHITE);display.drawPixel(fx+2,fy+1,SSD1306_BLACK);display.drawPixel(fx+7,fy+1,SSD1306_BLACK);display.drawPixel(fx+2,fy+5,SSD1306_BLACK);display.drawPixel(fx+7,fy+5,SSD1306_BLACK);display.setCursor(4,57);display.print("L/R side hL=up hR=dn");}
void arcadeDrawDoodle(){for(int i=0;i<7;i++){display.drawFastHLine(aXs[i],aYs[i],16,SSD1306_WHITE);display.drawPixel(aXs[i]+3,aYs[i]-1,SSD1306_WHITE);display.drawPixel(aXs[i]+12,aYs[i]-1,SSD1306_WHITE);}display.drawFastVLine(2,13,41,SSD1306_WHITE);display.drawFastVLine(125,13,41,SSD1306_WHITE);display.fillRoundRect(aPlayerX,aPlayerY,9,9,3,SSD1306_WHITE);display.drawPixel(aPlayerX+2,aPlayerY+3,SSD1306_BLACK);display.drawPixel(aPlayerX+6,aPlayerY+3,SSD1306_BLACK);display.drawFastHLine(aPlayerX+3,aPlayerY+6,3,SSD1306_BLACK);display.setCursor(39,57);display.print("L/R move");}
void arcadeDrawFlappy(){display.drawFastHLine(0,54,128,SSD1306_WHITE);display.drawFastHLine(0,57,128,SSD1306_WHITE);display.drawPixel(5,55,SSD1306_WHITE);display.drawPixel(15,56,SSD1306_WHITE);display.drawPixel(25,55,SSD1306_WHITE);display.fillRoundRect(aEnemyX,11,10,aEnemyY-11,2,SSD1306_WHITE);display.fillRoundRect(aEnemyX,aEnemyY+17,10,54-aEnemyY,2,SSD1306_WHITE);display.drawFastHLine(aEnemyX-2,aEnemyY-2,14,SSD1306_WHITE);display.drawFastHLine(aEnemyX-2,aEnemyY+17,14,SSD1306_WHITE);display.fillCircle(28,aPlayerY,4,SSD1306_WHITE);display.drawPixel(30,aPlayerY-1,SSD1306_BLACK);display.drawPixel(31,aPlayerY-1,SSD1306_WHITE);display.fillTriangle(31,aPlayerY,36,aPlayerY+1,31,aPlayerY+2,SSD1306_WHITE);display.drawLine(25,aPlayerY+3,22,aPlayerY+5,SSD1306_WHITE);display.setCursor(19,57);display.print("L/R flap");}
void arcadeDrawLander(){for(int sx=5;sx<128;sx+=13)display.drawPixel(sx,18+(sx*5%25),SSD1306_WHITE);display.drawFastHLine(0,57,128,SSD1306_WHITE);display.drawFastHLine(48,55,32,SSD1306_WHITE);display.drawFastVLine(48,55,5,SSD1306_WHITE);display.drawFastVLine(79,55,5,SSD1306_WHITE);display.drawLine(0,54,48,54,SSD1306_WHITE);display.drawLine(80,54,127,54,SSD1306_WHITE);display.drawLine(aPlayerX-5,aPlayerY+7,aPlayerX-3,aPlayerY+10,SSD1306_WHITE);display.drawLine(aPlayerX+5,aPlayerY+7,aPlayerX+3,aPlayerY+10,SSD1306_WHITE);display.fillRoundRect(aPlayerX-4,aPlayerY,8,7,2,SSD1306_WHITE);display.drawPixel(aPlayerX-1,aPlayerY+2,SSD1306_BLACK);display.drawPixel(aPlayerX+1,aPlayerY+2,SSD1306_BLACK);display.drawLine(aPlayerX-2,aPlayerY+6,aPlayerX-4,aPlayerY+8,SSD1306_WHITE);display.drawLine(aPlayerX+2,aPlayerY+6,aPlayerX+4,aPlayerY+8,SSD1306_WHITE);if(aAux>0){display.drawTriangle(aPlayerX-2,aPlayerY+8,aPlayerX+2,aPlayerY+8,aPlayerX,aPlayerY+13,SSD1306_WHITE);display.drawPixel(aPlayerX,aPlayerY+10,SSD1306_BLACK);}display.setCursor(9,13);display.print("L/R steer  M thrust");}
void arcadeDrawGalaga(){for(int sx=7;sx<126;sx+=24)display.drawPixel(sx,13+(sx*3%12),SSD1306_WHITE);for(int i=0;i<6;i++)if(aActive[i]){int x=aXs[i],y=aYs[i];display.fillTriangle(x+5,y,x,y+6,x+10,y+6,SSD1306_WHITE);display.drawRoundRect(x+2,y+1,6,5,2,SSD1306_WHITE);display.drawPixel(x+4,y+3,SSD1306_BLACK);display.drawPixel(x+6,y+3,SSD1306_BLACK);display.drawPixel(x,y+7,SSD1306_WHITE);display.drawPixel(x+9,y+7,SSD1306_WHITE);}display.fillTriangle(aPlayerX,53,aPlayerX-6,63,aPlayerX+6,63,SSD1306_WHITE);display.drawFastHLine(aPlayerX-2,59,5,SSD1306_BLACK);display.drawPixel(aPlayerX,52,SSD1306_WHITE);if(aAux>0){display.drawLine(aCursor,aEnemyY,aCursor,aEnemyY+7,SSD1306_WHITE);display.drawPixel(aCursor-1,aEnemyY+6,SSD1306_WHITE);display.drawPixel(aCursor+1,aEnemyY+6,SSD1306_WHITE);}display.setCursor(22,56);display.print("L/R move  M fire");}
void arcadeDrawMissile(){for(int sx=6;sx<128;sx+=18)display.drawPixel(sx,13+(sx*5%27),SSD1306_WHITE);for(int i=0;i<3;i++)if(aActive[16+i]){int cx=19+i*47;display.fillTriangle(cx,47,cx-7,55,cx+7,55,SSD1306_WHITE);display.drawFastHLine(cx-8,56,17,SSD1306_WHITE);display.drawFastVLine(cx-4,52,3,SSD1306_BLACK);display.drawFastVLine(cx+4,52,3,SSD1306_BLACK);}for(int i=0;i<8;i++)if(aActive[i]){display.drawLine(aXs[i]-2,aYs[i]-4,aXs[i]+1,aYs[i]+2,SSD1306_WHITE);display.drawLine(aXs[i]-1,aYs[i]-3,aXs[i],aYs[i],SSD1306_BLACK);display.drawPixel(aXs[i],aYs[i],SSD1306_WHITE);}display.drawLine(aCursor,13,aCursor,49,SSD1306_WHITE);display.drawPixel(aCursor-1,14,SSD1306_WHITE);display.drawPixel(aCursor+1,14,SSD1306_WHITE);if(aAux>0){display.drawCircle(aCursor,38,8,SSD1306_WHITE);display.drawCircle(aCursor,38,5,SSD1306_WHITE);display.drawPixel(aCursor,38,SSD1306_WHITE);}display.setCursor(30,57);display.print("L/R aim  M fire");}
void arcadeDrawRacing(){display.drawLine(19,11,19,56,SSD1306_WHITE);display.drawLine(109,11,109,56,SSD1306_WHITE);display.drawFastVLine(21,11,45,SSD1306_WHITE);display.drawFastVLine(107,11,45,SSD1306_WHITE);for(int y=12;y<56;y+=10){display.drawFastVLine(64,y,5,SSD1306_WHITE);display.drawPixel(5,y+3,SSD1306_WHITE);display.drawPixel(122,y+7,SSD1306_WHITE);}for(int i=0;i<4;i++)if(aActive[i]){int x=30+aXs[i]*12,y=aYs[i];display.fillRoundRect(x,y,10,9,2,SSD1306_WHITE);display.fillRect(x+2,y+1,6,2,SSD1306_BLACK);display.fillRect(x+1,y+6,2,2,SSD1306_BLACK);display.fillRect(x+7,y+6,2,2,SSD1306_BLACK);}int px=30+aPlayerX*12;display.fillRoundRect(px,47,10,10,2,SSD1306_WHITE);display.fillRect(px+2,48,6,3,SSD1306_BLACK);display.fillRect(px+1,54,2,2,SSD1306_BLACK);display.fillRect(px+7,54,2,2,SSD1306_BLACK);display.setCursor(17,58);display.print("L/R steer");}
void arcadeDrawMines(){for(int y=0;y<8;y++)for(int x=0;x<8;x++){int px=5+x*6,py=11+y*6;bool cur=(y*8+x)==aMineCursor;if(cur)display.fillRect(px,py,6,6,SSD1306_WHITE);else if(!arcadeOpened[y][x])display.drawRect(px,py,6,6,SSD1306_WHITE);if(arcadeOpened[y][x]|| (arcadeOver&&arcadeMines[y][x])){if(arcadeMines[y][x]){display.drawLine(px+1,py+1,px+4,py+4,cur?SSD1306_BLACK:SSD1306_WHITE);display.drawLine(px+4,py+1,px+1,py+4,cur?SSD1306_BLACK:SSD1306_WHITE);}else if(arcadeGrid[y][x]){display.setTextColor(cur?SSD1306_BLACK:SSD1306_WHITE);display.setCursor(px+1,py);display.print(arcadeGrid[y][x]);}}else if(arcadeFlags[y][x])display.fillCircle(px+3,py+3,2,cur?SSD1306_BLACK:SSD1306_WHITE);display.setTextColor(SSD1306_WHITE);}display.setCursor(58,14);display.print("L+R exit");int left=10;for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++)if(arcadeFlags[yy][xx])left--;if(left<0)left=0;display.setCursor(58,23);display.print("Mines ");display.print(left);display.setCursor(58,32);display.print("L/R = X");display.setCursor(58,41);display.print("hL^ hR v");display.setCursor(58,50);display.print("M = open");display.setCursor(58,57);display.print("hM = flag");}
void drawArcade(){display.clearDisplay();arcadeDrawHeader();if(arcadeOver){if(arcadeId==15){arcadeDrawMines();drawCenteredText(arcadeWon?"CLEARED!":"MINE!",29,0,1);display.display();return;}drawCenteredText(arcadeWon?"YOU WIN!":"GAME OVER",20,0,1);display.setCursor(27,34);display.print("Score ");display.print(arcadeScore);display.setCursor(27,45);display.print("Best  ");display.print(arcadeBest);display.setCursor(20,57);display.print("M=retry  hM=exit");display.display();return;}if(arcadePaused){drawCenteredText("PAUSED",25,0,2);display.setCursor(25,52);display.print("M=resume hM=exit");display.display();return;}switch(arcadeId){case 0:arcadeDrawSnake();break;case 1:arcadeDrawTetris();break;case 2:case 12:arcadeDrawBricks();break;case 3:arcadeDrawPong();break;case 4:arcadeDrawInvaders();break;case 5:arcadeDrawAsteroids();break;case 6:arcadeDrawMaze();break;case 7:arcadeDrawFrogger();break;case 8:arcadeDrawDoodle();break;case 9:arcadeDrawFlappy();break;case 10:arcadeDrawLander();break;case 11:arcadeDrawGalaga();break;case 13:arcadeDrawMissile();break;case 14:arcadeDrawRacing();break;case 15:arcadeDrawMines();break;}display.display();}
void arcadeRevealMine(int x,int y){if(x<0||x>=8||y<0||y>=8||arcadeOpened[y][x]||arcadeFlags[y][x])return;arcadeOpened[y][x]=1;if(arcadeMines[y][x]){arcadeFinish(false);return;}arcadeScore++;if(!arcadeGrid[y][x])for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)if(dx||dy)arcadeRevealMine(x+dx,y+dy);}
void arcadePlaceMines(int ex,int ey){int n=0;while(n<10){int x=random(8),y=random(8);if(arcadeMines[y][x]||(abs(x-ex)<=1&&abs(y-ey)<=1))continue;arcadeMines[y][x]=1;n++;}for(int y=0;y<8;y++)for(int x=0;x<8;x++){int c=0;for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)if(x+dx>=0&&x+dx<8&&y+dy>=0&&y+dy<8&&arcadeMines[y+dy][x+dx])c++;arcadeGrid[y][x]=c;}aAux=1;}
void arcadeSpawnSnakeFood(){bool found=false;while(!found){aX=random(16);aY=random(7);found=true;for(int i=0;i<aSnakeLen;i++)if(aSnakeX[i]==aX&&aSnakeY[i]==aY){found=false;break;}}}
void arcadeSnakeStep(){int x=aSnakeX[0],y=aSnakeY[0];if(aSnakeDir==0)y--;if(aSnakeDir==1)x++;if(aSnakeDir==2)y++;if(aSnakeDir==3)x--;if(x<0||x>=16||y<0||y>=7){arcadeFinish();return;}for(int i=0;i<aSnakeLen-1;i++)if(aSnakeX[i]==x&&aSnakeY[i]==y){arcadeFinish();return;}bool eat=x==aX&&y==aY;for(int i=aSnakeLen-1;i>0;i--){aSnakeX[i]=aSnakeX[i-1];aSnakeY[i]=aSnakeY[i-1];}aSnakeX[0]=x;aSnakeY[0]=y;if(eat){if(aSnakeLen<96)aSnakeLen++;arcadeScore++;arcadeSpawnSnakeFood();}}
void arcadeBrickStep(){aBallX+=aBallVX;aBallY+=aBallVY;if(aBallX<2||aBallX>125)aBallVX=-aBallVX;if(aBallY<12)aBallVY=fabs(aBallVY);if(aBallY>54&&aBallVY>0&&aBallX>=aPlayerX-2&&aBallX<=aPlayerX+24){aBallVY=-fabs(aBallVY);aBallVX+=(aBallX-(aPlayerX+11))*0.06f;if(aBallVX>2.8f)aBallVX=2.8f;if(aBallVX< -2.8f)aBallVX=-2.8f;}else if(aBallY>60){if(arcadeLives)arcadeLives--;if(!arcadeLives){arcadeFinish();return;}aBallX=aPlayerX+11;aBallY=52;aAux=0;return;}int cols=arcadeId==2?10:8,cell=arcadeId==2?11:14,rows=arcadeId==2?4:3;for(int r=0;r<rows;r++)for(int c=0;c<cols;c++)if(arcadeGrid[r][c]&&aBallY>=13+r*6&&aBallY<19+r*6&&aBallX>=3+c*cell&&aBallX<3+(c+1)*cell){if(--arcadeGrid[r][c]==0)arcadeScore+=10;aBallVY=-aBallVY;return;}int left=0;for(int r=0;r<rows;r++)for(int c=0;c<cols;c++)left+=arcadeGrid[r][c];if(!left)arcadeFinish(true);}
void arcadeTick(){uint32_t now=millis();if(arcadePaused||arcadeOver)return;if(arcadeId==3){if(now-arcadeFrameAt<ARCADE_FRAME_MS)return;arcadeFrameAt=now;}switch(arcadeId){case 0:if(now-arcadeLastTick>=(uint32_t)max(75,180-(int)arcadeScore)){arcadeLastTick=now;arcadeSnakeStep();}break;case 1:if(now-arcadeLastTick>500){arcadeLastTick=now;if(arcadeTetFits(aPlayerX,aPlayerY+1,aTetRot))aPlayerY++;else{for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(tetShapes[aTetType][aTetRot]&(0x8000>>(y*4+x))&&aPlayerY+y>=0)arcadeGrid[aPlayerY+y][aPlayerX+x]=1;for(int y=13;y>=0;y--){bool full=true;for(int x=0;x<10;x++)if(!arcadeGrid[y][x])full=false;if(full){arcadeScore+=100;for(int yy=y;yy>0;yy--)memcpy(arcadeGrid[yy],arcadeGrid[yy-1],10);memset(arcadeGrid[0],0,10);y++;}}aTetType=random(7);aTetRot=0;aPlayerX=3;aPlayerY=0;if(!arcadeTetFits(aPlayerX,aPlayerY,aTetRot))arcadeFinish();}}break;case 2:case 12:if(aAux)arcadeBrickStep();break;case 3:if(aAux){aBallX+=aBallVX;aBallY+=aBallVY;if(aBallY<12||aBallY>53)aBallVY=-aBallVY;if(aBallX>116&&aBallVX>0&&aBallY>=aEnemyY-2&&aBallY<=aEnemyY+15)aBallVX=-fabs(aBallVX);if(aBallX<10&&aBallVX<0&&aBallY>=aPlayerY-2&&aBallY<=aPlayerY+15)aBallVX=fabs(aBallVX);if(aBallX<1||aBallX>127){if(aBallX>127)arcadeScore++;else aEnemyX++;if(arcadeScore>=5||aEnemyX>=5){arcadeFinish(arcadeScore>=5);break;}aBallX=64;aBallY=32;aBallVX=1.5f;aBallVY=1.0f;aAux=0;}if(aEnemyY+6<aBallY)aEnemyY++;else if(aEnemyY+6>aBallY)aEnemyY--;}break;case 4:if(aAux>0){aEnemyY-=3;if(aEnemyY<11)aAux=0;for(int y=0;y<3;y++)for(int x=0;x<6;x++)if(arcadeGrid[y][x]&&aCursor>=8+x*17+aX&&aCursor<18+x*17+aX&&aEnemyY>=14+aY+y*9&&aEnemyY<20+aY+y*9){arcadeGrid[y][x]=0;aAux=0;arcadeScore+=10;}}if(now-arcadeLastTick>250){arcadeLastTick=now;aX+=aVX;if(aX<0||aX>25){aVX=-aVX;aY+=3;}bool any=false;for(int y=0;y<3;y++)for(int x=0;x<6;x++)any|=arcadeGrid[y][x];if(!any){arcadeScore+=50;for(int y=0;y<3;y++)for(int x=0;x<6;x++)arcadeGrid[y][x]=1;aY=14;}if(aY>40)arcadeFinish();}break;case 5:if(now-arcadeLastTick<45)break;arcadeLastTick=now;if(aAux>0){aEnemyY-=4;if((int32_t)(now-aShotUntil)>=0)aAux=0;}for(int i=0;i<7;i++)if(aActive[i]){aYs[i]++;if(aAux>0&&abs(aXs[i]-aCursor)<6&&abs(aYs[i]-aEnemyY)<5){aActive[i]=0;aAux=0;arcadeScore+=5;}if(aYs[i]>57){aYs[i]=12;aXs[i]=random(5,120);}if(abs(aXs[i]-aPlayerX)<7&&aYs[i]>48){aActive[i]=0;if(arcadeLives)arcadeLives--;if(!arcadeLives)arcadeFinish();}}break;case 6:if(now-arcadeLastTick>550){arcadeLastTick=now;if(random(2)){int nx=aEnemyX+(aEnemyX<aPlayerX?1:aEnemyX>aPlayerX?-1:0);if(nx>=0&&nx<15&&aMaze[aEnemyY][nx]!='#')aEnemyX=nx;}else{int ny=aEnemyY+(aEnemyY<aPlayerY?1:aEnemyY>aPlayerY?-1:0);if(ny>=0&&ny<7&&aMaze[ny][aEnemyX]!='#')aEnemyY=ny;}if(aEnemyX==aPlayerX&&aEnemyY==aPlayerY){if(arcadeLives)arcadeLives--;aPlayerX=1;aPlayerY=1;if(!arcadeLives)arcadeFinish();}}break;case 7:if(now-arcadeLastTick>400){arcadeLastTick=now;for(int r=0;r<5;r++){uint8_t m=arcadeGrid[r][0];m=(r%2)?(m>>1)|(m<<7):(m<<1)|(m>>7);if(random(4)==0)m^=1<<random(8);arcadeGrid[r][0]=m;}if(aPlayerY>0&&aPlayerY<5&&(arcadeGrid[aPlayerY-1][0]&(1<<aPlayerX))){if(arcadeLives)arcadeLives--;aPlayerX=3;aPlayerY=5;if(!arcadeLives)arcadeFinish();}}break;case 8:if(now-arcadeLastTick<45)break;arcadeLastTick=now;aPlayerY+=aVY;if(aVY<4)aVY++;if(aPlayerY>53){arcadeFinish();break;}if(aVY>0)for(int i=0;i<7;i++)if(aActive[i]&&aPlayerY+8>=aYs[i]&&aPlayerY+8<=aYs[i]+3&&aPlayerX+7>=aXs[i]&&aPlayerX<=aXs[i]+15){aVY=-6;arcadeScore++;}if(aPlayerY<20){int shift=20-aPlayerY;aPlayerY=20;for(int i=0;i<7;i++){aYs[i]+=shift;if(aYs[i]>58){aYs[i]=random(20,48);aXs[i]=random(8,110);}}arcadeScore++;}break;case 9:if(now-arcadeLastTick<45)break;arcadeLastTick=now;aVY+=1;if(aVY>3)aVY=3;aPlayerY+=aVY;aEnemyX-=2;if(aEnemyX< -9){aEnemyX=128;aEnemyY=random(14,34);arcadeScore++;}if(aPlayerY<12||aPlayerY>52||(aEnemyX<32&&aEnemyX>20&&(aPlayerY<aEnemyY||aPlayerY>aEnemyY+17))){if(arcadeLives)arcadeLives--;aPlayerY=30;aVY=0;aEnemyX=128;aEnemyY=random(14,34);if(!arcadeLives)arcadeFinish();}break;case 10:if(now-arcadeLastTick<100)break;arcadeLastTick=now;aVY++;if(aVY>2)aVY=2;aPlayerX+=aVX;aPlayerY+=aVY;if(aPlayerX<4)aPlayerX=4;if(aPlayerX>124)aPlayerX=124;if(aPlayerY>=55){if(aPlayerX>=50&&aPlayerX<=78&&abs(aVY)<=2)arcadeFinish(true);else arcadeFinish(false);}break;case 11:if(now-arcadeLastTick>80){arcadeLastTick=now;if(aAux>0){aEnemyY-=3;if(aEnemyY<10)aAux=0;}for(int i=0;i<6;i++)if(aActive[i]){aXs[i]+=((arcadeScore/5)%2)?-1:1;aYs[i]=17+(int)(5*sin((now/180.0)+i));if(aXs[i]<6||aXs[i]>112)aXs[i]=random(8,110);if(aAux>0&&abs(aXs[i]-aCursor)<7&&aEnemyY>=aYs[i]&&aEnemyY<aYs[i]+8){aActive[i]=0;aAux=0;arcadeScore+=10;}}bool any=false;for(int i=0;i<6;i++)any|=aActive[i];if(!any){for(int i=0;i<6;i++){aXs[i]=18+i*17;aYs[i]=17+(i%2)*8;aActive[i]=1;}arcadeScore+=30;}}break;case 13:if(now-arcadeLastTick<45)break;arcadeLastTick=now;for(int i=0;i<8;i++)if(aActive[i]){aYs[i]++;if(aYs[i]>54){aActive[i]=0;int city=constrain((int)aXs[i]/47,0,2);if(aActive[16+city]){aActive[16+city]=0;if(arcadeLives)arcadeLives--;if(!arcadeLives)arcadeFinish();}}if(aAux>0&&abs(aXs[i]-aCursor)<8&&aYs[i]>28&&aYs[i]<47){aActive[i]=0;arcadeScore+=5;}}if(aAux>0)aAux--;if(!aActive[16]&&!aActive[17]&&!aActive[18])arcadeFinish();break;case 14:if(now-arcadeLastTick>250){arcadeLastTick=now;for(int i=0;i<4;i++){aYs[i]+=2;if(aYs[i]>55){if(aXs[i]==aPlayerX){if(arcadeLives)arcadeLives--;if(!arcadeLives)arcadeFinish();}aYs[i]=random(-30,0);aXs[i]=random(1,6);arcadeScore++;}}}break;case 15:break;}}
void arcadeMoveMine(int dx,int dy){int x=aMineCursor%8,y=aMineCursor/8;x=(x+dx+8)%8;y=(y+dy+8)%8;aMineCursor=y*8+x;}
void arcadeExit(){btnLeft.fireOnPress=true;btnRight.fireOnPress=true;currentMenuIdx=7;currentScreen=SCR_MENU;drawCurrentMenu();}
void handleArcade(){bool l=takeShort(btnLeft),r=takeShort(btnRight),m=takeShort(btnSelect),hl=takeLong(btnLeft),hr=takeLong(btnRight),hm=takeLong(btnSelect);if(arcadeId==15&&digitalRead(BTN_L)==LOW&&digitalRead(BTN_R)==LOW){arcadeExit();return;}if(arcadeId!=15&&hm){arcadeExit();return;}if(arcadeOver){if(arcadeId==15&&hm){arcadeExit();return;}if(m){arcadeStart(arcadeId);drawArcade();}return;}if(arcadeId==15){if(l)arcadeMoveMine(-1,0);if(r)arcadeMoveMine(1,0);if(hl)arcadeMoveMine(0,-1);if(hr)arcadeMoveMine(0,1);if(m){if(!aAux){arcadePlaceMines(aMineCursor%8,aMineCursor/8);}if(!arcadeFlags[aMineCursor/8][aMineCursor%8])arcadeRevealMine(aMineCursor%8,aMineCursor/8);int safe=0;for(int y=0;y<8;y++)for(int x=0;x<8;x++)if(!arcadeMines[y][x]&&arcadeOpened[y][x])safe++;if(safe==54)arcadeFinish(true);}if(hm&&!arcadeOver){int x=aMineCursor%8,y=aMineCursor/8;if(!arcadeOpened[y][x]){if(arcadeFlags[y][x])arcadeFlags[y][x]=0;else{int flags=0;for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++)flags+=arcadeFlags[yy][xx];if(flags<10)arcadeFlags[y][x]=1;}}}drawArcade();return;}
  if(arcadeId==0){if(l)aSnakeDir=(aSnakeDir+3)%4;if(r)aSnakeDir=(aSnakeDir+1)%4;if(m)arcadePaused=!arcadePaused;}
  else if(arcadeId==1){if(l&&arcadeTetFits(aPlayerX-1,aPlayerY,aTetRot))aPlayerX--;if(r&&arcadeTetFits(aPlayerX+1,aPlayerY,aTetRot))aPlayerX++;if(m){uint8_t rot=(aTetRot+1)%4;if(arcadeTetFits(aPlayerX,aPlayerY,rot))aTetRot=rot;}if(hl)arcadePaused=!arcadePaused;if(hr){while(arcadeTetFits(aPlayerX,aPlayerY+1,aTetRot))aPlayerY++;arcadeLastTick=millis()-501;}}
  else if(arcadeId==2||arcadeId==12){if(l)aPlayerX-=5;if(r)aPlayerX+=5;aPlayerX=constrain(aPlayerX,2,SW-24);if(m&&!aAux){aAux=1;aBallX=aPlayerX+11;aBallY=53;}}
  else if(arcadeId==3){if(l)aPlayerY=max(12,aPlayerY-6);if(r)aPlayerY=min(43,aPlayerY+6);if(m&&!aAux){aAux=1;aBallX=64;aBallY=32;aBallVX=random(2)?2:-2;aBallVY=random(2)?1.5f:-1.5f;}}
  else if(arcadeId==4){if(l)aPlayerX=max(5,aPlayerX-5);if(r)aPlayerX=min(119,aPlayerX+5);if(m&&aAux==0){aAux=1;aCursor=aPlayerX;aEnemyY=53;}}
  else if(arcadeId==5){if(l)aPlayerX=max(5,aPlayerX-5);if(r)aPlayerX=min(123,aPlayerX+5);if(m&&aAux==0){aAux=1;aCursor=aPlayerX;aEnemyY=53;aShotUntil=millis()+350;}}
  else if(arcadeId==6){int nx=aPlayerX,ny=aPlayerY;if(l)nx--;if(r)nx++;if(hl)ny--;if(hr)ny++;if(nx>=0&&nx<15&&ny>=0&&ny<7&&aMaze[ny][nx]!='#'){aPlayerX=nx;aPlayerY=ny;if(aMaze[ny][nx]=='.'){aMaze[ny][nx]=' ';arcadeScore++;}if(aPlayerX==aEnemyX&&aPlayerY==aEnemyY)arcadeFinish(true);}}
  else if(arcadeId==7){if(l)aPlayerX=(aPlayerX+7)%8;if(r)aPlayerX=(aPlayerX+1)%8;if(hl&&aPlayerY>0)aPlayerY--;if(hr&&aPlayerY<5)aPlayerY++;if(aPlayerY==0){arcadeScore++;aPlayerY=5;aPlayerX=3;}}
  else if(arcadeId==8){if(l)aPlayerX=max(2,aPlayerX-6);if(r)aPlayerX=min(118,aPlayerX+6);}
  else if(arcadeId==9){if(l||r){aVY=-5;}}
  else if(arcadeId==10){if(l)aVX=max(-2,aVX-1);if(r)aVX=min(2,aVX+1);if(m&&arcadeLives>0){aVY=max(-3,aVY-2);arcadeLives--;aAux=1;}else aAux=0;}
  else if(arcadeId==11){if(l)aPlayerX=max(5,aPlayerX-5);if(r)aPlayerX=min(119,aPlayerX+5);if(m&&aAux==0){aAux=1;aCursor=aPlayerX;aEnemyY=53;}}
  else if(arcadeId==13){if(l)aCursor=max(8,aCursor-6);if(r)aCursor=min(120,aCursor+6);if(m&&aAux==0){aAux=8;}}
  else if(arcadeId==14){if(l)aPlayerX=max(0,aPlayerX-1);if(r)aPlayerX=min(5,aPlayerX+1);}
  arcadeTick();if(millis()-arcadeLastDraw>40){arcadeLastDraw=millis();drawArcade();}}


// =========================================================
// Sniffer
// =========================================================
volatile uint32_t sniffBeacon = 0, sniffProbe = 0, sniffData = 0, sniffMgmt = 0, sniffCtrl = 0, sniffDeauth = 0;
uint8_t deauthTop3[3][6] = {{0}};
volatile uint32_t deauthTop3Count[3] = {0, 0, 0};
void IRAM_ATTR snifferCB(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MGMT) {
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 1) return;
    uint8_t st = pkt->payload[0] >> 4;
    if (st == 8) sniffBeacon = sniffBeacon + 1;
    else if (st == 4) sniffProbe = sniffProbe + 1;
    else if (st == 12) {
      sniffDeauth = sniffDeauth + 1;
      if (pkt->rx_ctrl.sig_len < 16) return;
      const uint8_t* src = &pkt->payload[10];
      for (int i = 0; i < 3; i++) if (deauthTop3Count[i] == 0 || !memcmp(deauthTop3[i], src, 6)) { memcpy(deauthTop3[i], src, 6); deauthTop3Count[i] = deauthTop3Count[i] + 1; break; }
      int mi = 0;
      for (int i = 1; i < 3; i++) if (deauthTop3Count[i] < deauthTop3Count[mi]) mi = i;
      if (deauthTop3Count[mi] < 5) { memcpy(deauthTop3[mi], src, 6); deauthTop3Count[mi] = 1; }
    } else sniffMgmt = sniffMgmt + 1;
  } else if (type == WIFI_PKT_DATA) sniffData = sniffData + 1;
  else if (type == WIFI_PKT_CTRL) sniffCtrl = sniffCtrl + 1;
}
bool snifferDone = false;
uint32_t snifferStart = 0;
const uint32_t SNIFF_DURATION_MS = 15000;
void drawSniffer() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Detector");
  if (snifferRunning) { display.setCursor(80, 0); display.print((millis() - snifferStart) / 1000); display.print("s"); }
  else if (snifferDone) { display.setCursor(94, 0); display.print("done"); }
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 14); display.print("Beacon:"); display.print(sniffBeacon);
  display.setCursor(2, 22); display.print("Probe: "); display.print(sniffProbe);
  display.setCursor(2, 30); display.print("Data:  "); display.print(sniffData);
  display.setCursor(2, 38); display.print("Mgmt:  "); display.print(sniffMgmt);
  display.setCursor(2, 46); display.print("DEAUTH:");
  if (sniffDeauth > 0) { display.fillRect(52, 46, 72, 8, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
  display.print(sniffDeauth);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (deauthTop3Count[0] > 0) {
    char m[18]; snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X",
      deauthTop3[0][0], deauthTop3[0][1], deauthTop3[0][2], deauthTop3[0][3], deauthTop3[0][4], deauthTop3[0][5]);
    display.print(m); display.print(" "); display.print(deauthTop3Count[0]);
  } else display.print("no deauth");
  display.display();
}
void startSniffer() {
  haltScan();
  sniffBeacon = 0; sniffProbe = 0; sniffData = 0; sniffMgmt = 0; sniffCtrl = 0; sniffDeauth = 0;
  deauthTop3Count[0] = 0; deauthTop3Count[1] = 0; deauthTop3Count[2] = 0;
  memset(deauthTop3, 0, sizeof(deauthTop3));
  snifferRunning = true; snifferDone = false; snifferStart = millis();
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_promiscuous_rx_cb(&snifferCB); esp_wifi_set_promiscuous(true);
  drawSniffer();
}
void stopSniffer() {
  if (!snifferRunning) return;
  esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF);
  snifferRunning = false; snifferDone = true; drawSniffer();
}

// =========================================================
// Clone + Spam + Deauth
// =========================================================
int cloneTargetIdx = 0;
uint32_t cloneStart = 0;
const uint32_t CLONE_DURATION_MS = 60000;
void drawCloneSelect();
void drawCloneRun();
void startClone() {
  if (wifiApCount == 0) return;
  stopSpam(); haltScan();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(wifiAps[cloneTargetIdx].ssid, NULL, wifiAps[cloneTargetIdx].channel, 0, 4);
  cloneRunning = true; cloneStart = millis();
  currentScreen = SCR_ATTACK_CLONE_RUN; drawCloneRun();
}
void stopClone() { if (cloneRunning) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); cloneRunning = false; } }
void drawCloneSelect() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Clone");
  if (scanMode == SCAN_ON) { display.print(" ch"); display.print(scanCh); } else display.print(" done");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  if (wifiApCount == 0) { display.setCursor(2, 30); display.print("Scanning..."); display.display(); return; }
  cloneTargetIdx = constrain(cloneTargetIdx, 0, wifiApCount - 1);
  int st = listStart(cloneTargetIdx, wifiApCount, 3);
  int vi = min(3, wifiApCount - st);
  for (int i = 0; i < vi; i++) {
    int idx = st + i, y = 12 + i * 14;
    char nm[22]; strncpy(nm, wifiAps[idx].ssid, 20); nm[20] = 0;
    if (idx == cloneTargetIdx) { display.fillRect(0, y - 1, SW, 13, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.print(nm);
    display.setCursor(100, y); display.print(wifiAps[idx].rssi);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56); display.print("L/R sel, MID start");
  display.display();
}
void drawCloneRun() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Evil Twin RUN");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  char nm[18]; strncpy(nm, wifiAps[cloneTargetIdx].ssid, 16); nm[16] = 0;
  display.setCursor(2, 16); display.print("SSID:"); display.print(nm);
  display.setCursor(2, 28); display.print("Ch:"); display.print(wifiAps[cloneTargetIdx].channel);
  display.setCursor(2, 40); display.print("IP: "); display.print(WiFi.softAPIP().toString());
  display.setCursor(2, 56); display.print("Time: "); display.print((millis() - cloneStart) / 1000); display.print("s");
  display.display();
}
void stepClone() {
  if (!cloneRunning) return;
  if (millis() - cloneStart > CLONE_DURATION_MS) { stopClone(); currentScreen = SCR_ATTACK_CLONE_SEL; drawCloneSelect(); return; }
  static uint32_t lc = 0;
  if (millis() - lc > 500) { lc = millis(); drawCloneRun(); }
}
uint32_t spamCount = 0, spamStart = 0;
const uint32_t SPAM_DURATION_MS = 30000;
const char* spamNames[] = { "Free WiFi","Virus","Loading...","404","FBI","Tricorder","Hidden","SKYNET" };
const int spamNamesCount = 8;
uint8_t beaconFrame[128] = { 0x80,0,0,0, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0, 0,0,0,0,0,0,0,0, 0x64,0x00,0x31,0x04 };
void drawSpamRun() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("Beacon Spam");
  display.drawLine(0, 10, SW - 1, 10, SSD1306_WHITE);
  display.setCursor(2, 16); display.print("Sent: "); display.print(spamCount);
  display.setCursor(2, 28); display.print("Time: "); display.print((millis() - spamStart) / 1000); display.print("s");
  display.setCursor(2, 40); display.print("Ch: 1-13 random");
  display.setCursor(2, 56); display.print("MID stop");
  display.display();
}
void startSpam() {
  stopClone(); haltScan();
  spamRunning = true; spamCount = 0; spamStart = millis();
  WiFi.mode(WIFI_STA); esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  currentScreen = SCR_ATTACK_SPAM_RUN; drawSpamRun();
}
void stopSpam() { spamRunning = false; WiFi.mode(WIFI_OFF); }
void stepSpam() {
  if (!spamRunning) return;
  if (millis() - spamStart > SPAM_DURATION_MS) { stopSpam(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
  int idx = random(spamNamesCount); const char* ssid = spamNames[idx];
  int sl = strlen(ssid); if (sl > 32) sl = 32;
  for (int i = 10; i < 16; i++) beaconFrame[i] = random(256);
  for (int i = 16; i < 22; i++) beaconFrame[i] = random(256);
  beaconFrame[36] = 0; beaconFrame[37] = sl;
  memcpy(&beaconFrame[38], ssid, sl);
  esp_wifi_80211_tx(WIFI_IF_STA, beaconFrame, 38 + sl, false);
  spamCount++;
  static uint32_t ls = 0;
  if (millis() - ls > 200) { ls = millis(); drawSpamRun(); }
}

// Deauth — не в меню, но функции оставлены
int deauthTargetIdx = 0;
uint32_t deauthCount = 0, deauthStart = 0;
const uint32_t DEAUTH_DURATION_MS = 30000;
uint8_t deauthFrame[26] = { 0xC0,0x00,0x3A,0x01, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0, 0x07,0x00 };

// =========================================================
// Info / Space
// =========================================================
void drawInfo() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0); display.print("=== System Info ===");
  display.setCursor(2, 10);
  if (battValid) { display.print("Batt: "); display.print(battPct); display.print("% "); display.print(battVoltage, 2); display.print("V"); }
  else display.print("Batt: USB power");
  if (ntpSynced) { char tb[6]; snprintf(tb, sizeof(tb), "%02d:%02d", clockHour, clockMin); display.setCursor(90, 10); display.print(tb); }
  display.setCursor(2, 20); display.print("SoC: "); display.print(temperatureRead(), 1); display.print(" C");
  display.setCursor(2, 30); display.print("Heap: "); display.print(ESP.getFreeHeap() / 1024); display.print("/"); display.print(ESP.getHeapSize() / 1024);
  display.setCursor(2, 40); display.print("Up: ");
  uint32_t s = millis() / 1000, h = s / 3600; s %= 3600;
  uint32_t m = s / 60; s %= 60;
  char buf[16]; snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", h, m, s);
  display.print(buf);
  display.setCursor(2, 52);
  display.print("WiFi:"); display.print(WiFi.status() == WL_CONNECTED ? "on" : "off");
  display.print(" Saved:"); display.print(credCount);
  display.display();
}
struct Star { int16_t x, y, z; };
const int STAR_COUNT = 70;
Star stars[STAR_COUNT];
bool spacePaused = false;
void initStars() {
  for (int i = 0; i < STAR_COUNT; i++) { stars[i].x = random(-64, 64); stars[i].y = random(-32, 32); stars[i].z = random(2, 64); }
  spacePaused = false;
}
void drawStars() {
  display.clearDisplay();
  for (int i = 0; i < STAR_COUNT; i++) {
    int16_t sx = (int16_t)((int32_t)stars[i].x * 32 / stars[i].z) + 64;
    int16_t sy = (int16_t)((int32_t)stars[i].y * 32 / stars[i].z) + 32;
    if (sx >= 0 && sx < SW && sy >= 0 && sy < SH) {
      uint8_t sz = (stars[i].z < 8) ? 2 : 1;
      display.fillRect(sx, sy, sz, sz, SSD1306_WHITE);
    }
    if (!spacePaused) {
      if (stars[i].z <= 2) { stars[i].x = random(-64, 64); stars[i].y = random(-32, 32); stars[i].z = 64; }
      else stars[i].z -= 2;
    }
  }
  if (spacePaused) { display.setCursor(2, 0); display.print("PAUSE"); }
  display.display();
}

// =========================================================
// Menu logic
// =========================================================
void drawIrFavorites();
void enterSelected() {
  int idx = menus[currentMenuIdx].index;
  testParentMenu = currentMenuIdx;
  if (currentMenuIdx == 0) {
    if (idx == 0) { currentMenuIdx = 1; menus[1].index = 0; drawCurrentMenu(); }
    else if (idx == 1) { setIrNavigationOnRelease(false); irProjIdx = 0; currentScreen = SCR_IR_POWER; drawIrProj(); }
    else if (idx == 2) { setIrNavigationOnRelease(false); irTvIdx = 0; currentScreen = SCR_IR_TV; drawIrTv(); }
    else if (idx == 3) { currentMenuIdx = 5; menus[5].index = 0; drawCurrentMenu(); }
    else if (idx == 4) { currentMenuIdx = 6; menus[6].index = 0; drawCurrentMenu(); }
    else if (idx == 5) { currentMenuIdx = 7; menus[7].index = 0; drawCurrentMenu(); }
    else if (idx == 6) { currentMenuIdx = 8; menus[8].index = 0; drawCurrentMenu(); }
  }
  else if (currentMenuIdx == 1) {
    if (idx == 0) { currentScreen = SCR_WIFI_SCAN; startScan(); drawWifiScan(); }
    if (idx == 1) { currentScreen = SCR_WIFI_CHANNELS; startScan(); drawWifiChannels(); }
    if (idx == 2) { currentMenuIdx = 2; menus[2].index = 0; drawCurrentMenu(); }
    if (idx == 3) { currentMenuIdx = 3; menus[3].index = 0; drawCurrentMenu(); }
    if (idx == 4) { currentMenuIdx = 4; menus[4].index = 0; drawCurrentMenu(); }
    if (idx == 5) { mapTargetIdx = 0; loadMap(); currentScreen = SCR_WIFI_MAP_SEL; startScan(); drawWifiMapSelect(); }
  }
  else if (currentMenuIdx == 2) {
    if (idx == 0) { currentScreen = SCR_ANALYZE_HIDDEN; startScan(); drawAnalyzeHidden(); }
    if (idx == 1) { currentScreen = SCR_ANALYZE_TWINS; startScan(); drawAnalyzeTwins(); }
    if (idx == 2) { wifiScrollIdx = 0; currentScreen = SCR_ANALYZE_SIGNAL; startScan(); drawAnalyzeSignal(); }
  }
  else if (currentMenuIdx == 3) {
    if (idx == 0) startTestSetup(0);
    if (idx == 1) startTestSetup(1);
  }
  else if (currentMenuIdx == 4) {
    if (idx == 0) { currentScreen = SCR_ATTACK_CLONE_SEL; startScan(); drawCloneSelect(); }
    if (idx == 1) { currentScreen = SCR_ATTACK_SPAM_RUN; startSpam(); }
  }
  else if (currentMenuIdx == 5) {
    if (idx == 0) { rndMode = 0; rndValue = 0; currentScreen = SCR_TOOLS_RANDOM; drawToolsRandom(); }
    if (idx == 1) { pwLen = 16; genPassword(); currentScreen = SCR_TOOLS_PASSWORD; drawToolsPassword(); }
    if (idx == 2) { currentScreen = SCR_TOOLS_BATTERY; drawToolsBattery(); }
    if (idx == 3) {
      battCalTarget = battValid ? battVoltage : 3.7f;
      btnLeft.fireOnPress = false; btnRight.fireOnPress = false;
      currentScreen = SCR_TOOLS_BATTCAL; drawToolsBattCal();
    }
    if (idx == 4) { currentScreen = SCR_TOOLS_CLOCK; drawToolsClock(); }
    if (idx == 5) { macroSelIdx = 0; currentScreen = SCR_TOOLS_MACRO; drawMacroSel(); }
    if (idx == 6) { irFavoriteIdx = 0; setIrNavigationOnRelease(true); currentScreen = SCR_IR_FAVORITES; drawIrFavorites(); }
    if (idx == 7) { chatPortalError = !startChatPortal(); currentScreen = SCR_TOOLS_CHAT; drawChatPortal(); }
  }
  else if (currentMenuIdx == 6) {
    if (idx == 0) { pwdListIdx = 0; currentScreen = SCR_PWD_LIST; drawPwdList(); }
    if (idx == 1) { currentScreen = SCR_SNIFFER; startSniffer(); }
  }
  // Ten newly designed games replace the old arcade collection.
  else if (currentMenuIdx == 7) {
    btnLeft.fireOnPress = false; btnRight.fireOnPress = false;
    arcadeStart(idx); currentScreen = SCR_ARCADE; drawArcade();
  }
  else if (currentMenuIdx == 8) {
    if (idx == 0) { currentScreen = SCR_INFO; drawInfo(); }
    if (idx == 1) { currentScreen = SCR_SPACE; initStars(); drawStars(); }
  }
}
void goBackMenu() {
  int p = menus[currentMenuIdx].parent;
  if (p < 0) return;
  currentMenuIdx = p; drawCurrentMenu();
}
void handleMenuScreen() {
  if (takeShort(btnLeft)) { MenuLevel &m = menus[currentMenuIdx]; startAnim(-1, (m.index - 1 + m.count) % m.count); return; }
  if (takeShort(btnRight)) { MenuLevel &m = menus[currentMenuIdx]; startAnim(+1, (m.index + 1) % m.count); return; }
  if (takeShort(btnSelect)) { enterSelected(); return; }
  if (takeLong(btnSelect)) { goBackMenu(); return; }
}
void handleInput() {
  if (takeShort(btnLeft))  { kbdIdx = (kbdIdx + 40 - 1) % 40; drawKeyboard(); return; }
  if (takeShort(btnRight)) { kbdIdx = (kbdIdx + 1) % 40; drawKeyboard(); return; }
  if (takeLong(btnLeft))   {
    if (inputTarget && inputTarget->length() > 0) inputTarget->remove(inputTarget->length() - 1);
    drawKeyboard(); return;
  }
  if (takeLong(btnRight))  { kbdCat = (kbdCat + 1) % 3; kbdIdx = 0; drawKeyboard(); return; }
  if (takeShort(btnSelect)) {
    if (inputTarget && (int)inputTarget->length() < inputMaxLen) *inputTarget += kbdLayouts[kbdCat][kbdIdx];
    drawKeyboard(); return;
  }
  if (takeLong(btnSelect)) {
    if (inputTarget && inputTarget->length() == 0) {
      int a = inputAction; inputAction = INPUT_NONE; inputTarget = nullptr;
      if (a == INPUT_TEST_PASS) { currentScreen = SCR_TEST_SELECT_NET; drawWifiListGeneric("Nets ", true); }
      else { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); }
      return;
    }
    int a = inputAction; inputAction = INPUT_NONE; inputTarget = nullptr;
    if (a == INPUT_ADD_SSID) {
      if (tempSsid.length() == 0) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); }
      else { startInput("Pass:", &tempPass, 64, INPUT_ADD_PASS, tempSsid.c_str()); currentScreen = SCR_INPUT; drawKeyboard(); }
      return;
    }
    if (a == INPUT_ADD_PASS) { addCred(tempSsid.c_str(), tempPass.c_str()); pwdListIdx = 0; currentScreen = SCR_PWD_LIST; drawPwdList(); return; }
    if (a == INPUT_TEST_PASS) { launchTest(); return; }
    return;
  }
}

// =========================================================
// Main screen handler
// =========================================================
void handleTestScreen() {
  if (currentScreen == SCR_INPUT) { handleInput(); return; }
  if (currentScreen == SCR_ARCADE) { handleArcade(); return; }
  // --- IR POWER ---
  if (currentScreen == SCR_IR_POWER) {
    if (macroRecording) {
      if (takeLong(btnLeft)) { macroRecording = false; currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return; }
      if (takeShort(btnLeft) && irProjIdx > 0) { irProjIdx--; drawIrProj(); return; }
      if (takeShort(btnRight) && irProjIdx < irProjCount - 1) { irProjIdx++; drawIrProj(); return; }
      if (takeShort(btnSelect)) {
        Macro &m = macros[macroSelIdx];
        if (m.nSteps < MAX_STEPS) { m.steps[m.nSteps].isTv = 0; m.steps[m.nSteps].devIdx = irProjIdx; m.steps[m.nSteps].keyIdx = 0; m.nSteps++; saveMacros(); }
        macroRecording = false; currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return;
      }
      return;
    }
    if (takeLong(btnLeft)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && irProjIdx > 0) { irProjIdx--; drawIrProj(); return; }
    if (takeShort(btnRight) && irProjIdx < irProjCount - 1) { irProjIdx++; drawIrProj(); return; }
    if (takeShort(btnSelect)) { irKeyDevice = irProjIdx; irKeyIsTv = false; irKeyIdx = 0; setIrNavigationOnRelease(true); currentScreen = SCR_IR_KEYS; drawIrKeys(); return; }
    if (takeLong(btnSelect)) { irAutoIdx = 0; irAutoRunning = true; irAutoLast = millis() - 1500; currentScreen = SCR_IR_AUTO; drawIrAuto(); return; }
    return;
  }
  // --- IR TV ---
  if (currentScreen == SCR_IR_TV) {
    if (macroRecording) {
      if (takeLong(btnLeft)) { macroRecording = false; currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return; }
      if (takeShort(btnLeft) && irTvIdx > 0) { irTvIdx--; drawIrTv(); return; }
      if (takeShort(btnRight) && irTvIdx < irTvCount - 1) { irTvIdx++; drawIrTv(); return; }
      if (takeShort(btnSelect)) {
        Macro &m = macros[macroSelIdx];
        if (m.nSteps < MAX_STEPS) { m.steps[m.nSteps].isTv = 1; m.steps[m.nSteps].devIdx = irTvIdx; m.steps[m.nSteps].keyIdx = 0; m.nSteps++; saveMacros(); }
        macroRecording = false; currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return;
      }
      return;
    }
    if (takeLong(btnLeft)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && irTvIdx > 0) { irTvIdx--; drawIrTv(); return; }
    if (takeShort(btnRight) && irTvIdx < irTvCount - 1) { irTvIdx++; drawIrTv(); return; }
    if (takeShort(btnSelect)) { irKeyDevice = irTvIdx; irKeyIsTv = true; irKeyIdx = 0; setIrNavigationOnRelease(true); currentScreen = SCR_IR_KEYS; drawIrKeys(); return; }
    if (takeLong(btnSelect)) { irAutoIdx = irProjCount; irAutoRunning = true; irAutoLast = millis() - 1500; currentScreen = SCR_IR_AUTO; drawIrAuto(); return; }
    return;
  }
  // --- IR KEYS ---
  if (currentScreen == SCR_IR_KEYS) {
    const IrDevice* dev = irKeyIsTv ? &irTv[irKeyDevice] : &irProj[irKeyDevice];
    if (macroRecording) {
      if (takeLong(btnLeft)) { macroRecording = false; setIrNavigationOnRelease(false); currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return; }
      if (takeShort(btnLeft) && irKeyIdx > 0) { irKeyIdx--; drawIrKeys(); return; }
      if (takeShort(btnRight) && irKeyIdx < keyCount - 1) { irKeyIdx++; drawIrKeys(); return; }
      if (takeLong(btnRight)) { suppressLong(btnRight); return; }
      if (takeShort(btnSelect)) {
        Macro &m = macros[macroSelIdx];
        if (m.nSteps < MAX_STEPS) { m.steps[m.nSteps].isTv = irKeyIsTv ? 1 : 0; m.steps[m.nSteps].devIdx = irKeyDevice; m.steps[m.nSteps].keyIdx = irKeyIdx; m.nSteps++; saveMacros(); }
        macroRecording = false; setIrNavigationOnRelease(false); currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return;
      }
      return;
    }
    if (takeLong(btnLeft)) { setIrNavigationOnRelease(false); if (irKeyIsTv) { currentScreen = SCR_IR_TV; drawIrTv(); } else { currentScreen = SCR_IR_POWER; drawIrProj(); } return; }
    if (takeLong(btnSelect)) { setIrNavigationOnRelease(false); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && irKeyIdx > 0) { irKeyIdx--; drawIrKeys(); return; }
    if (takeShort(btnRight) && irKeyIdx < keyCount - 1) { irKeyIdx++; drawIrKeys(); return; }
    if (takeLong(btnRight)) {
      irFavoriteFeedback = dev->keys[irKeyIdx] ? addIrFavorite(irKeyIsTv, irKeyDevice, irKeyIdx) : 4;
      if (irFavoriteFeedback == 0) irFavoriteFeedback = 3;
      irFavoriteFeedbackAt = millis();
      drawIrKeys(); return;
    }
    if (takeShort(btnSelect)) { sendIR(*dev, irKeyIdx); drawIrKeys(); return; }
    if (irSent && millis() - irSentAt > 800) { irSent = false; drawIrKeys(); }
    if (irFavoriteFeedback && millis() - irFavoriteFeedbackAt > 1500) { irFavoriteFeedback = 0; drawIrKeys(); }
    return;
  }
  // --- IR AUTO ---
  if (currentScreen == SCR_IR_AUTO) {
    if (takeLong(btnLeft) || takeLong(btnSelect)) { irAutoRunning = false; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (irAutoRunning) {
      if (takeShort(btnSelect)) { irAutoRunning = false; drawIrAuto(); }
      stepIrAuto(); return;
    }
    if (takeShort(btnSelect)) { irAutoIdx = 0; irAutoRunning = true; irAutoLast = millis() - 1500; drawIrAuto(); }
    return;
  }
  // --- IR FAVORITES ---
  if (currentScreen == SCR_IR_FAVORITES) {
    if (takeLong(btnSelect)) { setIrNavigationOnRelease(false); currentMenuIdx = 5; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft)) {
      removeIrFavorite(irFavoriteIdx);
      drawIrFavorites(); return;
    }
    if (takeLong(btnRight)) return;
    if (takeShort(btnLeft)) { irFavoriteIdx = (irFavoriteIdx + MAX_IR_FAVORITES - 1) % MAX_IR_FAVORITES; drawIrFavorites(); return; }
    if (takeShort(btnRight)) { irFavoriteIdx = (irFavoriteIdx + 1) % MAX_IR_FAVORITES; drawIrFavorites(); return; }
    if (takeShort(btnSelect)) {
      IrFavorite &favorite = irFavorites[irFavoriteIdx];
      const IrDevice* dev = getIrFavoriteDevice(favorite);
      if (dev) sendIR(*dev, favorite.keyIdx);
      drawIrFavorites(); return;
    }
    if (irSent && millis() - irSentAt > 800) { irSent = false; drawIrFavorites(); }
    return;
  }
  // --- MACRO LIST ---
  if (currentScreen == SCR_TOOLS_MACRO) {
    if (takeLong(btnLeft)) { macroPlaying = false; currentMenuIdx = 5; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnSelect)) { macroPlaying = false; macroEditIdx = 0; currentScreen = SCR_TOOLS_MACRO_EDIT; drawMacroEdit(); return; }
    if (macroPlaying) {
      if (takeShort(btnSelect)) { macroPlaying = false; drawMacroSel(); return; }
      stepMacroPlay();
      return;
    }
    if (takeShort(btnLeft) && macroSelIdx > 0) { macroSelIdx--; drawMacroSel(); return; }
    if (takeShort(btnRight) && macroSelIdx < MAX_MACROS - 1) { macroSelIdx++; drawMacroSel(); return; }
    if (takeShort(btnSelect) && macros[macroSelIdx].nSteps > 0) { startMacroPlay(macroSelIdx); drawMacroSel(); return; }
    return;
  }
  // --- MACRO EDIT ---
  if (currentScreen == SCR_TOOLS_MACRO_EDIT) {
    Macro &m = macros[macroSelIdx];
    int totalRows = m.nSteps + 3;
    if (macroPlaying && takeShort(btnSelect)) { macroPlaying = false; drawMacroEdit(); return; }
    if (takeLong(btnSelect)) { macroPlaying = false; currentScreen = SCR_TOOLS_MACRO; drawMacroSel(); return; }
    if (takeShort(btnLeft) && macroEditIdx > 0) { macroEditIdx--; drawMacroEdit(); return; }
    if (takeShort(btnRight) && macroEditIdx < totalRows - 1) { macroEditIdx++; drawMacroEdit(); return; }
    if (takeShort(btnSelect)) {
      if (macroEditIdx < m.nSteps) { sendIR(m.steps[macroEditIdx].isTv ? irTv[m.steps[macroEditIdx].devIdx] : irProj[m.steps[macroEditIdx].devIdx], m.steps[macroEditIdx].keyIdx); drawMacroEdit(); return; }
      if (macroEditIdx == m.nSteps) { macroRecording = true; irTvIdx = 0; currentScreen = SCR_IR_TV; drawIrTv(); return; }
      if (macroEditIdx == m.nSteps + 1) { macroRecording = true; irProjIdx = 0; currentScreen = SCR_IR_POWER; drawIrProj(); return; }
      if (macroEditIdx == m.nSteps + 2) { startMacroPlay(macroSelIdx); drawMacroEdit(); return; }
      return;
    }
    if (takeLong(btnLeft) && macroEditIdx < m.nSteps) {
      for (int i = macroEditIdx; i < m.nSteps - 1; i++) m.steps[i] = m.steps[i + 1];
      m.nSteps--; saveMacros();
      if (macroEditIdx >= totalRows - 1) macroEditIdx = totalRows - 2;
      drawMacroEdit(); return;
    }
    if (macroPlaying) stepMacroPlay();
    return;
  }
  // --- TOOLS ---
  if (currentScreen == SCR_TOOLS_CHAT) {
    if (takeLong(btnSelect)) {
      stopChatPortal();
      currentMenuIdx = 5; currentScreen = SCR_MENU; drawCurrentMenu(); return;
    }
    return;
  }
  if (currentScreen == SCR_TOOLS_RANDOM) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft)) { rndMode = (rndMode + 3) % 4; rndValue = 0; drawToolsRandom(); return; }
    if (takeShort(btnRight)) { rndMode = (rndMode + 1) % 4; rndValue = 0; drawToolsRandom(); return; }
    if (takeShort(btnSelect)) {
      if (rndMode == 0) rndValue = random(2);
      else if (rndMode == 1) rndValue = random(1, 7);
      else if (rndMode == 2) rndValue = random(1, 21);
      else rndValue = random(1, 101);
      drawToolsRandom(); return;
    }
    return;
  }
  if (currentScreen == SCR_TOOLS_PASSWORD) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft)) { int i = 0; while (i < 4 && pwLens[i] != pwLen) i++; i = (i + 4) % 5; pwLen = pwLens[i]; genPassword(); drawToolsPassword(); return; }
    if (takeShort(btnRight)) { int i = 0; while (i < 4 && pwLens[i] != pwLen) i++; i = (i + 1) % 5; pwLen = pwLens[i]; genPassword(); drawToolsPassword(); return; }
    if (takeShort(btnSelect)) { genPassword(); drawToolsPassword(); return; }
    return;
  }
  if (currentScreen == SCR_TOOLS_BATTERY) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft)) { clearBattGraph(); drawToolsBattery(); return; }
    static uint32_t lb = 0;
    if (millis() - lb > 2000) { lb = millis(); drawToolsBattery(); }
    return;
  }
  if (currentScreen == SCR_TOOLS_BATTCAL) {
    if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (!battValid) return;
    if (takeShort(btnLeft))  { battCalTarget = max(2.5f, battCalTarget - 0.01f); drawToolsBattCal(); return; }
    if (takeShort(btnRight)) { battCalTarget = min(4.5f, battCalTarget + 0.01f); drawToolsBattCal(); return; }
    if (takeLong(btnLeft))   { battCalTarget = max(2.5f, battCalTarget - 0.10f); drawToolsBattCal(); return; }
    if (takeLong(btnRight))  { battCalTarget = min(4.5f, battCalTarget + 0.10f); drawToolsBattCal(); return; }
    if (takeShort(btnSelect)) {
      if (battVoltage > 0.5f) { battCal *= battCalTarget / battVoltage; saveBattCal(); battVoltage = readBatteryVoltage(); battPct = constrain(battPercentFromV(battVoltage), 0, 100); }
      drawToolsBattCal(); return;
    }
    static uint32_t lc2 = 0;
    if (millis() - lc2 > 500) { lc2 = millis(); battVoltage = readBatteryVoltage(); drawToolsBattCal(); }
    return;
  }
  if (currentScreen == SCR_TOOLS_CLOCK) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { ntpTrigger(); drawToolsClock(); return; }
    static uint32_t lc = 0;
    if (millis() - lc > 500) { lc = millis(); drawToolsClock(); }
    return;
  }
  // --- WIFI SCAN ---
  if (currentScreen == SCR_WIFI_SCAN) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) {
      if (scanMode == SCAN_ON) { haltScan(); drawWifiScan(); }
      else if (wifiApCount > 0) { currentScreen = SCR_WIFI_DETAIL; drawWifiDetail(); }
      return;
    }
    if (takeShort(btnLeft) && wifiScrollIdx > 0) { wifiScrollIdx--; drawWifiScan(); return; }
    if (takeShort(btnRight) && wifiScrollIdx < wifiApCount - 1) { wifiScrollIdx++; drawWifiScan(); return; }
    if (tickScan()) drawWifiScan();
    return;
  }
  if (currentScreen == SCR_WIFI_DETAIL) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { currentScreen = SCR_WIFI_SCAN; drawWifiScan(); return; }
    if (takeShort(btnLeft) && wifiScrollIdx > 0) { wifiScrollIdx--; drawWifiDetail(); return; }
    if (takeShort(btnRight) && wifiScrollIdx < wifiApCount - 1) { wifiScrollIdx++; drawWifiDetail(); return; }
    return;
  }
  if (currentScreen == SCR_WIFI_CHANNELS) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { if (scanMode == SCAN_ON) haltScan(); else startScan(); drawWifiChannels(); return; }
    if (tickScan()) drawWifiChannels();
    return;
  }
  if (currentScreen == SCR_ANALYZE_SIGNAL) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft) || takeLong(btnRight)) return;
    if (takeShort(btnLeft) && wifiApCount > 0) { wifiScrollIdx = (wifiScrollIdx + wifiApCount - 1) % wifiApCount; drawAnalyzeSignal(); return; }
    if (takeShort(btnRight) && wifiApCount > 0) { wifiScrollIdx = (wifiScrollIdx + 1) % wifiApCount; drawAnalyzeSignal(); return; }
    if (takeShort(btnSelect)) { exportWifiSurveyCsv(); drawAnalyzeSignal(); return; }
    if (tickScan()) drawAnalyzeSignal();
    return;
  }
  if (currentScreen == SCR_ANALYZE_HIDDEN || currentScreen == SCR_ANALYZE_TWINS) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) {
      if (scanMode == SCAN_ON) haltScan(); else startScan();
      if (currentScreen == SCR_ANALYZE_HIDDEN) drawAnalyzeHidden(); else drawAnalyzeTwins();
      return;
    }
    if (tickScan()) { if (currentScreen == SCR_ANALYZE_HIDDEN) drawAnalyzeHidden(); else drawAnalyzeTwins(); }
    return;
  }
  if (currentScreen == SCR_WIFI_MAP_SEL) {
    if (takeLong(btnLeft)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && mapTargetIdx > 0) { mapTargetIdx--; drawWifiMapSelect(); return; }
    if (takeShort(btnRight) && mapTargetIdx < wifiApCount - 1) { mapTargetIdx++; drawWifiMapSelect(); return; }
    if (takeShort(btnSelect) || takeLong(btnSelect)) {
      if (wifiApCount > 0) {
        uint8_t selBssid[6]; memcpy(selBssid, wifiAps[mapTargetIdx].bssidRaw, 6);
        String selSsid = String(wifiAps[mapTargetIdx].ssid);
        haltScan();
        int newIdx = findApByBssid(selBssid); if (newIdx >= 0) mapTargetIdx = newIdx;
        if (selSsid != mapSsid) { clearMap(); mapSsid = selSsid; saveMap(); }
        mapScrollIdx = max(0, mapPtCount - 1);
        currentScreen = SCR_WIFI_MAP; drawWifiMap();
      } else { startScan(); drawWifiMapSelect(); }
      return;
    }
    if (tickScan()) drawWifiMapSelect();
    return;
  }
  if (currentScreen == SCR_WIFI_MAP) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { logMapPoint(); drawWifiMap(); return; }
    if (takeShort(btnLeft) && mapScrollIdx > 0) { mapScrollIdx--; drawWifiMap(); return; }
    if (takeShort(btnRight) && mapScrollIdx < mapPtCount - 1) { mapScrollIdx++; drawWifiMap(); return; }
    if (takeLong(btnLeft)) { clearMap(); drawWifiMap(); return; }
    return;
  }
  // --- TEST ---
  if (currentScreen == SCR_TEST_SAVED_LIST) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && pwdListIdx > 0) { pwdListIdx--; drawTestSavedList(); return; }
    if (takeShort(btnRight) && pwdListIdx < credCount - 1) { pwdListIdx++; drawTestSavedList(); return; }
    if (takeShort(btnSelect) && credCount > 0) { pendingSSID = String(creds[pwdListIdx].ssid); pendingPass = String(creds[pwdListIdx].pass); launchTest(); return; }
    return;
  }
  if (currentScreen == SCR_TEST_SELECT_NET) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (scanMode == SCAN_ON) {
      if (takeShort(btnSelect)) { haltScan(); drawWifiListGeneric("Nets ", true); return; }
      if (takeShort(btnLeft) && wifiScrollIdx > 0) { wifiScrollIdx--; drawWifiListGeneric("Nets ", true); return; }
      if (takeShort(btnRight) && wifiScrollIdx < wifiApCount - 1) { wifiScrollIdx++; drawWifiListGeneric("Nets ", true); return; }
      if (tickScan()) drawWifiListGeneric("Nets ", true);
      return;
    }
    if (wifiApCount == 0) { if (takeShort(btnSelect)) { startScan(); drawWifiListGeneric("Nets ", true); } return; }
    if (takeShort(btnLeft) && wifiScrollIdx > 0) { wifiScrollIdx--; drawWifiListGeneric("Nets ", true); return; }
    if (takeShort(btnRight) && wifiScrollIdx < wifiApCount - 1) { wifiScrollIdx++; drawWifiListGeneric("Nets ", true); return; }
    if (takeShort(btnSelect)) {
      pendingSSID = String(wifiAps[wifiScrollIdx].ssid);
      pendingPass = "";
      startInput("Pass:", &pendingPass, 64, INPUT_TEST_PASS, pendingSSID.c_str());
      currentScreen = SCR_INPUT; drawKeyboard(); return;
    }
    return;
  }
  if (currentScreen == SCR_TEST_PING) {
    if (takeLong(btnSelect)) { stopNetworkTest(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft)) { stopNetworkTest(); currentScreen = SCR_TEST_SELECT_NET; startScan(); drawWifiListGeneric("Nets ", true); return; }
    if (takeShort(btnSelect)) { startNetworkTest(false); return; }
    tickNetworkTest();
    return;
  }
  if (currentScreen == SCR_TEST_SPEED) {
    if (takeLong(btnSelect)) { stopNetworkTest(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft)) { stopNetworkTest(); currentScreen = SCR_TEST_SELECT_NET; startScan(); drawWifiListGeneric("Nets ", true); return; }
    if (takeShort(btnSelect)) { startNetworkTest(true); return; }
    tickNetworkTest();
    return;
  }
  // --- PWD ---
  if (currentScreen == SCR_PWD_LIST) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeLong(btnLeft)) { currentScreen = SCR_PWD_HELP; drawPwdHelp(); return; }
    if (takeShort(btnLeft) && pwdListIdx > 0) { suppressLong(btnLeft); pwdListIdx--; drawPwdList(); return; }
    if (takeShort(btnRight) && pwdListIdx < credCount - 1) { suppressLong(btnRight); pwdListIdx++; drawPwdList(); return; }
    if (takeLong(btnRight)) { tempSsid = ""; tempPass = ""; startInput("SSID:", &tempSsid, 32, INPUT_ADD_SSID); currentScreen = SCR_INPUT; drawKeyboard(); return; }
    if (takeShort(btnSelect) && credCount > 0) { currentScreen = SCR_PWD_DETAIL; drawPwdDetail(); return; }
    return;
  }
  if (currentScreen == SCR_PWD_DETAIL) {
    if (takeShort(btnSelect) || takeLong(btnSelect)) { suppressAll(); currentScreen = SCR_PWD_LIST; drawPwdList(); return; }
    if (takeLong(btnLeft) && credCount > 0) { pwdDelIdx = pwdListIdx; currentScreen = SCR_PWD_DELETE_CONFIRM; drawPwdDeleteConfirm(); return; }
    if (takeShort(btnRight) || takeLong(btnRight)) { btnLeft.fireOnPress = false; btnRight.fireOnPress = false; currentScreen = SCR_PWD_QR; drawPwdQR(); return; }
    return;
  }
  if (currentScreen == SCR_PWD_QR) {
    if (anyBtnEvent()) { suppressAll(); btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentScreen = SCR_PWD_DETAIL; drawPwdDetail(); }
    return;
  }
  if (currentScreen == SCR_PWD_DELETE_CONFIRM) {
    if (takeLong(btnSelect)) {
      suppressAll();
      delCred(pwdDelIdx);
      if (pwdListIdx >= credCount && credCount > 0) pwdListIdx = credCount - 1;
      if (credCount == 0) pwdListIdx = 0;
      currentScreen = SCR_PWD_LIST; drawPwdList(); return;
    }
    if (takeShort(btnSelect) || takeShort(btnLeft) || takeLong(btnLeft) || takeShort(btnRight) || takeLong(btnRight)) {
      suppressAll(); currentScreen = SCR_PWD_LIST; drawPwdList(); return;
    }
    return;
  }
  if (currentScreen == SCR_PWD_HELP) {
    if (anyBtnEvent()) { suppressAll(); currentScreen = SCR_PWD_LIST; drawPwdList(); }
    return;
  }
  // --- CLONE ---
  if (currentScreen == SCR_ATTACK_CLONE_SEL) {
    if (takeLong(btnSelect)) { haltScan(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && cloneTargetIdx > 0) { cloneTargetIdx--; drawCloneSelect(); return; }
    if (takeShort(btnRight) && cloneTargetIdx < wifiApCount - 1) { cloneTargetIdx++; drawCloneSelect(); return; }
    if (takeShort(btnSelect) && wifiApCount > 0) {
      uint8_t selBssid[6]; memcpy(selBssid, wifiAps[cloneTargetIdx].bssidRaw, 6);
      haltScan(); int newIdx = findApByBssid(selBssid); if (newIdx >= 0) cloneTargetIdx = newIdx;
      startClone(); return;
    }
    if (tickScan()) drawCloneSelect();
    return;
  }
  if (currentScreen == SCR_ATTACK_CLONE_RUN) {
    if (takeShort(btnSelect) || takeLong(btnSelect)) { stopClone(); currentScreen = SCR_ATTACK_CLONE_SEL; drawCloneSelect(); return; }
    stepClone(); return;
  }
  if (currentScreen == SCR_ATTACK_SPAM_RUN) {
    if (takeShort(btnSelect) || takeLong(btnSelect)) { stopSpam(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    stepSpam(); return;
  }
  // --- SNIFFER ---
  if (currentScreen == SCR_SNIFFER) {
    if (snifferRunning) {
      if (millis() - snifferStart > SNIFF_DURATION_MS) { stopSniffer(); return; }
      if (takeLong(btnSelect)) { stopSniffer(); currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      static uint32_t lsn = 0;
      if (millis() - lsn > 500) { lsn = millis(); drawSniffer(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { startSniffer(); return; }
    return;
  }
  // --- INFO / SPACE ---
  if (currentScreen == SCR_INFO) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    static uint32_t li = 0;
    if (millis() - li > 500) { li = millis(); drawInfo(); }
    return;
  }
  if (currentScreen == SCR_SPACE) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { spacePaused = !spacePaused; drawStars(); return; }
    static uint32_t ls = 0;
    if (millis() - ls > 30) { ls = millis(); drawStars(); }
    return;
  }
}

// =========================================================
// setup / loop
// =========================================================
void drawBootMsg(const char* msg) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 28); display.print(msg);
  display.display();
}
bool tryAutoDeepSleep() {
  if (currentScreen != SCR_MENU || chatServerRunning || scanMode != SCAN_OFF ||
      ntpState != NTP_IDLE || cloneRunning || spamRunning || snifferRunning || deauthRunning ||
      millis() - lastBtnEventMs < AUTO_DEEP_SLEEP_MS) return false;
  if (digitalRead(BTN_L) == LOW || digitalRead(BTN_R) == LOW || digitalRead(BTN_M) == LOW) {
    lastBtnEventMs = millis(); return false;
  }
  delay(40);
  if (digitalRead(BTN_L) == LOW || digitalRead(BTN_R) == LOW || digitalRead(BTN_M) == LOW) {
    lastBtnEventMs = millis(); return false;
  }
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  uint64_t wakeMask = (1ULL << BTN_L) | (1ULL << BTN_R) | (1ULL << BTN_M);
  if (esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK) {
    display.ssd1306_command(SSD1306_DISPLAYON); displaySleeping = false;
    lastBtnEventMs = millis(); drawCurrentMenu(); return false;
  }
  Serial.flush();
  esp_deep_sleep_start();
  return true; // Deep sleep starts a fresh boot when any key is pressed.
}
void setup() {
  Serial.begin(115200);
  configureChatRoutes();
  delay(300);
  pinMode(BTN_L, INPUT_PULLUP); pinMode(BTN_R, INPUT_PULLUP); pinMode(BTN_M, INPUT_PULLUP);
  pinMode(IR_PIN, OUTPUT); digitalWrite(IR_PIN, LOW);
  delay(50);
  irSend.begin();
  randomSeed(esp_random());
  WiFi.mode(WIFI_OFF); WiFi.persistent(false);
  loadCreds(); loadBattCal(); loadMap(); loadMacros(); loadIrFavorites();
  analogReadResolution(12);
  battFirstDone = false;
  lastBattSample = millis();
  updateBattery();
  Wire.begin(SDA_PIN, SCL_PIN); Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for (;;) delay(1000); }
  display.setTextWrap(false);
  display.clearDisplay();
  display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 16); display.print("TRICORDER");
  display.setTextSize(1); display.setCursor(16, 38); display.print("v25.1 ready");
  display.display();
  delay(1500);
  drawBootMsg("Init FAT...");
  fatReady = FFat.begin(true);
  if (fatReady) loadBattGraph();
  lastNtpAttempt = 0;
  drawCurrentMenu();
}
void loop() {
  pollButton(btnLeft); pollButton(btnRight); pollButton(btnSelect);
  if (chatServerRunning) chatWeb.handleClient();
  if (tryAutoDeepSleep()) return;
  if (displaySleeping) {
    updateBattery();
    updateClock();
    if (!chatServerRunning) tickNtp();
    if (anyBtnEvent()) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displaySleeping = false;
      lastBtnEventMs = millis();
      suppressAll();
      drawCurrentMenu();
    } else delay(20);
    return;
  }
  updateBattery();
  updateClock();
  if (!chatServerRunning) tickNtp();
  if (tryAutoDeepSleep()) return;
  static int lastMenuBatteryPct = -999;
  if (currentScreen == SCR_MENU && lastMenuBatteryPct != battPct) {
    lastMenuBatteryPct = battPct;
    drawCurrentMenu();
  }
  uint32_t sleepAfter = (battValid && battPct <= 10) ? 30000UL : DISPLAY_SLEEP_MS;
  if (currentScreen == SCR_MENU && millis() - lastBtnEventMs >= sleepAfter) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displaySleeping = true;
    delay(20);
    return;
  }
  if (currentScreen == SCR_MENU) handleMenuScreen();
  else handleTestScreen();
}