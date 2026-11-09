// ============================================================
// TRICORDER v25.1
// v23.1: новая клавиатура 10×4
// v24.0: Games — Arcade/Logic/Cards/Idle
// v25.0: battery saver, IR favorites, passive WiFi survey
// v25.1: local tiny-chat AP and safe menu header layout
//        Tetris, Pong, Flappy, Doodle, Blackjack, BJ Multi, Farm
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
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

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
struct BJack;
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

// ---- High scores ----
int hiSnake = 0, hi2048 = 0, hiSimon = 0;
int hiTetris = 0, hiFlappy = 0, hiDoodle = 0;
uint32_t hiFarm = 0;
int hiBJ = 0;
void loadHighScores() {
  prefs.begin("hscore", true);
  hiSnake  = prefs.getInt("snake", 0);
  hi2048   = prefs.getInt("g2048", 0);
  hiSimon  = prefs.getInt("simon", 0);
  hiTetris = prefs.getInt("tetris", 0);
  hiFlappy = prefs.getInt("flappy", 0);
  hiDoodle = prefs.getInt("doodle", 0);
  hiFarm   = prefs.getUInt("farm", 0);
  hiBJ     = prefs.getInt("bj", 0);
  prefs.end();
}
void saveHighScore(const char* key, int val) {
  prefs.begin("hscore", false);
  prefs.putInt(key, val);
  prefs.end();
}
void saveHighScoreU(const char* key, uint32_t val) {
  prefs.begin("hscore", false);
  prefs.putUInt(key, val);
  prefs.end();
}

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
<title>Tricorder Tiny Chat</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#101522;color:#e7eefc;font:16px system-ui,sans-serif;display:grid;min-height:100vh;place-items:center}
main{width:min(680px,100%);height:min(760px,100vh);padding:18px;display:flex;flex-direction:column;gap:12px}h1{font-size:1.2rem;margin:0}small{color:#9eacc5}.log{flex:1;overflow:auto;background:#171f30;border:1px solid #2b3850;border-radius:14px;padding:12px;display:flex;flex-direction:column;gap:10px}
.msg{max-width:90%;padding:10px 12px;border-radius:12px;white-space:pre-wrap;line-height:1.4}.bot{align-self:flex-start;background:#25324a}.user{align-self:flex-end;background:#245e73}.row{display:flex;gap:8px}input{min-width:0;flex:1;background:#171f30;color:white;border:1px solid #3b4b67;border-radius:10px;padding:12px;font:inherit}button{border:0;border-radius:10px;padding:0 18px;background:#67d5c3;color:#071a20;font-weight:700;font:inherit}
</style></head><body><main><h1>Tricorder · Tiny Chat</h1><small>Локальная мини-сеть 8→8→9. Это классификатор тем с короткими ответами, не большая языковая модель и не требует интернета.</small>
<div id="log" class="log"><div class="msg bot">Привет! Спроси про заряд, время, Wi‑Fi или ИК-пульт.</div></div><form id="form" class="row"><input id="text" maxlength="120" autocomplete="off" placeholder="Напишите сообщение…"><button>Отправить</button></form></main>
<script>const log=document.querySelector('#log'),input=document.querySelector('#text');function add(t,c){const d=document.createElement('div');d.className='msg '+c;d.textContent=t;log.append(d);log.scrollTop=log.scrollHeight}document.querySelector('#form').addEventListener('submit',async e=>{e.preventDefault();const q=input.value.trim();if(!q)return;input.value='';add(q,'user');try{const r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:q.toLocaleLowerCase()});add(await r.text(),'bot')}catch(_){add('Связь с устройством потеряна. Подключитесь снова к Tricorder-Chat.','bot')}});</script></body></html>
)CHATHTML";

// Fixed, tiny 8-input -> 8-hidden -> 9-output ReLU network.
// Inputs are lightweight keyword features; replies are local templates, not generated prose.
const float chatW1[8][8] = {
  {1,0,0,0,0,0,0,0},{0,1,0,0,0,0,0,0},{0,0,1,0,0,0,0,0},{0,0,0,1,0,0,0,0},
  {0,0,0,0,1,0,0,0},{0,0,0,0,0,1,0,0},{0,0,0,0,0,0,1,0},{0,0,0,0,0,0,0,1}
};
const float chatW2[9][8] = {
  {1,0,0,0,0,0,0,0},{0,1,0,0,0,0,0,0},{0,0,1,0,0,0,0,0},{0,0,0,1,0,0,0,0},
  {0,0,0,0,1,0,0,0},{0,0,0,0,0,1,0,0},{0,0,0,0,0,0,1,0},{0,0,0,0,0,0,0,1},
  {0,0,0,0,0,0,0,0}
};
const float chatHiddenBias[8] = {0,0,0,0,0,0,0,0};
const float chatOutputBias[9] = {0,0,0,0,0,0,0,0,0.15f};

bool chatHasAny(const String& text, const char* const* words, size_t count) {
  for (size_t i = 0; i < count; i++) if (text.indexOf(words[i]) >= 0) return true;
  return false;
}
int classifyChatIntent(const String& text) {
  static const char* greeting[] = {"привет","здравств","добрый","hello","hi"};
  static const char* battery[] = {"батар","аккум","заряд","battery"};
  static const char* clock[] = {"время","час","time"};
  static const char* wifi[] = {"wifi","wi-fi","вайф","сеть"};
  static const char* infrared[] = {"ик-пульт","ик команд","инфракрас","пульт","телевиз","проектор","ir "};
  static const char* help[] = {"помощ","умеешь","функци","help"};
  static const char* joke[] = {"шутк","смешн","анекдот"};
  static const char* bye[] = {"пока","до свид","bye"};
  float input[8] = {
    chatHasAny(text, greeting, sizeof(greeting)/sizeof(greeting[0])) ? 1.0f : 0.0f,
    chatHasAny(text, battery, sizeof(battery)/sizeof(battery[0])) ? 1.0f : 0.0f,
    chatHasAny(text, clock, sizeof(clock)/sizeof(clock[0])) ? 1.0f : 0.0f,
    chatHasAny(text, wifi, sizeof(wifi)/sizeof(wifi[0])) ? 1.0f : 0.0f,
    chatHasAny(text, infrared, sizeof(infrared)/sizeof(infrared[0])) ? 1.0f : 0.0f,
    chatHasAny(text, help, sizeof(help)/sizeof(help[0])) ? 1.0f : 0.0f,
    chatHasAny(text, joke, sizeof(joke)/sizeof(joke[0])) ? 1.0f : 0.0f,
    chatHasAny(text, bye, sizeof(bye)/sizeof(bye[0])) ? 1.0f : 0.0f
  };
  float hidden[8];
  for (int h = 0; h < 8; h++) {
    float sum = chatHiddenBias[h];
    for (int i = 0; i < 8; i++) sum += chatW1[h][i] * input[i];
    hidden[h] = sum > 0 ? sum : 0; // ReLU
  }
  int best = 8;
  float bestScore = chatOutputBias[8];
  for (int o = 0; o < 8; o++) {
    float score = chatOutputBias[o];
    for (int h = 0; h < 8; h++) score += chatW2[o][h] * hidden[h];
    if (score > bestScore) { bestScore = score; best = o; }
  }
  return best;
}
String tinyChatReply(const String& text) {
  switch (classifyChatIntent(text)) {
    case 0: return "Привет! Я работаю прямо на ESP32 и отвечаю офлайн.";
    case 1:
      if (battValid) { String r = "Заряд: "; r += battPct; r += "%, напряжение "; r += String(battVoltage, 2); r += " В."; return r; }
      return "Пока не вижу корректного измерения батареи. Проверь делитель и калибровку ADC.";
    case 2:
      if (ntpSynced) { char t[16]; snprintf(t, sizeof(t), "%02d:%02d", clockHour, clockMin); return String("Текущее время: ") + t + "."; }
      return "Время ещё не синхронизировано. Запусти Tools → Clock при доступной сохранённой Wi‑Fi сети.";
    case 3: return "Для пассивного обзора открой WiFi → Analyze → Signal. Там есть история RSSI и экспорт CSV через Serial.";
    case 4: return "ИК-команды: открой Proj или TV, выбери устройство и кнопку. Удерживай RIGHT, чтобы сохранить команду в избранное.";
    case 5: return "Я мини-чат: могу подсказать заряд, время, меню Wi‑Fi и управление ИК. Это не LLM — модель выбирает тему и выдаёт короткий шаблонный ответ.";
    case 6: return "Почему ESP32 не спорит с батареей? Потому что та всегда на последнем проценте.";
    case 7: return "Пока! Чат остаётся локальным и не отправляет сообщения в интернет.";
    default: return "Я пока понимаю темы: заряд, время, Wi‑Fi, ИК-пульт, помощь и шутки. Попробуй спросить об одной из них.";
  }
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
  display.setCursor(2, 0); display.print("Tiny NN Chat");
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
// v24.1: плоский Games (10 пунктов)
const char* gamesItems[] = { "Snake","Pong","Flappy","Doodle","Mines","2048","Simon","Tetris","BJ","Farm" };
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
  { "Games",    gamesItems,   10, 0, 0 },
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
  SCR_MINES, SCR_SNAKE, SCR_2048, SCR_SIMON,
  SCR_TETRIS, SCR_PONG, SCR_FLAPPY, SCR_DOODLE,
  SCR_BJ, SCR_BJM, SCR_FARM,
  SCR_SNIFFER, SCR_INFO, SCR_SPACE
};
Screen currentScreen = SCR_MENU;
int testParentMenu = 0;
int pwdListIdx = 0, pwdDelIdx = 0;

bool animating = false;
int animOffset = 0, animDir = 0, animPrevIdx = 0, animMenuIdx = 0;
const int ANIM_STEP = 16;

// =========================================================
// UI primitives
// =========================================================
void drawArrow(int x, int y, bool r) {
  if (r) display.fillTriangle(x + 8, y, x + 8, y + 12, x, y + 6, SSD1306_WHITE);
  else display.fillTriangle(x, y, x, y + 12, x + 8, y + 6, SSD1306_WHITE);
}
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
void drawMenuChrome(int activeIdx) {
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
  drawArrow(2, 20, false); drawArrow(SW - 10, 20, true);
  int sp = 12;
  int tw = m.count * sp - (sp - 6);
  int sx = (SW - tw) / 2;
  for (int i = 0; i < m.count; i++) {
    int cx = sx + i * sp + 3;
    if (i == activeIdx) display.fillCircle(cx, 44, 3, SSD1306_WHITE);
    else display.drawCircle(cx, 44, 3, SSD1306_WHITE);
  }
}
void drawCurrentMenu() {
  display.clearDisplay();
  drawMenuChrome(menus[currentMenuIdx].index);
  drawCenteredText(menus[currentMenuIdx].items[menus[currentMenuIdx].index], 18, 0, 2);
  if (battValid && battPct <= LOW_BATTERY_WARN_PCT) {
    display.setTextSize(1); display.setCursor(2, 56);
    display.print("LOW BATTERY "); display.print(battPct); display.print("%");
  }
  display.display();
}
void drawAnimFrame() {
  display.clearDisplay();
  MenuLevel &m = menus[animMenuIdx];
  int ad = (animOffset < SW / 2) ? animPrevIdx : m.index;
  int si = m.index, sm = currentMenuIdx;
  currentMenuIdx = animMenuIdx; drawMenuChrome(ad); currentMenuIdx = sm;
  int os = -animDir * animOffset, ns = animDir * (SW - animOffset);
  drawCenteredText(m.items[animPrevIdx], 18, os, 2);
  drawCenteredText(m.items[si], 18, ns, 2);
  display.display();
}
void startAnim(int dir, int ni) {
  if (animating) return;
  animPrevIdx = menus[currentMenuIdx].index;
  animDir = dir; menus[currentMenuIdx].index = ni;
  animMenuIdx = currentMenuIdx; animOffset = 0; animating = true;
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

// =========================================================
// GAMES: Mines, Snake, 2048, Simon
// =========================================================
#define MS_SIZE 8
#define MS_MINES 10
#define MS_CELL 6
#define MS_X 4
#define MS_Y 10
struct MsCell { uint8_t mine : 1; uint8_t open : 1; uint8_t flag : 1; uint8_t num : 4; };
MsCell msGrid[MS_SIZE][MS_SIZE];
int msCurIdx = 27, msOpened = 0, msFlags = 0;
bool msGameOver = false, msWon = false, msMinesPlaced = false;
uint32_t msStartTime = 0;
void msPlaceMines(int exX, int exY) {
  int pl = 0;
  while (pl < MS_MINES) {
    int x = random(MS_SIZE), y = random(MS_SIZE);
    if (msGrid[y][x].mine || (abs(x - exX) <= 1 && abs(y - exY) <= 1)) continue;
    msGrid[y][x].mine = 1; pl++;
  }
  for (int y = 0; y < MS_SIZE; y++) for (int x = 0; x < MS_SIZE; x++) {
    if (msGrid[y][x].mine) continue;
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
      if (!dx && !dy) continue;
      int nx = x + dx, ny = y + dy;
      if (nx >= 0 && nx < MS_SIZE && ny >= 0 && ny < MS_SIZE && msGrid[ny][nx].mine) n++;
    }
    msGrid[y][x].num = n;
  }
}
void msInit() {
  msOpened = 0; msFlags = 0; msGameOver = false; msWon = false; msMinesPlaced = false;
  msCurIdx = 27; msStartTime = millis();
  for (int y = 0; y < MS_SIZE; y++) for (int x = 0; x < MS_SIZE; x++) msGrid[y][x] = {0, 0, 0, 0};
}
void msReveal(int x, int y) {
  if (x < 0 || x >= MS_SIZE || y < 0 || y >= MS_SIZE) return;
  if (msGrid[y][x].open || msGrid[y][x].flag) return;
  msGrid[y][x].open = 1; msOpened++;
  if (msGrid[y][x].mine) { msGameOver = true; return; }
  if (msGrid[y][x].num == 0)
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
      if (dx || dy) msReveal(x + dx, y + dy);
}
void drawMines() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (msGameOver) display.print("GAME OVER");
  else if (msWon) display.print("YOU WIN!");
  else { display.print("M:"); display.print(MS_MINES - msFlags); display.print(" T:"); display.print((millis() - msStartTime) / 1000); }
  for (int y = 0; y < MS_SIZE; y++) for (int x = 0; x < MS_SIZE; x++) {
    int px = MS_X + x * MS_CELL, py = MS_Y + y * MS_CELL;
    MsCell &c = msGrid[y][x];
    bool cur = (y * MS_SIZE + x) == msCurIdx && !msGameOver && !msWon;
    if (cur) { display.fillRect(px, py, MS_CELL, MS_CELL, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else { display.setTextColor(SSD1306_WHITE); if (!c.open) display.drawRect(px, py, MS_CELL, MS_CELL, SSD1306_WHITE); }
    if (c.open) {
      if (c.mine) {
        display.drawLine(px + 1, py + 1, px + MS_CELL - 2, py + MS_CELL - 2, cur ? SSD1306_BLACK : SSD1306_WHITE);
        display.drawLine(px + MS_CELL - 2, py + 1, px + 1, py + MS_CELL - 2, cur ? SSD1306_BLACK : SSD1306_WHITE);
      } else if (c.num > 0) { display.setCursor(px + 1, py); display.print(c.num); }
    } else if (c.flag) display.fillRect(px + 2, py + 2, 3, 3, cur ? SSD1306_BLACK : SSD1306_WHITE);
  }
  display.setTextColor(SSD1306_WHITE);
  int rx = MS_X + MS_SIZE * MS_CELL + 6;
  if (msGameOver || msWon) {
    display.setCursor(rx, 20); display.print("M=");
    display.setCursor(rx, 30); display.print("new");
    display.setCursor(rx, 44); display.print("hM=");
    display.setCursor(rx, 54); display.print("exit");
  } else {
    int cx = msCurIdx % MS_SIZE, cy = msCurIdx / MS_SIZE;
    display.setCursor(rx, 10); display.print("L/R mv");
    display.setCursor(rx, 22); display.print("hL/R ud");
    display.setCursor(rx, 34); display.print("M=open");
    display.setCursor(rx, 46);
    display.print(msGrid[cy][cx].open ? "hM=exit" : "hM=flag");
  }
  display.display();
}

#define SN_SIZE 8
#define SN_CELL 6
#define SN_X 4
#define SN_Y 10
int8_t snX[64], snY[64];
int snLen = 0;
int snDir = 1;
int snAppleX = 0, snAppleY = 0;
bool snGameOver = false;
bool snPaused = false;
uint32_t snLastMove = 0;
const uint32_t SN_SPEED_MS = 350;
void snSpawnApple() {
  while (true) {
    int ax = random(SN_SIZE), ay = random(SN_SIZE);
    bool ok = true;
    for (int i = 0; i < snLen; i++) if (snX[i] == ax && snY[i] == ay) { ok = false; break; }
    if (ok) { snAppleX = ax; snAppleY = ay; return; }
  }
}
void snInit() {
  snLen = 3;
  snX[0] = 3; snY[0] = 3;
  snX[1] = 2; snY[1] = 3;
  snX[2] = 1; snY[2] = 3;
  snDir = 1;
  snGameOver = false; snPaused = false;
  snLastMove = millis();
  snSpawnApple();
}
void snStep() {
  if (snGameOver || snPaused) return;
  if (millis() - snLastMove < SN_SPEED_MS) return;
  snLastMove = millis();
  int nx = snX[0], ny = snY[0];
  if (snDir == 0) ny--;
  else if (snDir == 1) nx++;
  else if (snDir == 2) ny++;
  else if (snDir == 3) nx--;
  if (nx < 0 || nx >= SN_SIZE || ny < 0 || ny >= SN_SIZE) { snGameOver = true; return; }
  for (int i = 0; i < snLen - 1; i++) if (snX[i] == nx && snY[i] == ny) { snGameOver = true; return; }
  bool ate = (nx == snAppleX && ny == snAppleY);
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i-1]; snY[i] = snY[i-1]; }
  snX[0] = nx; snY[0] = ny;
  if (ate && snLen < 64) { snLen++; snSpawnApple(); }
}
void drawSnake() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (snGameOver) display.print("OVER  len:");
  else if (snPaused) display.print("PAUSE len:");
  else display.print("Snake len:");
  display.print(snLen); display.print(" hi:"); display.print(hiSnake);
  int rx = SN_X + SN_SIZE * SN_CELL + 6;
  display.drawRect(SN_X - 1, SN_Y - 1, SN_SIZE * SN_CELL + 2, SN_SIZE * SN_CELL + 2, SSD1306_WHITE);
  display.fillRect(SN_X + snAppleX * SN_CELL + 1, SN_Y + snAppleY * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2, SSD1306_WHITE);
  for (int i = 0; i < snLen; i++) {
    if (i == 0) display.fillRect(SN_X + snX[i] * SN_CELL, SN_Y + snY[i] * SN_CELL, SN_CELL, SN_CELL, SSD1306_WHITE);
    else display.fillRect(SN_X + snX[i] * SN_CELL + 1, SN_Y + snY[i] * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2, SSD1306_WHITE);
  }
  if (snGameOver) {
    display.setCursor(rx, 12); display.print("M=");
    display.setCursor(rx, 22); display.print("new");
    display.setCursor(rx, 36); display.print("hM=");
    display.setCursor(rx, 46); display.print("exit");
  } else {
    display.setCursor(rx, 12); display.print("L/R");
    display.setCursor(rx, 22); display.print("turn");
    display.setCursor(rx, 36); display.print("M=");
    display.setCursor(rx, 46); display.print("pause");
  }
  display.display();
}

#define G_SIZE 4
uint16_t g2048[G_SIZE][G_SIZE];
bool g2048Over = false;
bool g2048Won = false;
uint32_t g2048Score = 0;
void g2048Spawn() {
  int free = 0;
  for (int y = 0; y < G_SIZE; y++) for (int x = 0; x < G_SIZE; x++) if (g2048[y][x] == 0) free++;
  if (free == 0) return;
  int r = random(free);
  for (int y = 0; y < G_SIZE; y++) for (int x = 0; x < G_SIZE; x++) {
    if (g2048[y][x] == 0) {
      if (r == 0) { g2048[y][x] = (random(10) == 0) ? 4 : 2; return; }
      r--;
    }
  }
}
void g2048Init() {
  memset(g2048, 0, sizeof(g2048));
  g2048Score = 0; g2048Over = false; g2048Won = false;
  g2048Spawn(); g2048Spawn();
}
bool g2048Slide(int dir) {
  bool moved = false;
  for (int pass = 0; pass < G_SIZE; pass++) {
    for (int i = 0; i < G_SIZE; i++) {
      int coords[G_SIZE];
      for (int j = 0; j < G_SIZE; j++) coords[j] = j;
      if (dir == 1 || dir == 3) { for (int j = 0; j < G_SIZE/2; j++) { int t = coords[j]; coords[j] = coords[G_SIZE-1-j]; coords[G_SIZE-1-j] = t; } }
      uint16_t line[G_SIZE];
      for (int j = 0; j < G_SIZE; j++) {
        if (dir < 2) line[j] = g2048[i][coords[j]];
        else line[j] = g2048[coords[j]][i];
      }
      uint16_t nl[G_SIZE] = {0,0,0,0};
      int pos = 0;
      for (int j = 0; j < G_SIZE; j++) {
        if (line[j] == 0) continue;
        if (pos > 0 && nl[pos-1] == line[j]) { nl[pos-1] *= 2; g2048Score += nl[pos-1]; pos--; moved = true; }
        else { nl[pos++] = line[j]; }
      }
      for (int j = 0; j < G_SIZE; j++) {
        if (dir < 2) g2048[i][coords[j]] = nl[j];
        else g2048[coords[j]][i] = nl[j];
      }
    }
  }
  return moved;
}
void g2048Check() {
  bool free = false;
  for (int y = 0; y < G_SIZE; y++) for (int x = 0; x < G_SIZE; x++) if (g2048[y][x] == 0) free = true;
  if (free) return;
  for (int y = 0; y < G_SIZE; y++) for (int x = 0; x < G_SIZE; x++) {
    if (x < G_SIZE - 1 && g2048[y][x] == g2048[y][x+1]) return;
    if (y < G_SIZE - 1 && g2048[y][x] == g2048[y+1][x]) return;
  }
  g2048Over = true;
}
void g2048DrawCell(int x, int y, uint16_t v) {
  int px = x * 30 + 2, py = y * 12 + 12;
  display.drawRect(px, py, 29, 11, SSD1306_WHITE);
  if (v == 0) return;
  if (v >= 128) display.fillRect(px + 1, py + 1, 27, 9, SSD1306_WHITE);
  display.setTextColor(v >= 128 ? SSD1306_BLACK : SSD1306_WHITE);
  char b[6]; snprintf(b, sizeof(b), "%u", v);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(b, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(px + (29 - w) / 2, py + 1);
  display.print(b);
  display.setTextColor(SSD1306_WHITE);
}
void draw2048() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (g2048Over) display.print("OVER ");
  else if (g2048Won) display.print("WIN! ");
  else display.print("2048 ");
  display.print(g2048Score); display.print(" hi:"); display.print(hi2048);
  for (int y = 0; y < G_SIZE; y++) for (int x = 0; x < G_SIZE; x++)
    g2048DrawCell(x, y, g2048[y][x]);
  display.setCursor(2, 60);
  display.print(g2048Over ? "M=new hM=out" : "L/R hL/R=M=undo");
  display.display();
}

#define SIM_MAX 20
uint8_t simSeq[SIM_MAX];
int simLen = 0;
int simShowIdx = 0;
int simInputIdx = 0;
bool simShowing = false;
bool simGameOver = false;
uint32_t simLastAction = 0;
int simFlash = -1;
uint32_t simFlashUntil = 0;
void simInit() {
  simLen = 1;
  simSeq[0] = random(3);
  simShowIdx = 0; simInputIdx = 0;
  simShowing = true; simGameOver = false;
  simFlash = -1; simLastAction = millis();
}
void simStep() {
  if (simGameOver) return;
  uint32_t now = millis();
  if (simFlash >= 0 && now > simFlashUntil) simFlash = -1;
  if (simShowing) {
    if (simFlash >= 0) return;
    if (now - simLastAction < 300) return;
    simLastAction = now;
    if (simShowIdx >= simLen) { simShowing = false; simInputIdx = 0; return; }
    simFlash = simSeq[simShowIdx++];
    simFlashUntil = now + 250;
  }
}
void simPress(int idx) {
  if (simShowing || simGameOver || simFlash >= 0) return;
  if (simSeq[simInputIdx] == idx) {
    simInputIdx++;
    if (simInputIdx >= simLen) {
      if (simLen >= SIM_MAX) { simGameOver = true; return; }
      simSeq[simLen] = random(3); simLen++;
      simShowIdx = 0; simShowing = true; simInputIdx = 0;
      simLastAction = millis() - 500;
    }
  } else simGameOver = true;
}
const char* simLabels[] = { "L", "M", "R" };
void drawSimon() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (simGameOver) display.print("OVER  lvl:");
  else if (simShowing) display.print("Watch  lvl:");
  else display.print("Repeat lvl:");
  display.print(simLen); display.print(" hi:"); display.print(hiSimon);
  for (int i = 0; i < 3; i++) {
    int cx = 22 + i * 42, cy = 32;
    bool on = (simFlash == i);
    if (on) { display.fillCircle(cx, cy, 15, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else { display.drawCircle(cx, cy, 15, SSD1306_WHITE); display.setTextColor(SSD1306_WHITE); }
    display.setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(simLabels[i], 0, 0, &x1, &y1, &w, &h);
    display.setCursor(cx - w/2, cy - 6);
    display.print(simLabels[i]);
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(2, 56);
  if (simGameOver) display.print("M=new hM=out");
  else if (simShowing) display.print("Watch...");
  else display.print("L M R press");
  display.display();
}

// =========================================================
// GAMES: Tetris, Pong, Flappy, Doodle
// =========================================================
#define T_W 8
#define T_H 16
#define T_PX 3
#define T_X 4
#define T_Y 10
uint8_t tGrid[T_H][T_W];
int tCurX = 0, tCurY = 0, tCurType = 0, tCurRot = 0;
int tNextType = 0;
uint32_t tScore = 0;
bool tGameOver = false, tPaused = false;
uint32_t tLastDrop = 0;
const uint32_t T_DROP_MS = 500;
const uint8_t tShapes[7][4][4] = {
  { {0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0} },
  { {0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0} },
  { {0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },
  { {0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0} },
  { {1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0} },
  { {1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },
  { {0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },
};
bool tCanPlace(int type, int rot, int px, int py) {
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
    int sx = x, sy = y;
    for (int r = 0; r < rot; r++) { int t = sx; sx = 3 - sy; sy = t; }
    if (!tShapes[type][sy][sx]) continue;
    int gx = px + x, gy = py + y;
    if (gx < 0 || gx >= T_W || gy >= T_H) return false;
    if (gy >= 0 && tGrid[gy][gx]) return false;
  }
  return true;
}
void tLockPiece() {
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
    int sx = x, sy = y;
    for (int r = 0; r < tCurRot; r++) { int t = sx; sx = 3 - sy; sy = t; }
    if (!tShapes[tCurType][sy][sx]) continue;
    int gx = tCurX + x, gy = tCurY + y;
    if (gy >= 0 && gy < T_H && gx >= 0 && gx < T_W) tGrid[gy][gx] = tCurType + 1;
  }
  int cleared = 0;
  for (int y = T_H - 1; y >= 0; y--) {
    bool full = true;
    for (int x = 0; x < T_W; x++) if (!tGrid[y][x]) { full = false; break; }
    if (full) {
      cleared++;
      for (int yy = y; yy > 0; yy--) for (int x = 0; x < T_W; x++) tGrid[yy][x] = tGrid[yy-1][x];
      for (int x = 0; x < T_W; x++) tGrid[0][x] = 0;
      y++;
    }
  }
  if (cleared == 1) tScore += 100;
  else if (cleared == 2) tScore += 300;
  else if (cleared == 3) tScore += 500;
  else if (cleared == 4) tScore += 800;
  tCurType = tNextType;
  tNextType = random(7);
  tCurRot = 0;
  tCurX = T_W / 2 - 2;
  tCurY = 0;
  if (!tCanPlace(tCurType, tCurRot, tCurX, tCurY)) tGameOver = true;
}
void tInit() {
  memset(tGrid, 0, sizeof(tGrid));
  tScore = 0; tGameOver = false; tPaused = false;
  tCurType = random(7); tNextType = random(7);
  tCurRot = 0; tCurX = T_W / 2 - 2; tCurY = 0;
  tLastDrop = millis();
}
void tStep() {
  if (tGameOver || tPaused) return;
  if (millis() - tLastDrop < T_DROP_MS) return;
  tLastDrop = millis();
  if (tCanPlace(tCurType, tCurRot, tCurX, tCurY + 1)) tCurY++;
  else tLockPiece();
}
void drawTetris() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (tGameOver) display.print("OVER ");
  else if (tPaused) display.print("PAUSE ");
  else display.print("Tetris ");
  display.print(tScore); display.print(" hi:"); display.print(hiTetris);
  display.drawRect(T_X - 1, T_Y - 1, T_W * T_PX + 2, T_H * T_PX + 2, SSD1306_WHITE);
  for (int y = 0; y < T_H; y++) for (int x = 0; x < T_W; x++)
    if (tGrid[y][x]) display.fillRect(T_X + x * T_PX, T_Y + y * T_PX, T_PX, T_PX, SSD1306_WHITE);
  if (!tGameOver) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
      int sx = x, sy = y;
      for (int r = 0; r < tCurRot; r++) { int t = sx; sx = 3 - sy; sy = t; }
      if (!tShapes[tCurType][sy][sx]) continue;
      int gx = tCurX + x, gy = tCurY + y;
      if (gy >= 0 && gy < T_H && gx >= 0 && gx < T_W)
        display.fillRect(T_X + gx * T_PX, T_Y + gy * T_PX, T_PX, T_PX, SSD1306_WHITE);
    }
  }
  int nx = T_X + T_W * T_PX + 6;
  display.setCursor(nx, 12); display.print("Next");
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
    if (tShapes[tNextType][y][x]) display.fillRect(nx + x * 3, 22 + y * 3, 3, 3, SSD1306_WHITE);
  display.setCursor(nx, 42); display.print("L/R");
  display.setCursor(nx, 50); display.print("hL rot");
  display.setCursor(nx, 58); display.print("hM out");
  display.display();
}

int pBallX, pBallY, pBallDx, pBallDy;
int pPaddleX, pAiX;
int pScoreP, pScoreAi;
bool pPaused = false, pGameOver = false;
uint32_t pLastMove = 0;
const uint32_t P_SPEED = 40;
const int P_PW = 16;
const int P_TOP = 12;
const int P_BOT = 62;
void pResetBall(int dir) {
  pBallX = 64; pBallY = 36;
  pBallDx = dir > 0 ? 2 : -2;
  pBallDy = (random(2) ? 1 : -1);
}
void pInit() {
  pPaddleX = 64 - P_PW / 2; pAiX = 64 - P_PW / 2;
  pScoreP = 0; pScoreAi = 0;
  pGameOver = false; pPaused = false;
  pResetBall(1); pLastMove = millis();
}
void pStep() {
  if (pPaused || pGameOver) return;
  if (millis() - pLastMove < P_SPEED) return;
  pLastMove = millis();
  pBallX += pBallDx; pBallY += pBallDy;
  if (pBallX <= 1) { pBallX = 1; pBallDx = -pBallDx; }
  if (pBallX >= SW - 2) { pBallX = SW - 2; pBallDx = -pBallDx; }
  if (pBallY <= P_TOP + 1) {
    pScoreP++;
    if (pScoreP >= 5) { pGameOver = true; return; }
    pResetBall(-1); return;
  }
  if (pBallY >= P_BOT - 1) {
    pScoreAi++;
    if (pScoreAi >= 5) { pGameOver = true; return; }
    pResetBall(1); return;
  }
  if (pBallY >= P_BOT - 4 && pBallY <= P_BOT - 1 && pBallDy > 0) {
    if (pBallX >= pPaddleX && pBallX <= pPaddleX + P_PW) {
      pBallDy = -pBallDy;
      int off = pBallX - (pPaddleX + P_PW / 2);
      pBallDx = off / 3; if (pBallDx == 0) pBallDx = 1;
    }
  }
  if (pBallY <= P_TOP + 4 && pBallY >= P_TOP && pBallDy < 0) {
    if (pBallX >= pAiX && pBallX <= pAiX + P_PW) {
      pBallDy = -pBallDy;
      int off = pBallX - (pAiX + P_PW / 2);
      pBallDx = off / 3; if (pBallDx == 0) pBallDx = 1;
    }
  }
  int target = pBallX - P_PW / 2;
  if (pAiX < target - 2) pAiX += 2;
  else if (pAiX > target + 2) pAiX -= 2;
  if (pAiX < 0) pAiX = 0;
  if (pAiX > SW - P_PW) pAiX = SW - P_PW;
}
// v24.3: Pong без наложений — футер убран, статус в шапку
void drawPong() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);

  display.setCursor(2, 0);
  if (pGameOver) {
    display.print(pScoreP > pScoreAi ? "WIN " : "LOSE ");
    display.print(pScoreP); display.print(":"); display.print(pScoreAi);
    display.setCursor(SW - 36, 0);
    display.print("M=new");
  } else {
    display.print("Pong ");
    display.print(pScoreP); display.print(":"); display.print(pScoreAi);
    display.setCursor(SW - 36, 0);
    display.print("L/R hM");
  }
  display.drawLine(0, 9, SW - 1, 9, SSD1306_WHITE);

  for (int y = P_TOP + 2; y < P_BOT - 4; y += 4) display.drawPixel(64, y, SSD1306_WHITE);

  display.fillRect(pAiX, P_TOP, P_PW, 3, SSD1306_WHITE);
  display.fillRect(pPaddleX, P_BOT - 3, P_PW, 3, SSD1306_WHITE);
  display.fillRect(pBallX, pBallY, 3, 3, SSD1306_WHITE);

  display.display();
}

int fBirdY, fVel, fPipeX, fGapY, fScore;
bool fGameOver;
uint32_t fLastMove;
const uint32_t F_SPEED = 60;
const int F_PIPE_GAP = 24;    // v24.2: 20 → 24
const int F_BIRD_X = 20;
const int F_TOP = 12;
const int F_BOT = 62;
void fResetPipe() {
  fPipeX = SW;
  fGapY = F_TOP + 6 + random(F_BOT - F_TOP - F_PIPE_GAP - 12);
}
void fInit() {
  fBirdY = 36; fVel = 0; fScore = 0; fGameOver = false;
  fResetPipe(); fLastMove = millis();
}
void fStep() {
  if (fGameOver) return;
  if (millis() - fLastMove < F_SPEED) return;
  fLastMove = millis();
  fVel++; if (fVel > 3) fVel = 3;
  fBirdY += fVel;
  fPipeX -= 2;
  if (fPipeX < -12) { fPipeX = SW; fGapY = F_TOP + 6 + random(F_BOT - F_TOP - F_PIPE_GAP - 12); fScore++; }
  if (fBirdY < F_TOP || fBirdY > F_BOT - 3) fGameOver = true;
  // v24.2: коллизия по всему перекрытию (труба 8px + bird 4px)
  if (fPipeX + 8 > F_BIRD_X && fPipeX < F_BIRD_X + 4) {
    if (fBirdY < fGapY || fBirdY + 3 > fGapY + F_PIPE_GAP) fGameOver = true;
  }
}
void fFlap() { if (!fGameOver) fVel = -7; }  // v24.2: -3 → -7
void drawFlappy() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (fGameOver) display.print("OVER ");
  else display.print("Flappy ");
  display.print(fScore); display.print(" hi:"); display.print(hiFlappy);
  display.fillRect(fPipeX, F_TOP, 8, fGapY - F_TOP, SSD1306_WHITE);
  display.fillRect(fPipeX, fGapY + F_PIPE_GAP, 8, F_BOT - fGapY - F_PIPE_GAP, SSD1306_WHITE);
  display.fillRect(F_BIRD_X, fBirdY, 4, 3, SSD1306_WHITE);
  display.setCursor(2, 56);
  if (fGameOver) display.print("M=new hM=out");
  else display.print("L/R=flap hM=out");  // v24.2
  display.display();
}

struct DoodPlat { int x, y; };
#define DOOD_NP 8
DoodPlat dPlats[DOOD_NP];
int dBirdX, dBirdY, dVel, dCam;
bool dGameOver;
uint32_t dLastMove;
const uint32_t D_SPEED = 40;
// v24.2: старт повыше + платформы ниже
void doodInit() {
  dBirdX = 64; dBirdY = 45; dVel = 0; dCam = 0; dGameOver = false;
  for (int i = 0; i < DOOD_NP; i++) { dPlats[i].x = random(SW - 16); dPlats[i].y = 62 - i * 10; }
  dLastMove = millis();
}
void doodStep() {
  if (dGameOver) return;
  if (millis() - dLastMove < D_SPEED) return;
  dLastMove = millis();
  // v24.2: dVel>4, зона +4, прыжок -7
  dVel++; if (dVel > 4) dVel = 4;
  dBirdY += dVel;
  if (dVel > 0) {
    for (int i = 0; i < DOOD_NP; i++) {
      if (dBirdY + 3 >= dPlats[i].y && dBirdY + 3 <= dPlats[i].y + 4
          && dBirdX + 3 >= dPlats[i].x && dBirdX <= dPlats[i].x + 15) { dVel = -7; break; }
    }
  }
  if (dBirdY < 25) {
    int shift = 25 - dBirdY;
    dBirdY = 25;
    for (int i = 0; i < DOOD_NP; i++) dPlats[i].y += shift;
    dCam += shift;
  }
  for (int i = 0; i < DOOD_NP; i++) {
    if (dPlats[i].y > F_BOT + 5) { dPlats[i].y = F_TOP - 5; dPlats[i].x = random(SW - 16); }
  }
  if (dBirdY > F_BOT + 3) dGameOver = true;
  if (dBirdX < 0) dBirdX = 0;
  if (dBirdX > SW - 4) dBirdX = SW - 4;
}
void drawDoodle() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (dGameOver) display.print("OVER ");
  else display.print("Doodle ");
  display.print(dCam / 10); display.print("m hi:"); display.print(hiDoodle);
  for (int i = 0; i < DOOD_NP; i++) display.fillRect(dPlats[i].x, dPlats[i].y, 16, 2, SSD1306_WHITE);
  display.fillRect(dBirdX, dBirdY, 4, 4, SSD1306_WHITE);
  display.setCursor(2, 56);
  if (dGameOver) display.print("M=new hM=out");
  else display.print("L/R move hM=out");
  display.display();
}

// =========================================================
// GAMES: Blackjack, BJ Multi, Farm
// =========================================================
struct BJack { uint8_t cards[10]; int nCards; int total; bool done, bust, stand; };
BJack bjPlayer[3];
BJack bjDealer;
int bjHands = 1;
int bjHand = 0;
bool bjGameOver = false, bjWin = false;
uint32_t bjSessionWin = 0;
int bjDrawCard() {
  int c = random(1, 14);
  if (c == 1) return 11;
  if (c > 10) return 10;
  return c;
}
int bjSum(BJack &h) {
  int s = 0, aces = 0;
  for (int i = 0; i < h.nCards; i++) { s += h.cards[i]; if (h.cards[i] == 11) aces++; }
  while (s > 21 && aces > 0) { s -= 10; aces--; }
  return s;
}
void bjInit(int hands) {
  bjHands = hands; bjHand = 0; bjGameOver = false; bjWin = false;
  for (int i = 0; i < 3; i++) { bjPlayer[i].nCards = 0; bjPlayer[i].bust = false; bjPlayer[i].stand = false; bjPlayer[i].done = false; }
  bjDealer.nCards = 0;
  for (int i = 0; i < bjHands; i++) {
    bjPlayer[i].cards[bjPlayer[i].nCards++] = bjDrawCard();
    bjPlayer[i].cards[bjPlayer[i].nCards++] = bjDrawCard();
    bjPlayer[i].total = bjSum(bjPlayer[i]);
  }
  bjDealer.cards[bjDealer.nCards++] = bjDrawCard();
  bjDealer.cards[bjDealer.nCards++] = bjDrawCard();
  bjDealer.total = bjSum(bjDealer);
}
void bjHit(int hand) {
  BJack &h = bjPlayer[hand];
  if (h.stand || h.bust || h.nCards >= 10) return;
  h.cards[h.nCards++] = bjDrawCard();
  h.total = bjSum(h);
  if (h.total > 21) h.bust = true;
  else if (h.total == 21) h.stand = true;
}
void bjStand(int hand) { bjPlayer[hand].stand = true; }
void bjFinish() {
  bool allDone = true;
  for (int i = 0; i < bjHands; i++) if (!bjPlayer[i].stand && !bjPlayer[i].bust) allDone = false;
  if (!allDone) return;
  while (bjDealer.total < 17 && bjDealer.nCards < 10) {
    bjDealer.cards[bjDealer.nCards++] = bjDrawCard();
    bjDealer.total = bjSum(bjDealer);
  }
  bool anyWin = false;
  for (int i = 0; i < bjHands; i++) {
    if (bjPlayer[i].bust) continue;
    if (bjDealer.total > 21 || bjPlayer[i].total > bjDealer.total) anyWin = true;
  }
  bjWin = anyWin;
  if (anyWin) { bjSessionWin++; if ((int)bjSessionWin > hiBJ) { hiBJ = bjSessionWin; saveHighScore("bj", hiBJ); } }
  bjGameOver = true;
}
void drawBJ() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  if (bjGameOver) display.print(bjWin ? "WIN!" : "LOSE");
  else display.print(bjHands == 1 ? "Blackjack" : "BJ Multi");
  display.print("  hi:"); display.print(hiBJ);
  display.setCursor(2, 10);
  display.print("Dealer:"); display.print(bjDealer.total);
  int sy = 20;
  for (int i = 0; i < bjHands; i++) {
    BJack &h = bjPlayer[i];
    int y = sy + i * 12;
    bool sel = (i == bjHand && !bjGameOver);
    if (sel) display.fillRect(0, y - 1, SW, 10, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(2, y);
    display.print("H"); display.print(i+1); display.print(": ");
    for (int j = 0; j < h.nCards && j < 6; j++) {
      int v = h.cards[j];
      if (v == 11) display.print("A");
      else if (v == 10) display.print("X");
      else display.print(v);
    }
    display.print("="); display.print(h.total);
    if (h.bust) display.print(" X");
    else if (h.stand) display.print(" S");
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 56);
  if (bjGameOver) display.print("M=new hM=out");
  else if (bjHands == 1) display.print("L=hit R=stand hM=out");
  else display.print("L/R sel M=hit hM=std");
  display.display();
}

#define FARM_PLOTS 4
#define FARM_GROW_MS 20000
uint32_t farmPlanted[FARM_PLOTS];
uint32_t farmCash = 0;
int farmUpg = 0;
int farmSel = 0;
bool farmStarted = false;
void farmInit() {
  memset(farmPlanted, 0, sizeof(farmPlanted));
  farmCash = 0; farmUpg = 0; farmSel = 0; farmStarted = true;
}
uint32_t farmGrowMs() { return FARM_GROW_MS / (1 + farmUpg); }
void farmAction() {
  uint32_t now = millis();
  int i = farmSel;
  if (farmPlanted[i] == 0) farmPlanted[i] = now;
  else if (now - farmPlanted[i] > farmGrowMs()) {
    farmPlanted[i] = 0;
    farmCash += 10 + farmUpg * 5;
    if (farmCash > hiFarm) { hiFarm = farmCash; saveHighScoreU("farm", hiFarm); }
  }
}
void farmUpgrade() {
  uint32_t cost = 100 + farmUpg * 100;
  if (farmCash >= cost) { farmCash -= cost; farmUpg++; }
}
// v24.3: 2×2 сетка, спрайт растения, прогресс-бар, READY мигает
void drawFarm() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);

  display.setCursor(2, 0);
  display.print("$"); display.print(farmCash);
  uint32_t cost = 100 + farmUpg * 100;
  char upg[20];
  snprintf(upg, sizeof(upg), "Up%d $%lu", farmUpg, cost);
  display.setCursor(SW - strlen(upg) * 6 - 2, 0);
  display.print(upg);

  display.drawLine(0, 9, SW - 1, 9, SSD1306_WHITE);

  uint32_t now = millis();
  uint32_t growMs = farmGrowMs();

  for (int i = 0; i < FARM_PLOTS; i++) {
    int col = i % 2, row = i / 2;
    int px = 3 + col * 62;
    int py = 12 + row * 20;
    int pw = 58, ph = 16;

    bool sel   = (i == farmSel);
    bool ready = (farmPlanted[i] != 0) && (now - farmPlanted[i] >= growMs);

    if (sel) display.drawRect(px - 2, py - 2, pw + 4, ph + 4, SSD1306_WHITE);
    display.drawRect(px, py, pw, ph, SSD1306_WHITE);

    if (farmPlanted[i] == 0) {
      for (int x = px + 4; x < px + pw - 4; x += 6)
        display.drawPixel(x, py + ph / 2, SSD1306_WHITE);
      display.setCursor(px + 16, py + 4);
      display.print("empty");
    } else if (ready) {
      bool flash = (millis() / 300) % 2;
      if (flash) display.fillRect(px + 1, py + 1, pw - 2, ph - 2, SSD1306_WHITE);
      display.setTextColor(flash ? SSD1306_BLACK : SSD1306_WHITE);
      display.setCursor(px + 14, py + 4);
      display.print("READY");
      display.setTextColor(SSD1306_WHITE);
    } else {
      uint32_t elapsed = now - farmPlanted[i];
      int stage = (elapsed * 4) / growMs;
      if (stage > 3) stage = 3;

      int sx = px + pw / 2;
      int sy = py + ph - 2;
      int height = 2 + stage * 2;

      display.drawLine(sx, sy, sx, sy - height, SSD1306_WHITE);
      display.drawPixel(sx - 2, sy - height / 2, SSD1306_WHITE);
      display.drawPixel(sx + 2, sy - height / 2, SSD1306_WHITE);
      if (stage >= 2) {
        display.drawPixel(sx - 3, sy - height + 1, SSD1306_WHITE);
        display.drawPixel(sx + 3, sy - height + 1, SSD1306_WHITE);
      }
      if (stage >= 3) {
        display.fillCircle(sx, sy - height - 2, 1, SSD1306_WHITE);
        display.drawPixel(sx - 1, sy - height - 1, SSD1306_WHITE);
        display.drawPixel(sx + 1, sy - height - 1, SSD1306_WHITE);
      }

      int prog = (elapsed * (pw - 4)) / growMs;
      if (prog > pw - 4) prog = pw - 4;
      display.fillRect(px + 2, py + ph - 2, prog, 2, SSD1306_WHITE);
    }
  }

  display.setCursor(2, 56);
  display.print("M=act hL=upg hM=out");

  display.display();
}

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
  // v24.1: Games плоский (10 пунктов)
  else if (currentMenuIdx == 7) {
    btnLeft.fireOnPress = false; btnRight.fireOnPress = false;
    if (idx == 0) { snInit(); currentScreen = SCR_SNAKE; drawSnake(); }
    if (idx == 1) { pInit(); currentScreen = SCR_PONG; drawPong(); }
    if (idx == 2) { fInit(); currentScreen = SCR_FLAPPY; drawFlappy(); }
    if (idx == 3) { doodInit(); currentScreen = SCR_DOODLE; drawDoodle(); }
    if (idx == 4) { msInit(); currentScreen = SCR_MINES; drawMines(); }
    if (idx == 5) { g2048Init(); currentScreen = SCR_2048; draw2048(); }
    if (idx == 6) { simInit(); currentScreen = SCR_SIMON; drawSimon(); }
    if (idx == 7) { tInit(); currentScreen = SCR_TETRIS; drawTetris(); }
    if (idx == 8) { bjInit(1); currentScreen = SCR_BJ; drawBJ(); }
    if (idx == 9) { if (!farmStarted) farmInit(); currentScreen = SCR_FARM; drawFarm(); }
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
  // --- TETRIS ---
  if (currentScreen == SCR_TETRIS) {
    if (tGameOver) {
      if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if ((int)tScore > hiTetris) { hiTetris = tScore; saveHighScore("tetris", hiTetris); } tInit(); drawTetris(); }
      return;
    }
    if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { tPaused = !tPaused; drawTetris(); return; }
    if (takeShort(btnLeft))  { if (tCanPlace(tCurType, tCurRot, tCurX - 1, tCurY)) { tCurX--; drawTetris(); } return; }
    if (takeShort(btnRight)) { if (tCanPlace(tCurType, tCurRot, tCurX + 1, tCurY)) { tCurX++; drawTetris(); } return; }
    if (takeLong(btnLeft)) { int nr = (tCurRot + 1) % 4; if (tCanPlace(tCurType, nr, tCurX, tCurY)) { tCurRot = nr; drawTetris(); } return; }
    if (takeLong(btnRight)) { tLastDrop = millis() - T_DROP_MS; return; }
    tStep();
    if (tGameOver && (int)tScore > hiTetris) { hiTetris = tScore; saveHighScore("tetris", hiTetris); }
    static uint32_t ltd = 0;
    if (millis() - ltd > 100) { ltd = millis(); drawTetris(); }
    return;
  }
  // --- PONG ---
  if (currentScreen == SCR_PONG) {
    if (pGameOver) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { pInit(); drawPong(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft))  { pPaddleX = max(0, pPaddleX - 8); drawPong(); return; }
    if (takeShort(btnRight)) { pPaddleX = min(SW - P_PW, pPaddleX + 8); drawPong(); return; }
    pStep();
    static uint32_t ltp = 0;
    if (millis() - ltp > 50) { ltp = millis(); drawPong(); }
    return;
  }
  // --- FLAPPY ---
  if (currentScreen == SCR_FLAPPY) {
    if (fGameOver) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if (fScore > hiFlappy) { hiFlappy = fScore; saveHighScore("flappy", hiFlappy); } fInit(); drawFlappy(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    // v24.2: flap на L/R, M свободен (только restart после game over)
    if (takeShort(btnLeft) || takeShort(btnRight)) { fFlap(); drawFlappy(); return; }
    fStep();
    if (fGameOver && fScore > hiFlappy) { hiFlappy = fScore; saveHighScore("flappy", hiFlappy); }
    static uint32_t ltf = 0;
    if (millis() - ltf > 50) { ltf = millis(); drawFlappy(); }
    return;
  }
  // --- DOODLE ---
  if (currentScreen == SCR_DOODLE) {
    if (dGameOver) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if (dCam / 10 > hiDoodle) { hiDoodle = dCam / 10; saveHighScore("doodle", hiDoodle); } doodInit(); drawDoodle(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft))  { dBirdX -= 5; if (dBirdX < 0) dBirdX = 0; return; }
    if (takeShort(btnRight)) { dBirdX += 5; if (dBirdX > SW - 4) dBirdX = SW - 4; return; }
    doodStep();
    if (dGameOver && dCam / 10 > hiDoodle) { hiDoodle = dCam / 10; saveHighScore("doodle", hiDoodle); }
    static uint32_t ltdd = 0;
    if (millis() - ltdd > 50) { ltdd = millis(); drawDoodle(); }
    return;
  }
  // --- BLACKJACK ---
  if (currentScreen == SCR_BJ || currentScreen == SCR_BJM) {
    if (bjGameOver) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { bjInit(bjHands); drawBJ(); }
      return;
    }
    if (takeLong(btnSelect)) { if (bjHands == 1) bjStand(0); else bjStand(bjHand); bjFinish(); drawBJ(); return; }
    if (bjHands > 1) {
      if (takeShort(btnLeft) && bjHand > 0) { bjHand--; drawBJ(); return; }
      if (takeShort(btnRight) && bjHand < bjHands - 1) { bjHand++; drawBJ(); return; }
      if (takeShort(btnSelect)) { bjHit(bjHand); bjFinish(); drawBJ(); return; }
      return;
    }
    if (takeShort(btnLeft))  { bjHit(0); bjFinish(); drawBJ(); return; }
    if (takeShort(btnRight)) { bjStand(0); bjFinish(); drawBJ(); return; }
    return;
  }
  // --- FARM ---
  if (currentScreen == SCR_FARM) {
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnLeft) && farmSel > 0) { farmSel--; drawFarm(); return; }
    if (takeShort(btnRight) && farmSel < FARM_PLOTS - 1) { farmSel++; drawFarm(); return; }
    if (takeShort(btnSelect)) { farmAction(); drawFarm(); return; }
    if (takeLong(btnLeft)) { farmUpgrade(); drawFarm(); return; }
    static uint32_t ltfarm = 0;
    if (millis() - ltfarm > 500) { ltfarm = millis(); drawFarm(); }
    return;
  }
  // --- SNAKE ---
  if (currentScreen == SCR_SNAKE) {
    if (snGameOver) {
      if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if (snLen > hiSnake) { hiSnake = snLen; saveHighScore("snake", hiSnake); } snInit(); drawSnake(); }
      return;
    }
    if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (takeShort(btnSelect)) { snPaused = !snPaused; drawSnake(); return; }
    if (takeShort(btnLeft) && !snPaused)  { snDir = (snDir + 3) % 4; return; }
    if (takeShort(btnRight) && !snPaused) { snDir = (snDir + 1) % 4; return; }
    snStep();
    if (snGameOver && snLen > hiSnake) { hiSnake = snLen; saveHighScore("snake", hiSnake); }
    static uint32_t lts = 0;
    if (millis() - lts > 100) { lts = millis(); drawSnake(); }
    return;
  }
  // --- 2048 ---
  if (currentScreen == SCR_2048) {
    if (g2048Over || g2048Won) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if ((int)g2048Score > hi2048) { hi2048 = g2048Score; saveHighScore("g2048", hi2048); } g2048Init(); draw2048(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    int dir = -1;
    if (takeShort(btnLeft))  dir = 0;
    if (takeShort(btnRight)) dir = 1;
    if (takeLong(btnLeft))   dir = 2;
    if (takeLong(btnRight))  dir = 3;
    if (dir >= 0) {
      if (g2048Slide(dir)) { g2048Spawn(); g2048Check(); }
      if ((int)g2048Score > hi2048) { hi2048 = g2048Score; saveHighScore("g2048", hi2048); }
      draw2048(); return;
    }
    return;
  }
  // --- SIMON ---
  if (currentScreen == SCR_SIMON) {
    if (simGameOver) {
      if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { if (simLen > hiSimon) { hiSimon = simLen; saveHighScore("simon", hiSimon); } simInit(); drawSimon(); }
      return;
    }
    if (takeLong(btnSelect)) { currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
    if (!simShowing && simFlash < 0) {
      if (takeShort(btnLeft))  simPress(0);
      if (takeShort(btnSelect)) simPress(1);
      if (takeShort(btnRight)) simPress(2);
    }
    simStep();
    if (simGameOver && simLen > hiSimon) { hiSimon = simLen; saveHighScore("simon", hiSimon); }
    static uint32_t ltsim = 0;
    if (millis() - ltsim > 60) { ltsim = millis(); drawSimon(); }
    return;
  }
  // --- MINES ---
  if (currentScreen == SCR_MINES) {
    if (msGameOver || msWon) {
      if (takeLong(btnSelect)) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      if (takeShort(btnSelect)) { msInit(); drawMines(); }
      return;
    }
    int x = msCurIdx % MS_SIZE, y = msCurIdx / MS_SIZE;
    if (takeShort(btnLeft))  { x = (x + MS_SIZE - 1) % MS_SIZE; msCurIdx = y * MS_SIZE + x; drawMines(); return; }
    if (takeShort(btnRight)) { x = (x + 1) % MS_SIZE; msCurIdx = y * MS_SIZE + x; drawMines(); return; }
    if (takeLong(btnLeft))   { y = (y + MS_SIZE - 1) % MS_SIZE; msCurIdx = y * MS_SIZE + x; drawMines(); return; }
    if (takeLong(btnRight))  { y = (y + 1) % MS_SIZE; msCurIdx = y * MS_SIZE + x; drawMines(); return; }
    if (takeShort(btnSelect)) {
      if (!msGrid[y][x].flag) {
        if (!msMinesPlaced) { msPlaceMines(x, y); msMinesPlaced = true; }
        msReveal(x, y);
        if (msOpened == MS_SIZE * MS_SIZE - MS_MINES) msWon = true;
      }
      drawMines(); return;
    }
    if (takeLong(btnSelect)) {
      if (msGrid[y][x].open) { btnLeft.fireOnPress = true; btnRight.fireOnPress = true; currentMenuIdx = testParentMenu; currentScreen = SCR_MENU; drawCurrentMenu(); return; }
      msGrid[y][x].flag = !msGrid[y][x].flag;
      msFlags += msGrid[y][x].flag ? 1 : -1;
      drawMines(); return;
    }
    return;
  }
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
  loadCreds(); loadBattCal(); loadMap(); loadMacros(); loadIrFavorites(); loadHighScores();
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
  static int lastMenuBatteryPct = -999;
  if (currentScreen == SCR_MENU && !animating && lastMenuBatteryPct != battPct) {
    lastMenuBatteryPct = battPct;
    drawCurrentMenu();
  }
  uint32_t sleepAfter = (battValid && battPct <= 10) ? 30000UL : DISPLAY_SLEEP_MS;
  if (currentScreen == SCR_MENU && !animating && millis() - lastBtnEventMs >= sleepAfter) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displaySleeping = true;
    delay(20);
    return;
  }
  if (animating) {
    if (!anyBtnEvent()) {
      animOffset += ANIM_STEP;
      if (animOffset >= SW) { animating = false; drawCurrentMenu(); }
      else drawAnimFrame();
      return;
    }
    animating = false;
    drawCurrentMenu();
  }
  if (currentScreen == SCR_MENU) handleMenuScreen();
  else handleTestScreen();
}