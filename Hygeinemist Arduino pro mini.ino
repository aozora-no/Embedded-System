// Water monitoring: Arduino Pro Mini (ATmega328P) + ESP-01 + microSD
//
// Records go to WATER.CSV while offline. When the ESP-01 reports Wi-Fi,
// WATER.CSV is renamed to PENDING.CSV and uploaded one line at a time.
// The ESP-01 answers each uploaded line with "ACK,<record id>".
// PENDING.CSV is deleted only after every line has been acknowledged.
//
// Arduino -> ESP-01 : DATA,<record>\n          (record starts with its ID)
// ESP-01 -> Arduino : WIFI,1 / WIFI,0          (heartbeat, about every 1-2 s)
//                     TIME,yyyy,mm,dd,hh,mm,ss
//                     ACK,<record id>          (after Firebase upload succeeds)
//
// Use the record ID as the Firebase key (PUT /records/<id>) so a re-sent
// record overwrites itself instead of creating a duplicate.

#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <VL53L0X.h>
#include <HX711.h>
#include <SoftwareSerial.h>

// ---------- Settings ----------
#define DEVICE_ID    "WM001"
#define SAMPLE_MS    1000UL    // time between readings
#define ACK_TIMEOUT  3000UL    // resend a pending line if no ACK in this time
#define SAVE_EVERY   20        // save the ID counter to SD every N records
#define HEADER "ID,Date,Time,ToF_mm,HX711,Float,Water1,Water2,Relay"

// ---------- Pins ----------
#define SD_CS      10
#define HX_DT      2
#define HX_SCK     3
#define FLOAT_PIN  4
#define WATER1_PIN 5
#define WATER2_PIN 6
#define RELAY_PIN  7
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ---------- Files ----------
#define WATER_FILE   "WATER.CSV"
#define PENDING_FILE "PENDING.CSV"
#define ID_FILE      "ID.TXT"

SoftwareSerial espSerial(2, 3);   // RX, TX  (ESP-01 is 3.3 V: level-shift if Pro Mini is 5 V)
SdFat32 sd;
VL53L0X tof;
HX711 scale;

// ---------- State ----------
bool sdOK, tofOK, hxOK;
bool wifiState, espAvailable;
unsigned long lastHeartbeat, lastSample;

// Record ID counter
unsigned long counter, savedCounter;

// Local storage state
bool waterHasData;       // WATER.CSV holds at least one record
bool hasPending;         // PENDING.CSV exists
unsigned long pendingPos;   // file offset of the next line to confirm
unsigned long nextPos;      // file offset after the line currently waiting for ACK
bool waiting;               // a pending line was sent and no ACK yet
unsigned long sentAt;
char ackId[16];

// Clock (set by the ESP-01)
bool timeOK;
uint16_t yr;
byte mon, dy, hr, mn, sc;
unsigned long lastTick;

// ESP receive buffer
char rx[40];
byte rxLen;

// =====================================================
// CLOCK
// =====================================================
byte monthDays(byte m, uint16_t y) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
  return 31;
}

void updateClock() {
  if (!timeOK) return;
  while (millis() - lastTick >= 1000) {
    lastTick += 1000;
    if (++sc < 60) continue;
    sc = 0;
    if (++mn < 60) continue;
    mn = 0;
    if (++hr < 24) continue;
    hr = 0;
    if (++dy <= monthDays(mon, yr)) continue;
    dy = 1;
    if (++mon > 12) { mon = 1; yr++; }
  }
}

// =====================================================
// ESP-01 MESSAGES
// =====================================================
void handleLine() {
  if (strncmp(rx, "WIFI,", 5) == 0) {
    wifiState = (rx[5] == '1');
    lastHeartbeat = millis();
  }
  else if (strncmp(rx, "TIME,", 5) == 0) {
    int y, mo, d, h, mi, s;
    if (sscanf(rx + 5, "%d,%d,%d,%d,%d,%d", &y, &mo, &d, &h, &mi, &s) == 6 &&
        y >= 2020 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
        h >= 0 && h < 24 && mi >= 0 && mi < 60 && s >= 0 && s < 60) {
      yr = y; mon = mo; dy = d; hr = h; mn = mi; sc = s;
      lastTick = millis();
      timeOK = true;
    }
  }
  else if (strncmp(rx, "ACK,", 4) == 0) {
    if (waiting && strcmp(rx + 4, ackId) == 0) {
      pendingPos = nextPos;     // this line is safely in Firebase
      waiting = false;
    }
  }
  else if (strncmp(rx, "IP,", 3) == 0) {
  Serial.print(F("ESP IP: "));
  Serial.println(rx + 3);
  }
}

void receiveESP() {
  while (espSerial.available()) {
    char c = espSerial.read();
    if (c == '\n') {
      rx[rxLen] = '\0';
      handleLine();
      rxLen = 0;
    } else if (c != '\r' && rxLen < sizeof(rx) - 1) {
      rx[rxLen++] = c;
    }
  }
  espAvailable = wifiState && (millis() - lastHeartbeat < 5000);
}

// =====================================================
// SD HELPERS
// =====================================================
// Make sure WATER.CSV exists with a header, and note if it already has data.
bool startWater() {
  File32 f = sd.open(WATER_FILE, O_WRITE | O_CREAT | O_AT_END);
  if (!f) return false;
  if (f.fileSize() == 0) f.println(F(HEADER));
  waterHasData = f.fileSize() > sizeof(HEADER) + 1;   // header + CRLF
  f.close();
  return true;
}

void appendWater(const char *line) {
  File32 f = sd.open(WATER_FILE, O_WRITE | O_CREAT | O_AT_END);
  if (f) {
    f.println(line);
    f.close();
    waterHasData = true;
  }
}

void loadCounter() {
  File32 f = sd.open(ID_FILE, O_RDONLY);
  if (f) {
    char b[12];
    int n = f.read(b, sizeof(b) - 1);
    if (n > 0) {
      b[n] = '\0';
      counter = strtoul(b, NULL, 10);
    }
    f.close();
  }
  savedCounter = counter;
}

// Saves a value ahead of the counter, so IDs never repeat after a reboot
// (a reboot may skip a few numbers, which is harmless).
void saveCounter() {
  savedCounter = counter + SAVE_EVERY;
  File32 f = sd.open(ID_FILE, O_WRITE | O_CREAT | O_TRUNC);
  if (f) {
    f.print(savedCounter);
    f.close();
  }
}

// =====================================================
// UPLOAD PENDING.CSV  (one line per call, never blocks)
// =====================================================
void syncPending() {
  if (!espAvailable || !sdOK) return;

  // Start a new upload: only when no PENDING.CSV is waiting
  if (!hasPending) {
    if (!waterHasData) return;
    if (!sd.rename(WATER_FILE, PENDING_FILE)) return;
    hasPending = true;
    pendingPos = 0;
    waiting = false;
    startWater();               // fresh WATER.CSV for new readings
  }

  // Wait for ACK, resend after timeout
  if (waiting) {
    if (millis() - sentAt < ACK_TIMEOUT) return;
    waiting = false;
  }

  File32 f = sd.open(PENDING_FILE, O_RDONLY);
  if (!f) return;
  f.seekSet(pendingPos);

  char line[80];
  while (true) {
    int n = f.fgets(line, sizeof(line));
    if (n <= 0) {                       // end of file: everything is confirmed
      f.close();
      if (sd.remove(PENDING_FILE)) hasPending = false;
      return;
    }
    if (strncmp(line, DEVICE_ID, sizeof(DEVICE_ID) - 1) == 0) break;
    pendingPos = f.curPosition();       // skip header or blank lines
  }
  nextPos = f.curPosition();
  f.close();

  line[strcspn(line, "\r\n")] = '\0';

  // Remember the ID we expect in the ACK
  byte i = 0;
  while (line[i] && line[i] != ',' && i < sizeof(ackId) - 1) {
    ackId[i] = line[i];
    i++;
  }
  ackId[i] = '\0';

  espSerial.print(F("DATA,"));
  espSerial.println(line);
  waiting = true;
  sentAt = millis();
}

// =====================================================
// READ SENSORS AND STORE / SEND ONE RECORD
// =====================================================
void takeReading() {
  // ToF
  int tofValue = -1;
  if (tofOK) {
    uint16_t d = tof.readRangeContinuousMillimeters();
    if (!tof.timeoutOccurred() && d != 65535) tofValue = d;
  }

  // HX711
  long hxValue = -1;
  if (hxOK && scale.is_ready()) hxValue = (long)scale.get_value(3);

  // Digital inputs
  byte flt = digitalRead(FLOAT_PIN) == LOW;
  byte w1  = digitalRead(WATER1_PIN) == LOW;
  byte w2  = digitalRead(WATER2_PIN) == LOW;
  byte rly = w1 || w2;
  digitalWrite(RELAY_PIN, rly ? RELAY_ON : RELAY_OFF);

  // Date and time
  char date[12], tm[10];
  if (timeOK) {
    snprintf(date, sizeof(date), "%04u-%02u-%02u", yr, mon, dy);
    snprintf(tm, sizeof(tm), "%02u:%02u:%02u", hr, mn, sc);
  } else {
    strcpy(date, "NO_DATE");
    strcpy(tm, "NO_TIME");
  }

  // Record with unique ID: WM001-00000001,...
  counter++;
  if (sdOK && counter >= savedCounter) saveCounter();

  char rec[80];
  snprintf(rec, sizeof(rec), "%s-%08lu,%s,%s,%d,%ld,%u,%u,%u,%u",
           DEVICE_ID, counter, date, tm, tofValue, hxValue, flt, w1, w2, rly);

  if (espAvailable) {
    espSerial.print(F("DATA,"));
    espSerial.println(rec);
  } else if (sdOK) {
    appendWater(rec);
  }

  Serial.print(espAvailable ? F("ON  ") : F("OFF "));
  Serial.println(rec);
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  Wire.begin();

  pinMode(SS, OUTPUT);
  digitalWrite(SS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  pinMode(FLOAT_PIN, INPUT_PULLUP);
  pinMode(WATER1_PIN, INPUT);
  pinMode(WATER2_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  Serial.println(F("WATER MONITORING SYSTEM"));

  // ToF
  tof.setTimeout(500);
  tofOK = tof.init();
  if (tofOK) tof.startContinuous(100);
  Serial.println(tofOK ? F("TOF OK") : F("TOF ERROR"));

  // HX711
  scale.begin(HX_DT, HX_SCK);
  unsigned long t = millis();
  while (!scale.is_ready() && millis() - t < 2000) delay(10);
  hxOK = scale.is_ready();
  if (hxOK) scale.tare();
  Serial.println(hxOK ? F("HX711 OK") : F("HX711 ERROR"));

  // SD card
  sdOK = sd.begin(SD_CS, SD_SCK_MHZ(1)) && startWater();
  if (sdOK) {
    hasPending = sd.exists(PENDING_FILE);   // upload left over from before a reset
    loadCounter();
  }
  Serial.println(sdOK ? F("SD OK") : F("SD ERROR"));
  Serial.println(F("SYSTEM READY"));
}

// =====================================================
// LOOP  (no delay(): the ESP serial is read continuously)
// =====================================================
void loop() {
  receiveESP();
  updateClock();
  syncPending();

  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample = millis();
    takeReading();
  }
}
