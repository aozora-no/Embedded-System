#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "Sensors.h"
#include "Communication.h"


// PIN ASSIGNMENTS

const byte wakePin = 3;
const byte pumpPin = 7;
const byte espRx = 9;
const byte espTx = 8;
const byte sdPin = 10;


// TIMING SETTINGS

const unsigned long pumpTime = 3000;
const unsigned long liveTime = 2000;
const unsigned long logTime = 180000;
const unsigned long sdTime = 10000;


// SD CARD FILE

const char logFile[] = "HYGIENE.CSV";


// SENSOR AND COMMUNICATION OBJECTS

Sensors sensors;

Communication communication(espRx, espTx);


// OPERATING MODES

enum Mode {
  AUTO,
  MAINTENANCE
};

Mode currentMode = AUTO;


// SD CARD VARIABLES

bool sdReady = false;
bool scanningSD = false;


// PUMP VARIABLES

bool pumpOn = false;
bool pumpTriggered = true;
bool dispenseCompleted = false;
bool dispenseReading = false;

byte dispenseEvent = 0;
byte pumpEvent = 0;


// MAINTENANCE VARIABLES

bool manualReading = false;
bool manualTare = false;
bool modeLivePending = false;


// TIMING VARIABLES

unsigned long pumpStart = 0;
unsigned long lastLive = 0;
unsigned long lastLog = 0;
unsigned long lastSd = 0;


// RECORD ID VARIABLES

unsigned long recordId = 0;
unsigned long firebaseId = 0;
unsigned long scanOffset = 0;


// RECORD LOOKUP VARIABLES

unsigned long lookupOffset = 0;
unsigned long lookupId = 0;

unsigned long lastFoundId = 0;
unsigned long lastFoundEnd = 0;

unsigned long nextAvailableId = 0;


// SD PAGINATION VARIABLES

struct PageJob {

  bool active = false;
  bool sending = false;
  bool more = false;

  byte count = 0;
  byte next = 0;
  byte sent = 0;

  unsigned long ticket = 0;
  unsigned long before = 0;
  unsigned long offset = 0;
  unsigned long end = 0;

  unsigned long offsets[5];

} page;


// FUNCTION DECLARATIONS

void checkPumpTime();

void sendLiveData();

void changeMode(Mode next);

void processCommand(
  Communication::Command command,
  unsigned long a,
  unsigned long b,
  byte kind,
  const char *text
);


// INITIALIZE HARDWARE

void initializePins() {

  digitalWrite(pumpPin, HIGH);
  pinMode(pumpPin, OUTPUT);

  pinMode(wakePin, INPUT);

  pinMode(SS, OUTPUT);
  digitalWrite(SS, HIGH);

  pinMode(sdPin, OUTPUT);
  digitalWrite(sdPin, HIGH);

}


// TURN PUMP ON

void turnPumpOn() {

  if (pumpOn) return;

  pumpEvent = currentMode == MAINTENANCE ? 2 : 1;

  pumpOn = true;
  pumpStart = millis();

  digitalWrite(pumpPin, LOW);

  Serial.println(F("[PUMP] ON"));

}


// TURN PUMP OFF

void turnPumpOff() {

  digitalWrite(pumpPin, HIGH);

  pumpOn = false;

}


// AUTOMATIC PUMP ACTIVATION

void automaticPump() {

  if (
    pumpOn ||
    sensors.isTaring() ||
    (dispenseCompleted && sdReady)
  ) return;

  if (sensors.handRemoved()) {

    pumpTriggered = false;

  }

  if (
    !pumpTriggered &&
    sensors.handDetected() &&
    sensors.nearObject()
  ) {

    pumpTriggered = true;

    sensors.resetDetection();

    turnPumpOn();

  }

}


// NON-BLOCKING PUMP TIMER

void checkPumpTime() {

  if (!pumpOn || millis() - pumpStart < pumpTime) {

    return;

  }

  turnPumpOff();

  sensors.resetDetection();

  dispenseCompleted = true;
  dispenseEvent = pumpEvent;
  dispenseReading = false;

  Serial.println(F("[PUMP] OFF - timeout"));

  if (pumpEvent == 2) {

    communication.ack(F("PUMP0"));

    sendLiveData();

  }

}


// READ ONE CSV ROW

bool readRow(
  File &file,
  char *row,
  size_t capacity
) {

  while (file.available()) {

    size_t length = 0;

    bool tooLong = false;

    while (file.available()) {

      int c = file.read();

      if (c == '\n' || c < 0) break;

      if (c == '\r') continue;

      if (length < capacity - 1) {

        row[length++] = c;

      } else {

        tooLong = true;

      }

    }

    row[length] = '\0';

    if (!tooLong && length > 0) {

      return true;

    }

  }

  return false;

}


// EXTRACT RECORD ID FROM CSV

unsigned long rowId(const char *row) {

  if (*row < '0' || *row > '9') {

    return 0;

  }

  char *end;

  unsigned long id = strtoul(row, &end, 10);

  if (*end != ',') return 0;

  int commas = 0;

  for (; *row; row++) {

    if (*row == ',') commas++;

  }

  // SUPPORT CURRENT AND LEGACY CSV RECORDS

  return (commas == 9 || commas == 10)
    ? id
    : 0;

}


// HANDLE SD CARD FAILURE

void sdFailure(
  const __FlashStringHelper *reason
) {

  sdReady = false;
  scanningSD = false;

  lastSd = millis();

  Serial.print(F("[SD] ERROR: "));
  Serial.println(reason);

}


// CHECK WHETHER SD CAN BE READ

bool sdReadable() {

  return sdReady || scanningSD;

}


// INITIALIZE SD CARD

void startSD() {

  sdReady = false;
  scanningSD = false;

  Serial.println(F("[SD] Initializing..."));

  digitalWrite(sdPin, HIGH);

  if (!SD.begin(sdPin)) {

    sdFailure(F("SD.begin failed"));

    return;

  }

  Serial.println(F("[SD] Card initialized"));

  File file = SD.open(logFile, FILE_WRITE);

  if (!file) {

    sdFailure(
      F("Cannot open HYGIENE.CSV for writing")
    );

    return;

  }


  // CREATE CSV HEADER

  if (file.size() == 0) {

    file.println(F(
      "id,uptime_s,distance_mm,weight_g,"
      "ultrasonic_zero,noncontact1,noncontact2,"
      "pump,level,weight_status,event"
    ));

  }

  bool written = !file.getWriteError();

  file.close();

  if (!written) {

    sdFailure(F("CSV header write failed"));

    return;

  }


  // RESET RECORD LOOKUP VARIABLES

  scanOffset = 0;
  lookupOffset = 0;
  lookupId = 0;

  nextAvailableId = 0;

  lastFoundId = 0;
  lastFoundEnd = 0;


  // START RECORD ID RECOVERY

  scanningSD = true;

  Serial.println(
    F("[SD] Recovering record IDs; history can be read")
  );

}


// RECOVER RECORD IDS

void scanSD() {

  File file = SD.open(logFile, FILE_READ);

  if (!file || !file.seek(scanOffset)) {

    if (file) file.close();

    sdFailure(
      F("Record recovery open/seek failed")
    );

    return;

  }

  unsigned long started = millis();

  char row[112];


  // SCAN RECORDS IN SMALL TIME SLICES

  while (
    file.available() &&
    millis() - started < 15UL
  ) {

    if (!readRow(file, row, sizeof(row))) {

      break;

    }

    unsigned long id = rowId(row);

    if (id > recordId) {

      recordId = id;

    }

  }


  // SAVE SCAN POSITION

  scanOffset = file.position();

  if (file.available()) {

    file.close();

    return;

  }


  // CHECK FINAL CSV NEWLINE

  bool needsNewline = false;

  if (file.size() > 0) {

    if (!file.seek(file.size() - 1)) {

      file.close();

      sdFailure(
        F("Cannot check final CSV row")
      );

      return;

    }

    needsNewline = file.read() != '\n';

  }

  file.close();


  // REPAIR MISSING NEWLINE

  if (needsNewline) {

    file = SD.open(logFile, FILE_WRITE);

    if (!file) {

      sdFailure(
        F("Cannot reopen CSV for repair")
      );

      return;

    }

    file.println();

    bool written = !file.getWriteError();

    file.close();

    if (!written) {

      sdFailure(F("CSV repair write failed"));

      return;

    }

  }


  // SYNCHRONIZE WITH FIREBASE RECORD ID

  if (firebaseId > recordId) {

    recordId = firebaseId;

  }


  // SD CARD READY

  scanningSD = false;
  sdReady = true;

  Serial.print(F("[SD] Ready. Last record: "));
  Serial.println(recordId);

}


// CHECK SD CARD CONNECTION

void checkSD() {

  if (scanningSD) return;

  if (!sdReady) {

    startSD();

    return;

  }

  File file = SD.open(logFile, FILE_READ);

  if (!file) {

    sdFailure(
      F("Card check could not open HYGIENE.CSV")
    );

    return;

  }

  file.close();

}


// PREPARE SENSOR DATA

void writeReadings(
  Print &output,
  bool live,
  bool dispensed
) {

  // UPTIME

  output.print(millis() / 1000);
  output.print(',');


  // ULTRASONIC DISTANCE

  output.print(sensors.adjustedDistance());
  output.print(',');


  // LOAD CELL

  if (sensors.weightReady()) {

    output.print(sensors.weight(), 1);

  } else if (live) {

    output.print(-999);

  } else {

    output.print(F("ERR"));

  }

  output.print(',');


  // OBJECT DETECTION

  output.print(sensors.nearObject());
  output.print(',');


  // NON-CONTACT SENSOR 1

  output.print(sensors.nonContact1());
  output.print(',');


  // NON-CONTACT SENSOR 2

  output.print(sensors.nonContact2());
  output.print(',');


  // PUMP STATUS

  output.print(pumpOn || dispensed);
  output.print(',');


  // OPERATING MODE FOR LIVE DATA

  if (live) {

    output.print(currentMode == MAINTENANCE);

    output.print(',');

  }


  // NON-CONTACT LEVEL

  output.print(sensors.liquidLevel());
  output.print(',');


  // WEIGHT STATUS

  output.print(sensors.weightStatus());


  // EVENT TYPE FOR SD RECORDS

  if (!live) {

    output.print(',');

    if (!dispensed) {

      output.println(F("PERIODIC"));

    } else if (dispenseEvent == 2) {

      output.println(F("MAINTENANCE_DISPENSE"));

    } else {

      output.println(F("AUTO_DISPENSE"));

    }

  } else {

    output.println();

  }

}


// SEND LIVE SENSOR DATA

void sendLiveData() {

  communication.live(writeReadings);

}


// SAVE SENSOR DATA TO SD CARD

void saveData(bool dispensed) {

  if (!sdReady || recordId == ULONG_MAX) {

    return;

  }

  File file = SD.open(logFile, FILE_WRITE);

  if (!file) {

    sdFailure(F("Cannot open CSV for logging"));

    return;

  }


  // WRITE NEW RECORD ID

  file.print(recordId + 1);

  file.print(',');


  // WRITE SENSOR READINGS

  writeReadings(file, false, dispensed);

  file.flush();

  bool saved = !file.getWriteError();

  file.close();


  // UPDATE RECORD ID

  if (saved) {

    recordId++;

    Serial.print(F("[SD] Saved record "));
    Serial.println(recordId);

  } else {

    sdFailure(F("Record write failed"));

  }

}


// SAVE DISPENSING EVENT

void saveDispenseData() {

  if (
    !dispenseCompleted ||
    pumpOn ||
    !sdReady
  ) return;


  // REQUEST FRESH SENSOR READINGS

  if (!dispenseReading) {

    if (sensors.isTaring()) return;

    sensors.requestRead();

    dispenseReading = true;

    return;

  }

  if (!sensors.readingDone()) return;

  unsigned long previousId = recordId;

  saveData(true);


  // CHECK SUCCESSFUL LOGGING

  if (recordId > previousId) {

    dispenseCompleted = false;
    dispenseReading = false;

    Serial.println(F("[DISPENSE] Data saved"));

  }

}


// SAVE PERIODIC SENSOR DATA

void savePeriodicData() {

  if (
    pumpOn ||
    !sdReady ||
    sensors.isTaring() ||
    dispenseCompleted
  ) return;

  if (millis() - lastLog < logTime) {

    return;

  }

  unsigned long previousId = recordId;

  saveData(false);

  if (recordId > previousId) {

    lastLog = millis();

  }

}


// RETRIEVE AND SEND SAVED RECORD

void sendRecord(unsigned long wantedId) {

  Serial.print(F("[GET] "));
  Serial.println(wantedId);


  // CHECK IF SYSTEM IS BUSY

  if (pumpOn || scanningSD) {

    communication.output().print(F("BUSY,"));

    communication.output().println(wantedId);

    return;

  }


  // CHECK SD CARD

  if (!sdReady) {

    communication.output().println(F("ERR,SD"));

    return;

  }


  // VALIDATE REQUESTED ID

  if (wantedId == 0) {

    communication.output().println(F("ERR,ID"));

    return;

  }


  // CHECK END OF RECORDS

  if (wantedId > recordId) {

    communication.output().print(F("END,"));

    communication.output().println(wantedId);

    return;

  }


  // OPEN CSV FILE

  File file = SD.open(logFile, FILE_READ);

  if (!file) {

    sdFailure(
      F("Cannot open CSV for record transfer")
    );

    communication.output().println(F("ERR,SD"));

    return;

  }


  // INITIALIZE OR CONTINUE RECORD SEARCH

  if (lookupId != wantedId) {

    lookupId = wantedId;

    nextAvailableId = 0;

    lookupOffset = (wantedId > lastFoundId)
      ? lastFoundEnd
      : 0;

  }


  // SEEK TO PREVIOUS SEARCH POSITION

  if (!file.seek(lookupOffset)) {

    file.close();

    lookupOffset = 0;
    lookupId = 0;
    nextAvailableId = 0;

    sdFailure(F("Record transfer seek failed"));

    communication.output().println(F("ERR,SD"));

    return;

  }

  unsigned long started = millis();

  char row[112];


  // SEARCH CSV RECORDS

  while (
    file.available() &&
    millis() - started < 15UL
  ) {

    if (!readRow(file, row, sizeof(row))) {

      break;

    }

    checkPumpTime();

    unsigned long id = rowId(row);


    // TRACK NEXT AVAILABLE RECORD

    if (
      id > wantedId &&
      (!nextAvailableId || id < nextAvailableId)
    ) {

      nextAvailableId = id;

    }


    // REQUESTED RECORD FOUND

    if (id == wantedId) {

      lastFoundId = wantedId;

      lastFoundEnd = file.position();

      lookupId = 0;

      file.close();


      // SEND RECORD WITH CRC CHECKSUM

      communication.record(row);

      Serial.print(F("[SD] Sent record "));
      Serial.println(wantedId);

      return;

    }

  }


  // SAVE CURRENT SEARCH POSITION

  lookupOffset = file.position();

  bool more = file.available();

  file.close();


  // REQUEST ESP TO RETRY

  if (more) {

    communication.output().print(F("BUSY,"));

    communication.output().println(wantedId);

    return;

  }

  lookupId = 0;


  // HANDLE MISSING RECORD ID

  if (nextAvailableId) {

    communication.output().print(F("NEXT,"));

    communication.output().print(wantedId);

    communication.output().print(',');

    communication.output().println(nextAvailableId);

    Serial.print(F("[SD] Missing ID; next available: "));

    Serial.println(nextAvailableId);

    return;

  }


  // NO HIGHER RECORD FOUND

  communication.output().print(F("GAP,"));

  communication.output().println(wantedId);

}


// INITIALIZE SD PAGINATION REQUEST

void sendSDPage(
  unsigned long ticket,
  unsigned long before
) {

  // ALLOW READ-ONLY PAGES DURING ID RECOVERY

  if (!sdReadable() || pumpOn) {

    communication.pageError(
      ticket,
      pumpOn ? F("BUSY") : F("SD")
    );

    return;

  }


  // IGNORE DUPLICATE ACTIVE REQUEST

  if (
    page.active &&
    page.ticket == ticket &&
    page.before == before
  ) {

    return;

  }


  // OPEN SENSOR CSV FILE

  File file = SD.open(logFile, FILE_READ);

  if (!file) {

    Serial.print(F("[SD] Cannot open page file: "));
    Serial.println(logFile);

    communication.pageError(ticket, F("SD"));

    return;

  }


  // INITIALIZE PAGE JOB

  page.active = true;
  page.sending = false;
  page.more = false;

  page.count = 0;
  page.next = 0;
  page.sent = 0;

  page.ticket = ticket;
  page.before = before;
  page.offset = 0;

  page.end = file.size();

  file.close();

}


// PROCESS SD PAGINATION

void servicePage() {

  if (!page.active || pumpOn) {

    return;

  }


  // CHECK SD CARD STATUS

  if (!sdReadable()) {

    communication.pageError(page.ticket, F("SD"));

    page.active = false;

    return;

  }


  // OPEN SENSOR CSV

  File file = SD.open(logFile, FILE_READ);

  char row[112];

  if (!file) {

    Serial.println(F("[SD] Page read open failed"));

    communication.pageError(page.ticket, F("SD"));

    page.active = false;

    return;

  }


  // SCAN PAGE RECORDS

  if (!page.sending) {

    if (!file.seek(page.offset)) {

      file.close();

      Serial.println(F("[SD] Page seek failed"));

      communication.pageError(page.ticket, F("SD"));

      page.active = false;

      return;

    }

    unsigned long started = millis();


    // SCAN IN SMALL TIME SLICES

    while (
      file.position() < page.end &&
      file.available() &&
      millis() - started < 15UL
    ) {

      unsigned long offset = file.position();

      if (!readRow(file, row, sizeof(row))) {

        break;

      }

      if (file.position() > page.end) {

        break;

      }


      // VALIDATE RECORD ID AND CURSOR

      unsigned long id = rowId(row);

      if (
        !id ||
        (page.before && id >= page.before)
      ) {

        continue;

      }


      // KEEP LAST FIVE MATCHING RECORDS

      page.offsets[page.next] = offset;

      page.next = (page.next + 1) % 5;

      if (page.count < 5) {

        page.count++;

      } else {

        page.more = true;

      }

    }


    // STORE SCAN PROGRESS

    page.offset = file.position();

    page.sending =
      page.offset >= page.end ||
      !file.available();

    file.close();

    return;

  }


  // TRANSMIT PAGE RECORDS

  if (page.sent < page.count) {

    byte index =
      (page.next + 4 - page.sent) % 5;


    // READ REQUESTED RECORD

    if (
      !file.seek(page.offsets[index]) ||
      !readRow(file, row, sizeof(row))
    ) {

      file.close();

      Serial.println(F("[SD] Page row read failed"));

      communication.pageError(page.ticket, F("SD"));

      page.active = false;

      return;

    }

    file.close();


    // SEND ONE RECORD WITH CRC CHECKSUM
    // KIND 0 = SENSOR RECORDS

    communication.pageRow(
      page.ticket,
      0,
      page.offsets[index] + 1,
      row
    );

    page.sent++;

    return;

  }


  // PAGE TRANSMISSION COMPLETE

  file.close();

  communication.pageEnd(
    page.ticket,
    page.more
  );

  page.active = false;

}


// CHANGE OPERATING MODE

void changeMode(Mode next) {

  if (currentMode != next) {

    bool manualPumpStopped =
      pumpOn && pumpEvent == 2;

    turnPumpOff();

    if (manualPumpStopped) {

      communication.ack(F("PUMP0"));

    }

    currentMode = next;

    pumpTriggered = true;

    sensors.resetDetection();

    manualReading = false;

    if (next == AUTO) {

      lastLive = millis() - liveTime;

    }

    modeLivePending = true;

  }


  // SEND MODE ACKNOWLEDGMENT

  communication.ack(
    next == MAINTENANCE
      ? F("MAINT")
      : F("AUTO")
  );

  Serial.println(
    next == MAINTENANCE
      ? F("[MODE] MAINTENANCE")
      : F("[MODE] AUTO")
  );

}


// MAINTENANCE COMMANDS

void maintenanceCommand(
  Communication::Command command
) {

  if (currentMode != MAINTENANCE) return;

  switch (command) {


    // MANUAL SENSOR READING

    case Communication::READ:

      if (!manualReading && !manualTare) {

        sensors.requestRead();

        manualReading = true;

        Serial.println(
          F("[MAINT] Reading sensors")
        );

      }

      break;


    // MANUAL LOAD CELL TARE

    case Communication::TARE:

      if (manualTare) break;

      if (pumpOn) {

        turnPumpOff();

        communication.ack(F("PUMP0"));

      }

      manualReading = false;

      sensors.requestTare();

      manualTare = true;

      Serial.println(
        F("[MAINT] Taring load cell")
      );

      break;


    // MANUAL PUMP ACTIVATION

    case Communication::PUMP1:

      if (
        !pumpOn &&
        !sensors.isTaring() &&
        !(dispenseCompleted && sdReady)
      ) {

        turnPumpOn();

        communication.ack(F("PUMP1"));

        sendLiveData();

      }

      break;


    default:

      break;

  }

}


// PROCESS RECEIVED COMMANDS

void processCommand(
  Communication::Command command,
  unsigned long a,
  unsigned long b,
  byte kind,
  const char *text
) {

  switch (command) {


    // RETRIEVE SAVED RECORD

    case Communication::GET:

      sendRecord(a);

      break;


    // SYNCHRONIZE FIREBASE RECORD ID

    case Communication::BASE:

      Serial.print(F("[BASE] Firebase cursor: "));
      Serial.println(a);

      firebaseId = a;

      if (firebaseId > recordId) {

        recordId = firebaseId;

      }


      // CONFIRM FIREBASE SYNCHRONIZATION

      communication.ack(F("BASE"));

      break;


    // RECEIVE ESP-01 IP ADDRESS

    case Communication::IP:

      Serial.print(F("[ESP] IP Address: "));
      Serial.println(text);

      break;


    // AUTOMATIC MODE

    case Communication::AUTO:

      changeMode(AUTO);

      break;


    // MAINTENANCE MODE

    case Communication::MAINT:

      changeMode(MAINTENANCE);

      break;


    // MAINTENANCE COMMANDS

    case Communication::READ:

    case Communication::TARE:

    case Communication::PUMP1:

      maintenanceCommand(command);

      break;


    // SENSOR LOG PAGINATION

    case Communication::SDPAGE:

      sendSDPage(a, b);

      break;


    default:

      // SLEEP AND SESSION HISTORY COMMANDS
      // WILL BE IMPLEMENTED LATER.

      break;

  }

}


// PROCESS SENSOR REQUESTS

void serviceSensorRequests() {

  byte result = sensors.takeTareResult();

  if (result) {

    if (manualTare) {

      manualTare = false;


      // SUCCESSFUL TARE

      if (result == 1) {

        communication.ack(F("TARE"));

        Serial.println(
          F("[HX711] Tare successful")
        );

        sensors.requestRead();

        manualReading = true;

      }


      // FAILED TARE

      else {

        communication.output().println(
          F("ERR,HX711")
        );

        Serial.println(
          F("[HX711] Tare timeout")
        );

      }

    }


    // STARTUP TARE RESULT

    else if (result == 2) {

      Serial.println(F("[HX711] Not ready"));

    } else if (result == 1) {

      Serial.println(
        F("[HX711] Startup tare successful")
      );

    }

  }


  // TRANSMIT REQUESTED SENSOR READINGS

  if (manualReading && sensors.readingDone()) {

    manualReading = false;

    sendLiveData();

    Serial.println(
      F("[MAINT] Sensor readings sent")
    );

  }

}


// DISPLAY SENSOR READINGS

void displayReadings() {

  Serial.println(F("-------------------------"));

  Serial.print(F("Mode: "));

  Serial.println(
    currentMode == AUTO
      ? F("AUTO")
      : F("MAINTENANCE")
  );


  // ULTRASONIC SENSOR

  Serial.print(F("Distance: "));
  Serial.print(sensors.distance());
  Serial.println(F(" mm"));

  Serial.print(F("Adjusted distance: "));
  Serial.print(sensors.adjustedDistance());
  Serial.println(F(" mm"));

  Serial.print(F("Object detected: "));
  Serial.println(sensors.nearObject());


  // LOAD CELL

  Serial.print(F("Weight: "));

  if (sensors.weightReady()) {

    Serial.print(sensors.weight(), 1);
    Serial.println(F(" g"));

  } else {

    Serial.println(F("ERROR"));

  }


  // NON-CONTACT SENSORS

  Serial.print(F("Non-contact 1: "));
  Serial.println(sensors.nonContact1());

  Serial.print(F("Non-contact 2: "));
  Serial.println(sensors.nonContact2());

  Serial.print(F("Non-contact level: "));
  Serial.println(sensors.liquidLevel());


  // WEIGHT STATUS

  Serial.print(F("Weight status: "));
  Serial.println(sensors.weightStatus());


  // PUMP STATUS

  Serial.print(F("Pump: "));

  Serial.println(
    pumpOn ? F("ON") : F("OFF")
  );


  // SD CARD STATUS

  Serial.print(F("SD: "));

  if (scanningSD) {

    Serial.println(F("RECOVERING"));

  } else if (sdReady) {

    Serial.println(F("READY"));

  } else {

    Serial.println(F("ERROR"));

  }


  // RECORD IDS

  Serial.print(F("Last record: "));
  Serial.println(recordId);

  Serial.print(F("Firebase record: "));
  Serial.println(firebaseId);

}


// AUTOMATIC MODE

void autoMode() {

  sensors.update(!pumpOn, true);

  checkPumpTime();

  serviceSensorRequests();

  automaticPump();


  // PERIODIC LIVE DATA

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    sendLiveData();

    if (!pumpOn) {

      displayReadings();

    }

  }


  // PERIODIC SD LOGGING

  savePeriodicData();

}


// MAINTENANCE MODE

void maintenanceMode() {

  sensors.update(false, false);

  serviceSensorRequests();

  checkPumpTime();

}


// SERVICE SD CARD TASKS

void serviceSD() {

  // AVOID SD OPERATIONS DURING DISPENSING

  if (pumpOn) return;


  // RECOVER RECORD IDS

  if (scanningSD) {

    scanSD();

  }


  // PERIODIC SD CARD CHECK

  if (millis() - lastSd >= sdTime) {

    lastSd = millis();

    checkSD();

  }


  // SAVE DISPENSING RECORDS

  saveDispenseData();


  // PROCESS SD PAGE REQUEST

  servicePage();

}


// SETUP

void setup() {

  initializePins();


  // HARDWARE SERIAL

  Serial.begin(9600);


  // SOFTWARE SERIAL

  communication.begin(9600);

  Serial.println(F("=== HYGIENEMIST ==="));


  // INITIALIZE SENSORS

  sensors.begin();


  // INITIALIZE SD CARD

  startSD();


  // DEFAULT OPERATING MODE

  currentMode = AUTO;


  // NOTIFY ESP-01 OF STARTUP

  communication.output().println(F("BOOT"));


  // INITIALIZE TIMERS

  lastLive = millis();
  lastLog = millis();
  lastSd = millis();

}


// MAIN LOOP

void loop() {

  checkPumpTime();


  // RECEIVE ESP COMMANDS

  communication.update(processCommand);

  checkPumpTime();


  // EXECUTE OPERATING MODE

  switch (currentMode) {

    case AUTO:

      autoMode();

      break;

    case MAINTENANCE:

      maintenanceMode();

      break;

  }


  // SEND STATUS AFTER MODE CHANGE

  if (modeLivePending) {

    modeLivePending = false;

    sendLiveData();

  }


  // RECEIVE ADDITIONAL COMMANDS

  communication.update(processCommand);


  // SERVICE SD CARD

  serviceSD();


  // FINAL PUMP TIMER CHECK

  checkPumpTime();

}
