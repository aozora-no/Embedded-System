
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SoftwareSerial.h>
#include <VL53L0X.h>
#include <HX711.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// ==========================================
// PIN CONFIGURATION
// ==========================================

const uint8_t PIN_WAKE = 2;
const uint8_t PIN_HX_DT = A0;
const uint8_t PIN_HX_SCK = A1;

const uint8_t PIN_NONCONTACT1 = 5;
const uint8_t PIN_NONCONTACT2 = 6;

const uint8_t PIN_PUMP = 7;

const uint8_t PIN_ESP_RX = 8;
const uint8_t PIN_ESP_TX = 9;

const uint8_t PIN_SD_CS = 10;

// ToF VL53L0X:
// SDA = A4
// SCL = A5

// ==========================================
// SYSTEM SETTINGS
// ==========================================

const unsigned long LIVE_INTERVAL_MS = 2000UL;
const unsigned long LOG_INTERVAL_MS = 180000UL;
const unsigned long SD_CHECK_MS = 10000UL;
const unsigned long TOF_READ_INTERVAL_MS = 200UL;

const unsigned long PUMP_DURATION_MS = 3000UL;
const unsigned long MAINT_PUMP_MAX_MS = 30000UL;

// ==========================================
// SENSOR SETTINGS
// ==========================================

const float CAL_FACTOR = 420.0;

const float MAX_WEIGHT_G = 1000.0;
const float HALF_WEIGHT_G = 500.0;
const float WEIGHT_TOLERANCE_G = 5.0;

const uint16_t TOF_ZERO_MM = 20;

const bool PUMP_ACTIVE_LOW = true;
const bool NONCONTACT_ACTIVE_LOW = false;

// ==========================================
// MODES
// ==========================================

const uint8_t MODE_AUTO = 0;
const uint8_t MODE_MAINT = 1;

// ==========================================
// SD FILE
// ==========================================

// New filename prevents mixing old CSV
// columns with the updated data format.
const char LOG_FILE[] = "MONITOR.CSV";

// ==========================================
// OBJECTS
// ==========================================

VL53L0X tof;
HX711 scale;

SoftwareSerial esp(PIN_ESP_RX, PIN_ESP_TX);

// ==========================================
// SYSTEM STATE
// ==========================================

bool sdOk = false;
bool tofOk = false;
bool weightOk = false;

bool nonContact1 = false;
bool nonContact2 = false;

bool tofZero = false;

bool pumpOn = false;
bool pumpRunning = false;
bool pumpTriggered = false;

uint8_t mode = MODE_AUTO;

int distanceMm = -1;
int distanceFromZeroMm = -1;

float weightVal = 0;

unsigned long pumpStartTime = 0;
unsigned long pumpOnSince = 0;

unsigned long recordId = 0;
unsigned long firebaseCommittedId = 0;

unsigned long lastLive = 0;
unsigned long lastLog = 0;
unsigned long lastSdCheck = 0;
unsigned long lastTofRead = 0;

char espIp[16] = "unknown";

char cmdBuf[40];
uint8_t cmdLen = 0;
bool cmdOverflow = false;

// ==========================================
// PUMP CONTROL
// ==========================================

void setPump(bool on) {

  if (pumpOn == on) return;

  pumpOn = on;

  if (on) {
    pumpOnSince = millis();
  }

  digitalWrite(
    PIN_PUMP,
    PUMP_ACTIVE_LOW
      ? (on ? LOW : HIGH)
      : (on ? HIGH : LOW)
  );
}

// ==========================================
// STOP AUTOMATIC PUMP CYCLE
// ==========================================

void stopPumpCycle() {

  setPump(false);
  pumpRunning = false;
}

// ==========================================
// NON-CONTACT SENSOR READING
// ==========================================

bool isNonContactDetected(uint8_t pin) {

  return digitalRead(pin) ==
    (NONCONTACT_ACTIVE_LOW ? LOW : HIGH);
}

// ==========================================
// NON-CONTACT LEVEL INDICATOR
// ==========================================

const char* getNonContactLevel() {

  if (nonContact1 && nonContact2) {
    return "HIGH";
  }

  if (nonContact1 || nonContact2) {
    return "HALF";
  }

  return "LOW";
}

// ==========================================
// LOAD CELL WEIGHT INDICATOR
// ==========================================

const char* getWeightIndicator() {

  if (!weightOk) {
    return "ERROR";
  }

  if (weightVal < -WEIGHT_TOLERANCE_G) {
    return "NEGATIVE";
  }

  if (weightVal >= MAX_WEIGHT_G) {
    return "FULL";
  }

  if (weightVal >= HALF_WEIGHT_G) {
    return "HALF";
  }

  return "LOW";
}

// ==========================================
// READ TOF SENSOR
// ==========================================

void readTof() {

  distanceMm = -1;
  distanceFromZeroMm = -1;
  tofZero = false;

  if (!tofOk) {
    return;
  }

  uint16_t d =
    tof.readRangeContinuousMillimeters();

  if (tof.timeoutOccurred() ||
      d == 0 ||
      d >= 8190) {

    return;
  }

  distanceMm = (int)d;

  if (distanceMm <= TOF_ZERO_MM) {

    distanceFromZeroMm = 0;
    tofZero = true;

  } else {

    distanceFromZeroMm =
      distanceMm - TOF_ZERO_MM;

    tofZero = false;
  }
}

// ==========================================
// READ ALL SENSORS
// ==========================================

void readSensors() {

  readTof();

  weightOk = false;
  weightVal = 0;

  if (scale.is_ready()) {

    weightVal = scale.get_units(3);
    weightOk = true;
  }

  nonContact1 =
    isNonContactDetected(PIN_NONCONTACT1);

  nonContact2 =
    isNonContactDetected(PIN_NONCONTACT2);
}

// ==========================================
// AUTOMATIC PUMP CONTROL
// ==========================================

void updateAutoPump() {

  if (mode != MODE_AUTO) {
    return;
  }

  unsigned long now = millis();

  // Stop pump after exactly one
  // 3-second timed cycle.
  if (pumpRunning) {

    if (now - pumpStartTime >=
        PUMP_DURATION_MS) {

      stopPumpCycle();

      Serial.println(
        F("[PUMP] OFF - 3 seconds completed")
      );
    }

    return;
  }

  // Ignore invalid ToF readings.
  if (distanceMm < 0) {
    return;
  }

  // Reset trigger when distance
  // moves above 20 mm.
  if (distanceMm > TOF_ZERO_MM) {

    pumpTriggered = false;
    return;
  }

  // Start a new 3-second cycle.
  if (distanceMm <= TOF_ZERO_MM &&
      !pumpTriggered) {

    setPump(true);

    pumpRunning = true;
    pumpTriggered = true;

    pumpStartTime = now;

    Serial.println(
      F("[PUMP] ON - ToF threshold reached")
    );
  }
}

// ==========================================
// CSV RECORD VALIDATION
// ==========================================

bool csvRowId(
  const char *line,
  unsigned long &id
) {

  if (!line ||
      *line < '0' ||
      *line > '9') {

    return false;
  }

  char *endptr;

  id = strtoul(line, &endptr, 10);

  if (id == 0 || *endptr != ',') {
    return false;
  }

  uint8_t commas = 0;

  for (const char *p = line; *p; p++) {

    if (*p == ',') {
      commas++;
    }
  }

  // 10 columns = 9 commas.
  return commas == 9;
}

// ==========================================
// GET LAST VALID SD RECORD
// ==========================================

unsigned long loadLastRecordId() {

  File f = SD.open(LOG_FILE, FILE_READ);

  if (!f) return 0;

  unsigned long last = 0;

  char row[112];
  uint8_t used = 0;
  bool overflow = false;

  while (f.available()) {

    int c = f.read();

    if (c == '\n') {

      if (!overflow) {

        row[used] = '\0';

        unsigned long id;

        if (csvRowId(row, id) && id > last) {
          last = id;
        }
      }

      used = 0;
      overflow = false;

    } else if (c != '\r') {

      if (used < sizeof(row) - 1) {

        row[used++] = (char)c;

      } else {

        overflow = true;
      }
    }
  }

  if (used > 0 && !overflow) {

    row[used] = '\0';

    unsigned long id;

    if (csvRowId(row, id) && id > last) {
      last = id;
    }
  }

  f.close();

  return last;
}

// ==========================================
// INITIALIZE SD CARD
// ==========================================

bool startSdCard() {

  Serial.println(F("[SD] Initializing..."));

  pinMode(SS, OUTPUT);
  digitalWrite(SS, HIGH);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  delay(300);

  if (!SD.begin(PIN_SD_CS)) {

    Serial.println(
      F("[SD] Initialization FAILED")
    );

    return false;
  }

  Serial.println(F("[SD] Card detected"));

  if (!SD.exists(LOG_FILE)) {

    File f = SD.open(LOG_FILE, FILE_WRITE);

    if (!f) {

      Serial.println(
        F("[SD] Cannot create file")
      );

      return false;
    }

    f.println(
      F("id,uptime_s,distance_mm,weight_g,tof_zero,noncontact1,noncontact2,pump,level,weight_status")
    );

    bool ok = !f.getWriteError();
    f.close();

    if (!ok) return false;
  }

  File f = SD.open(LOG_FILE, FILE_READ);

  if (!f) return false;

  uint32_t size = f.size();
  int lastChar = '\n';

  if (size > 0) {

    if (!f.seek(size - 1)) {

      f.close();
      return false;
    }

    lastChar = f.read();
  }

  f.close();

  if (lastChar != '\n') {

    File append =
      SD.open(LOG_FILE, FILE_WRITE);

    if (!append) return false;

    append.println();

    bool ok = !append.getWriteError();
    append.close();

    if (!ok) return false;
  }

  unsigned long diskId =
    loadLastRecordId();

  if (diskId > recordId) {
    recordId = diskId;
  }

  if (firebaseCommittedId > recordId) {
    recordId = firebaseCommittedId;
  }

  Serial.print(F("[SD] Last record: "));
  Serial.println(recordId);

  return true;
}

// ==========================================
// SD CARD RECOVERY
// ==========================================

void checkSdCard() {

  if (sdOk) {

    File f = SD.open(LOG_FILE, FILE_READ);

    if (f) {

      f.close();

    } else {

      sdOk = false;

      Serial.println(
        F("[SD] File access failed")
      );
    }

  } else {

    sdOk = startSdCard();

    if (sdOk) {

      Serial.println(
        F("[SD] Reconnected")
      );
    }
  }
}

// ==========================================
// CRC16 CHECKSUM
// ==========================================

uint16_t csvCrc16(const char *data) {

  uint16_t crc = 0xFFFF;

  while (*data) {

    crc ^=
      (uint16_t)(uint8_t)(*data++) << 8;

    for (uint8_t i = 0; i < 8; i++) {

      crc = (crc & 0x8000)
        ? (uint16_t)((crc << 1) ^ 0x1021)
        : (uint16_t)(crc << 1);
    }
  }

  return crc;
}

// ==========================================
// SEND CRC-PROTECTED RECORD
// ==========================================

void sendCsvWithCrc(const char *csv) {

  char hex[5];

  snprintf(
    hex,
    sizeof(hex),
    "%04X",
    (unsigned)csvCrc16(csv)
  );

  esp.print(F("L,"));
  esp.print(csv);
  esp.print('*');
  esp.println(hex);
}

// ==========================================
// SEND REQUESTED SD RECORD
// ==========================================

void sendSdRecord(unsigned long wantedId) {

  if (!sdOk) {

    esp.println(F("ERR,SD"));
    return;
  }

  if (wantedId == 0) {

    esp.println(F("ERR,ID"));
    return;
  }

  if (wantedId > recordId) {

    esp.print(F("END,"));
    esp.println(wantedId);
    return;
  }

  File f = SD.open(LOG_FILE, FILE_READ);

  if (!f) {

    sdOk = false;
    esp.println(F("ERR,SD"));
    return;
  }

  char row[112];

  uint8_t used = 0;
  bool overflow = false;
  bool found = false;

  while (f.available()) {

    int c = f.read();

    if (c == '\n') {

      if (!overflow) {

        row[used] = '\0';

        unsigned long foundId;

        if (csvRowId(row, foundId)) {

          if (foundId == wantedId) {

            found = true;
            break;
          }

          if (foundId > wantedId) {
            break;
          }
        }
      }

      used = 0;
      overflow = false;

    } else if (c != '\r') {

      if (used < sizeof(row) - 1) {

        row[used++] = (char)c;

      } else {

        overflow = true;
      }
    }
  }

  if (!found && used > 0 && !overflow) {

    row[used] = '\0';

    unsigned long foundId;

    if (csvRowId(row, foundId) &&
        foundId == wantedId) {

      found = true;
    }
  }

  f.close();

  if (found) {

    sendCsvWithCrc(row);

  } else {

    esp.print(F("GAP,"));
    esp.println(wantedId);

    Serial.print(
      F("[SD] Missing record #")
    );

    Serial.println(wantedId);
  }
}

// ==========================================
// SEND LIVE SENSOR READING
// ==========================================

void sendReading() {

  esp.print(F("D,"));

  esp.print(millis() / 1000);
  esp.print(',');

  esp.print(distanceFromZeroMm);
  esp.print(',');

  if (weightOk) {

    esp.print(weightVal, 1);

  } else {

    esp.print(-999);
  }

  esp.print(',');

  esp.print(tofZero ? 1 : 0);
  esp.print(',');

  esp.print(nonContact1 ? 1 : 0);
  esp.print(',');

  esp.print(nonContact2 ? 1 : 0);
  esp.print(',');

  esp.print(pumpOn ? 1 : 0);
  esp.print(',');

  esp.print(mode);
  esp.print(',');

  esp.print(getNonContactLevel());
  esp.print(',');

  esp.println(getWeightIndicator());
}

// ==========================================
// SAVE SENSOR DATA TO SD
// ==========================================

void saveToSd() {

  if (!sdOk) {

    Serial.println(F("[SD] NOT SAVED"));
    return;
  }

  if (recordId == ULONG_MAX) {

    Serial.println(
      F("[SD] Record ID overflow")
    );

    return;
  }

  File f = SD.open(LOG_FILE, FILE_WRITE);

  if (!f) {

    sdOk = false;

    Serial.println(
      F("[SD] Open failed")
    );

    return;
  }

  unsigned long newId = recordId + 1;

  f.print(newId);
  f.print(',');

  f.print(millis() / 1000);
  f.print(',');

  f.print(distanceFromZeroMm);
  f.print(',');

  if (weightOk) {

    f.print(weightVal, 1);

  } else {

    f.print(F("ERR"));
  }

  f.print(',');

  f.print(tofZero ? 1 : 0);
  f.print(',');

  f.print(nonContact1 ? 1 : 0);
  f.print(',');

  f.print(nonContact2 ? 1 : 0);
  f.print(',');

  f.print(pumpOn ? 1 : 0);
  f.print(',');

  f.print(getNonContactLevel());
  f.print(',');

  f.println(getWeightIndicator());

  bool ok = !f.getWriteError();

  f.close();

  if (!ok) {

    sdOk = false;

    Serial.println(
      F("[SD] Write failed")
    );

    return;
  }

  recordId = newId;

  Serial.print(F("[SD] Saved #"));
  Serial.println(recordId);
}

// ==========================================
// SEND ACKNOWLEDGMENT
// ==========================================

void sendAck(
  const __FlashStringHelper *message
) {

  esp.print(F("ACK,"));
  esp.println(message);
}

// ==========================================
// WAKE INTERRUPT
// ==========================================

void wakeISR() {
  // Wake processor.
}

// ==========================================
// ENTER POWER DOWN SLEEP
// ==========================================

void goToSleep() {

  Serial.println(
    F("[SLEEP] Preparing sleep")
  );

  stopPumpCycle();

  // Prevent immediate repeat after wake
  // while the ToF is still triggered.
  pumpTriggered = true;

  sendAck(F("SLEEP"));

  esp.flush();

  if (tofOk) {
    tof.stopContinuous();
  }

  scale.power_down();

  uint8_t savedADC = ADCSRA;
  ADCSRA &= ~(1 << ADEN);

  pinMode(PIN_WAKE, INPUT);

  attachInterrupt(
    digitalPinToInterrupt(PIN_WAKE),
    wakeISR,
    LOW
  );

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  noInterrupts();

  sleep_enable();
  sleep_bod_disable();

  sei();
  sleep_cpu();

  sleep_disable();

  detachInterrupt(
    digitalPinToInterrupt(PIN_WAKE)
  );

  ADCSRA = savedADC;

  scale.power_up();

  if (tofOk) {
    tof.startContinuous();
  }

  mode = MODE_AUTO;

  stopPumpCycle();

  lastLive = millis() - LIVE_INTERVAL_MS;
  lastLog = millis();

  Serial.println(F("[SLEEP] Awake"));

  sendAck(F("WAKE"));

  readSensors();
  updateAutoPump();
  sendReading();
}

// ==========================================
// PROCESS ESP COMMANDS
// ==========================================

void handleCommand(const char *cmd) {

  // --------------------------------------
  // SD RECORD REQUEST
  // --------------------------------------

  if (strncmp(cmd, "GET,", 4) == 0) {

    const char *p = cmd + 4;

    if (!*p) return;

    for (const char *s = p; *s; s++) {

      if (*s < '0' || *s > '9') {
        return;
      }
    }

    unsigned long wanted =
      strtoul(p, NULL, 10);

    sendSdRecord(wanted);
    return;
  }

  // --------------------------------------
  // FIREBASE RECORD BASE
  // --------------------------------------

  if (strncmp(cmd, "BASE,", 5) == 0) {

    const char *p = cmd + 5;

    if (!*p) return;

    for (const char *c = p; *c; c++) {

      if (*c < '0' || *c > '9') {
        return;
      }
    }

    unsigned long committed =
      strtoul(p, NULL, 10);

    firebaseCommittedId = committed;

    if (committed > recordId) {
      recordId = committed;
    }

    Serial.print(
      F("[FIREBASE] Base ID: ")
    );

    Serial.println(committed);

    sendAck(F("BASE"));
    return;
  }

  // --------------------------------------
  // ESP IP ADDRESS
  // --------------------------------------

  if (strncmp(cmd, "IP,", 3) == 0) {

    strncpy(
      espIp,
      cmd + 3,
      sizeof(espIp) - 1
    );

    espIp[sizeof(espIp) - 1] = '\0';

    Serial.print(
      F("[ESP] IP Address: ")
    );

    Serial.println(espIp);

    return;
  }

  // --------------------------------------
  // AUTO MODE
  // --------------------------------------

  if (strcmp(cmd, "AUTO") == 0) {

    mode = MODE_AUTO;

    stopPumpCycle();
    pumpTriggered = true;

    lastLive = millis() - LIVE_INTERVAL_MS;

    readSensors();
    updateAutoPump();

    sendAck(F("AUTO"));
    sendReading();

    Serial.println(F("[MODE] AUTO"));
    return;
  }

  // --------------------------------------
  // MAINTENANCE MODE
  // --------------------------------------

  if (strcmp(cmd, "MAINT") == 0) {

    mode = MODE_MAINT;

    stopPumpCycle();
    pumpTriggered = true;

    readSensors();

    sendAck(F("MAINT"));
    sendReading();

    Serial.println(
      F("[MODE] MAINTENANCE")
    );

    return;
  }

  // --------------------------------------
  // SLEEP MODE
  // --------------------------------------

  if (strcmp(cmd, "SLEEP") == 0) {

    goToSleep();
    return;
  }

  // --------------------------------------
  // SENSOR CHECK
  // --------------------------------------

  if (strcmp(cmd, "READ") == 0) {

    if (mode == MODE_MAINT) {

      readSensors();
      sendReading();
    }

    return;
  }

  // --------------------------------------
  // MAINTENANCE COMMANDS
  // --------------------------------------

  if (mode != MODE_MAINT) {
    return;
  }

  // --------------------------------------
  // TARE LOAD CELL
  // --------------------------------------

  if (strcmp(cmd, "TARE") == 0) {

    if (scale.wait_ready_timeout(1000)) {

      scale.tare(5);

      sendAck(F("TARE"));

      readSensors();
      sendReading();

      Serial.println(
        F("[LOAD CELL] Tare completed")
      );

    } else {

      esp.println(F("ERR,HX711"));
    }

    return;
  }

  // --------------------------------------
  // PUMP ON
  // --------------------------------------

  if (strcmp(cmd, "PUMP1") == 0) {

    stopPumpCycle();

    setPump(true);

    sendAck(F("PUMP1"));
    sendReading();

    return;
  }

  // --------------------------------------
  // PUMP OFF
  // --------------------------------------

  if (strcmp(cmd, "PUMP0") == 0) {

    stopPumpCycle();

    sendAck(F("PUMP0"));
    sendReading();

    return;
  }
}

// ==========================================
// RECEIVE ESP COMMANDS
// ==========================================

void readEspCommands() {

  while (esp.available()) {

    int incoming = esp.read();

    if (incoming < 0) break;

    char c = (char)incoming;

    if (c == '\n' || c == '\r') {

      if (cmdLen > 0 && !cmdOverflow) {

        cmdBuf[cmdLen] = '\0';

        handleCommand(cmdBuf);
      }

      cmdLen = 0;
      cmdOverflow = false;

    } else if (c >= 32 && c <= 126) {

      if (!cmdOverflow) {

        if (cmdLen < sizeof(cmdBuf) - 1) {

          cmdBuf[cmdLen++] = c;

        } else {

          cmdOverflow = true;
        }
      }

    } else {

      cmdLen = 0;
      cmdOverflow = true;
    }
  }
}

// ==========================================
// SETUP
// ==========================================

void setup() {

  Serial.begin(9600);
  esp.begin(9600);

  Serial.println(
    F("=== WATER MONITOR WM001 ===")
  );

  // ESP GPIO2 -> Arduino D2
  pinMode(PIN_WAKE, INPUT);

  // Non-contact sensors
  pinMode(PIN_NONCONTACT1, INPUT);
  pinMode(PIN_NONCONTACT2, INPUT);

  // Initialize pump output safely.
  digitalWrite(
    PIN_PUMP,
    PUMP_ACTIVE_LOW ? HIGH : LOW
  );

  pinMode(PIN_PUMP, OUTPUT);

  pumpOn = false;

  // --------------------------------------
  // INITIALIZE SD CARD
  // --------------------------------------

  sdOk = startSdCard();

  if (sdOk) {

    Serial.println(
      F("[OK] SD Card Ready")
    );

  } else {

    Serial.println(
      F("[FAIL] SD Card")
    );
  }

  // --------------------------------------
  // INITIALIZE I2C
  // --------------------------------------

  Wire.begin();

  // --------------------------------------
  // INITIALIZE TOF
  // --------------------------------------

  tof.setTimeout(500);

  tofOk = tof.init();

  if (tofOk) {

    tof.startContinuous();

    Serial.println(F("[OK] VL53L0X"));

  } else {

    Serial.println(F("[FAIL] VL53L0X"));
  }

  // --------------------------------------
  // INITIALIZE HX711
  // --------------------------------------

  scale.begin(PIN_HX_DT, PIN_HX_SCK);

  scale.set_scale(CAL_FACTOR);

  if (scale.wait_ready_timeout(2000)) {

    // Tare before placing container.
    scale.tare(5);

    Serial.println(F("[OK] HX711"));

  } else {

    Serial.println(
      F("[WARN] HX711 not ready")
    );
  }

  // --------------------------------------
  // REQUEST ESP IP
  // --------------------------------------

  esp.println(F("BOOT"));

  lastLive = millis();
  lastLog = millis();
  lastSdCheck = millis();

  lastTofRead =
    millis() - TOF_READ_INTERVAL_MS;

  // Read sensors before normal operation.
  readSensors();

  // Avoid accidental pump activation
  // if ToF is already triggered at startup.
  pumpTriggered = true;

  Serial.println(F("[SYSTEM] Ready"));
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {

  // Process ESP commands.
  readEspCommands();

  unsigned long now = millis();

  // --------------------------------------
  // PUMP SAFETY TIMER
  // --------------------------------------

  // Check automatic pump timeout on
  // every loop, not just every sensor read.
  if (mode == MODE_AUTO && pumpRunning) {

    updateAutoPump();
  }

  // --------------------------------------
  // SD CARD CHECK
  // --------------------------------------

  if (now - lastSdCheck >= SD_CHECK_MS) {

    lastSdCheck = now;

    checkSdCard();
  }

  // --------------------------------------
  // AUTO MODE
  // --------------------------------------

  if (mode == MODE_AUTO) {

    // Read ToF every 200 ms.
    if (now - lastTofRead >=
        TOF_READ_INTERVAL_MS) {

      lastTofRead = now;

      readTof();
      updateAutoPump();
    }

    // Send live data every 2 seconds.
    if (now - lastLive >= LIVE_INTERVAL_MS) {

      lastLive = now;

      readSensors();
      updateAutoPump();

      sendReading();

      Serial.print(F("[TOF] Raw: "));
      Serial.print(distanceMm);

      Serial.print(F(" mm | Adjusted: "));
      Serial.print(distanceFromZeroMm);

      Serial.print(F(" mm | Pump: "));
      Serial.println(
        pumpOn ? F("ON") : F("OFF")
      );

      Serial.print(F("[NON-CONTACT] "));
      Serial.println(getNonContactLevel());

      Serial.print(F("[WEIGHT] "));
      Serial.print(weightVal, 1);

      Serial.print(F(" g | "));
      Serial.println(getWeightIndicator());
    }

    // Save data every 3 minutes.
    if (now - lastLog >= LOG_INTERVAL_MS) {

      lastLog = now;

      readSensors();
      updateAutoPump();

      saveToSd();
    }
  }

  // --------------------------------------
  // MAINTENANCE MODE
  // --------------------------------------

  else if (mode == MODE_MAINT) {

    // Maximum manual pump ON time:
    // 30 seconds.
    if (pumpOn &&
        now - pumpOnSince >=
        MAINT_PUMP_MAX_MS) {

      stopPumpCycle();

      sendAck(F("PUMP0"));
      sendReading();

      Serial.println(
        F("[MAINT] Pump safety timeout")
      );
    }
  }
}
