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

unsigned long recordId = 0;
unsigned long scanOffset = 0;


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

  // 1 = AUTO DISPENSE
  // 2 = MAINTENANCE DISPENSE

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


  // REARM AFTER HAND REMOVAL

  if (sensors.handRemoved()) {

    pumpTriggered = false;

  }


  // DETECT HAND AND ACTIVATE PUMP

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

  if (!pumpOn) return;

  if (millis() - pumpStart < pumpTime) return;


  // STOP PUMP

  turnPumpOff();

  sensors.resetDetection();


  // MARK DISPENSING EVENT FOR SD LOGGING

  dispenseCompleted = true;

  dispenseEvent = pumpEvent;

  dispenseReading = false;

  Serial.println(F("[PUMP] OFF - timeout"));


  // CONFIRM MANUAL DISPENSING COMPLETED

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


// EXTRACT RECORD ID FROM CSV ROW

unsigned long rowId(const char *row) {

  // IGNORE HEADER OR INVALID ROW

  if (*row < '0' || *row > '9') {

    return 0;

  }

  char *end;

  unsigned long id = strtoul(row, &end, 10);

  if (*end != ',') return 0;


  // VALIDATE NUMBER OF CSV FIELDS

  int commas = 0;

  for (; *row; row++) {

    if (*row == ',') {

      commas++;

    }

  }


  // SUPPORT ORIGINAL AND LEGACY RECORDS

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


// INITIALIZE SD CARD

void startSD() {

  sdReady = false;

  scanningSD = false;

  Serial.println(F("[SD] Initializing..."));

  digitalWrite(sdPin, HIGH);


  // CHECK SD CARD INITIALIZATION

  if (!SD.begin(sdPin)) {

    sdFailure(F("SD.begin failed"));

    return;

  }

  Serial.println(F("[SD] Card initialized"));


  // OPEN OR CREATE CSV FILE

  File file = SD.open(logFile, FILE_WRITE);

  if (!file) {

    sdFailure(
      F("Cannot open HYGIENE.CSV for writing")
    );

    return;

  }


  // CREATE HEADER IF FILE IS EMPTY

  if (file.size() == 0) {

    file.println(F(
      "id,uptime_s,distance_mm,weight_g,"
      "ultrasonic_zero,noncontact1,noncontact2,"
      "pump,level,weight_status,event"
    ));

  }


  // CHECK HEADER WRITE

  bool written = !file.getWriteError();

  file.close();

  if (!written) {

    sdFailure(F("CSV header write failed"));

    return;

  }


  // START RECORD ID RECOVERY

  scanOffset = 0;

  scanningSD = true;

  Serial.println(F("[SD] Recovering record IDs"));

}


// RECOVER LAST RECORD ID

void scanSD() {

  File file = SD.open(logFile, FILE_READ);


  // OPEN FILE AND CONTINUE SCANNING

  if (!file || !file.seek(scanOffset)) {

    if (file) {

      file.close();

    }

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


  // SAVE CURRENT SCAN POSITION

  scanOffset = file.position();


  // CONTINUE RECOVERY ON NEXT LOOP

  if (file.available()) {

    file.close();

    return;

  }


  // CHECK IF FINAL ROW HAS A NEWLINE

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


  // REPAIR MISSING FINAL NEWLINE

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


  // SD CARD IS READY FOR LOGGING

  scanningSD = false;

  sdReady = true;

  Serial.print(F("[SD] Ready. Last record: "));

  Serial.println(recordId);

}


// CHECK SD CARD CONNECTION

void checkSD() {

  // DO NOT INTERRUPT RECORD RECOVERY

  if (scanningSD) return;


  // RETRY FAILED INITIALIZATION

  if (!sdReady) {

    startSD();

    return;

  }


  // CHECK IF CSV FILE CAN STILL BE OPENED

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


  // UPTIME IN SECONDS

  output.print(millis() / 1000);

  output.print(',');


  // ADJUSTED ULTRASONIC DISTANCE

  output.print(sensors.adjustedDistance());

  output.print(',');


  // LOAD CELL WEIGHT

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
  // DISPENSING LOGS RECORD PUMP AS ON

  output.print(pumpOn || dispensed);

  output.print(',');


  // INCLUDE MODE ONLY IN LIVE DATA

  if (live) {

    output.print(
      currentMode == MAINTENANCE
    );

    output.print(',');

  }


  // NON-CONTACT LEVEL

  output.print(sensors.liquidLevel());

  output.print(',');


  // WEIGHT STATUS

  output.print(sensors.weightStatus());


  // INCLUDE EVENT ONLY IN SD LOGS

  if (!live) {

    output.print(',');

    if (!dispensed) {

      output.println(F("PERIODIC"));

    } else if (dispenseEvent == 2) {

      output.println(
        F("MAINTENANCE_DISPENSE")
      );

    } else {

      output.println(
        F("AUTO_DISPENSE")
      );

    }

  } else {

    output.println();

  }

}


// SEND LIVE DATA TO ESP-01

void sendLiveData() {

  communication.live(writeReadings);

}


// SAVE SENSOR DATA TO SD CARD

void saveData(bool dispensed) {

  if (!sdReady || recordId == ULONG_MAX) {

    return;

  }


  // OPEN CSV FILE IN APPEND MODE

  File file = SD.open(logFile, FILE_WRITE);

  if (!file) {

    sdFailure(F("Cannot open CSV for logging"));

    return;

  }


  // WRITE NEXT RECORD ID

  file.print(recordId + 1);

  file.print(',');


  // WRITE SENSOR DATA

  writeReadings(file, false, dispensed);


  // FLUSH FILE DATA

  file.flush();


  // CHECK WRITE RESULT

  bool saved = !file.getWriteError();

  file.close();


  // UPDATE RECORD ID AFTER SUCCESSFUL WRITE

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
  // AFTER DISPENSING

  if (!dispenseReading) {

    if (sensors.isTaring()) return;

    sensors.requestRead();

    dispenseReading = true;

    return;

  }


  // WAIT UNTIL SENSOR READING IS COMPLETE

  if (!sensors.readingDone()) return;


  // STORE PREVIOUS RECORD ID

  unsigned long previousId = recordId;


  // SAVE DISPENSING RECORD

  saveData(true);


  // CHECK IF RECORD WAS SAVED

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


  // SAVE EVERY THREE MINUTES

  if (millis() - lastLog < logTime) {

    return;

  }

  unsigned long previousId = recordId;


  // SAVE CURRENT SENSOR READINGS

  saveData(false);


  // UPDATE TIMER AFTER SUCCESSFUL LOGGING

  if (recordId > previousId) {

    lastLog = millis();

  }

}


// CHANGE OPERATING MODE

void changeMode(Mode next) {

  if (currentMode != next) {


    // CHECK IF MANUAL PUMP IS ACTIVE

    bool manualPumpStopped =
      pumpOn && pumpEvent == 2;


    // STOP PUMP BEFORE CHANGING MODE

    turnPumpOff();


    // CONFIRM MANUAL PUMP STOPPED

    if (manualPumpStopped) {

      communication.ack(F("PUMP0"));

    }


    // UPDATE OPERATING MODE

    currentMode = next;


    // RESET HAND DETECTION

    pumpTriggered = true;

    sensors.resetDetection();


    // RESET MANUAL SENSOR REQUEST

    manualReading = false;


    // UPDATE AUTO MODE IMMEDIATELY

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


  // DISPLAY CURRENT MODE

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


      // STOP PUMP BEFORE TARING

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


    default:

      // RECORD RETRIEVAL, HISTORY AND SLEEP
      // WILL BE IMPLEMENTED IN LATER COMMITS.

      break;

  }

}


// PROCESS SENSOR REQUESTS

void serviceSensorRequests() {

  byte result = sensors.takeTareResult();


  // CHECK TARE RESULT

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


  // CHECK MANUAL SENSOR READING

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


  // CURRENT MODE

  Serial.print(F("Mode: "));

  Serial.println(

    currentMode == AUTO

      ? F("AUTO")

      : F("MAINTENANCE")

  );


  // ULTRASONIC DISTANCE

  Serial.print(F("Distance: "));

  Serial.print(sensors.distance());

  Serial.println(F(" mm"));


  // ADJUSTED DISTANCE

  Serial.print(F("Adjusted distance: "));

  Serial.print(sensors.adjustedDistance());

  Serial.println(F(" mm"));


  // OBJECT DETECTION

  Serial.print(F("Object detected: "));

  Serial.println(sensors.nearObject());


  // LOAD CELL WEIGHT

  Serial.print(F("Weight: "));

  if (sensors.weightReady()) {

    Serial.print(sensors.weight(), 1);

    Serial.println(F(" g"));

  } else {

    Serial.println(F("ERROR"));

  }


  // NON-CONTACT SENSOR 1

  Serial.print(F("Non-contact 1: "));

  Serial.println(sensors.nonContact1());


  // NON-CONTACT SENSOR 2

  Serial.print(F("Non-contact 2: "));

  Serial.println(sensors.nonContact2());


  // NON-CONTACT LEVEL

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


  // LAST SAVED RECORD

  Serial.print(F("Last record: "));

  Serial.println(recordId);

}


// AUTOMATIC MODE

void autoMode() {


  // CONTINUOUS SENSOR SAMPLING

  sensors.update(!pumpOn, true);


  // CHECK PUMP TIMER

  checkPumpTime();


  // PROCESS SENSOR REQUESTS

  serviceSensorRequests();


  // AUTOMATIC HAND DETECTION

  automaticPump();


  // SEND LIVE DATA EVERY TWO SECONDS

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    sendLiveData();

    if (!pumpOn) {

      displayReadings();

    }

  }


  // SAVE PERIODIC SENSOR DATA

  savePeriodicData();

}


// MAINTENANCE MODE

void maintenanceMode() {


  // DISABLE AUTOMATIC HAND DETECTION
  // AND CONTINUOUS SENSOR SAMPLING

  sensors.update(false, false);


  // PROCESS MANUAL AND DISPENSING READINGS

  serviceSensorRequests();


  // CHECK MANUAL PUMP TIMER

  checkPumpTime();

}


// SERVICE SD CARD

void serviceSD() {


  // DO NOT PERFORM SD OPERATIONS
  // WHILE THE PUMP IS ACTIVE

  if (pumpOn) return;


  // CONTINUE RECORD ID RECOVERY

  if (scanningSD) {

    scanSD();

  }


  // CHECK SD CARD EVERY TEN SECONDS

  if (millis() - lastSd >= sdTime) {

    lastSd = millis();

    checkSD();

  }


  // SAVE COMPLETED DISPENSING EVENT

  saveDispenseData();

}


// SETUP

void setup() {


  // INITIALIZE HARDWARE

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


  // CHECK PUMP TIMER

  checkPumpTime();


  // RECEIVE ESP-01 COMMANDS

  communication.update(processCommand);

  checkPumpTime();


  // EXECUTE CURRENT MODE

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


  // PROCESS SD CARD TASKS

  serviceSD();


  // FINAL PUMP TIMER CHECK

  checkPumpTime();

}
