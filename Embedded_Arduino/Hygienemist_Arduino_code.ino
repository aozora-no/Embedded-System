#include <Arduino.h>
#include "Sensors.h"
#include "Communication.h"


 // PIN ASSIGNMENTS

const byte wakePin = 3;
const byte pumpPin = 7;

const byte espRx = 9;
const byte espTx = 8;

const byte sdPin = 10;


// SENSOR AND COMMUNICATION OBJECTS

Sensors sensors;

Communication communication(espRx, espTx);


// OPERATING MODES

enum Mode {

  AUTO,
  MAINTENANCE

};

Mode currentMode = AUTO;


// TIMING SETTINGS

const unsigned long pumpTime = 3000;
const unsigned long liveTime = 2000;


// PUMP VARIABLES

bool pumpOn = false;
bool pumpTriggered = true;

byte pumpEvent = 0;


// MAINTENANCE VARIABLES

bool manualReading = false;
bool manualTare = false;

bool modeLivePending = false;


// TIMING VARIABLES

unsigned long pumpStart = 0;
unsigned long lastLive = 0;


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


  // IDENTIFY DISPENSING MODE
  // 1 = AUTO
  // 2 = MAINTENANCE

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

  if (pumpOn || sensors.isTaring()) return;


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


  // STOP PUMP AFTER THREE SECONDS

  turnPumpOff();

  sensors.resetDetection();

  Serial.println(F("[PUMP] OFF - timeout"));


  // CONFIRM MANUAL DISPENSING IS COMPLETE

  if (pumpEvent == 2) {

    communication.ack(F("PUMP0"));

    sendLiveData();

  }

}


// PREPARE LIVE SENSOR DATA

void writeReadings(
  Print &output,
  bool live,
  bool dispensed
) {


  // UPTIME

  output.print(millis() / 1000);

  output.print(',');


  // ADJUSTED ULTRASONIC DISTANCE

  output.print(sensors.adjustedDistance());

  output.print(',');


  // LOAD CELL READING

  if (sensors.weightReady()) {

    output.print(sensors.weight(), 1);

  } else {

    output.print(-999);

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


  // OPERATING MODE
  // AUTO = 0
  // MAINTENANCE = 1

  if (live) {

    output.print(currentMode == MAINTENANCE);

    output.print(',');

  }


  // NON-CONTACT LIQUID LEVEL

  output.print(sensors.liquidLevel());

  output.print(',');


  // WEIGHT STATUS

  output.println(sensors.weightStatus());

}


// SEND LIVE DATA TO ESP-01

void sendLiveData() {

  communication.live(writeReadings);

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


    // UPDATE CURRENT MODE

    currentMode = next;


    // RESET AUTOMATIC HAND DETECTION

    pumpTriggered = true;

    sensors.resetDetection();


    // RESET MANUAL SENSOR REQUEST

    manualReading = false;


    // IMMEDIATELY UPDATE AUTO MODE

    if (next == AUTO) {

      lastLive = millis() - liveTime;

    }


    // REQUEST LIVE STATUS TRANSMISSION

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

  // ONLY ACCEPT MANUAL COMMANDS
  // WHEN MAINTENANCE MODE IS ACTIVE

  if (currentMode != MAINTENANCE) return;


  switch (command) {


    // MANUAL SENSOR READING

    case Communication::READ:

      if (!manualReading && !manualTare) {

        sensors.requestRead();

        manualReading = true;

        Serial.println(F("[MAINT] Reading sensors"));

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


      // REQUEST LOAD CELL TARE

      manualReading = false;

      sensors.requestTare();

      manualTare = true;

      Serial.println(F("[MAINT] Taring load cell"));

      break;


    // MANUAL PUMP ACTIVATION

    case Communication::PUMP1:

      if (!pumpOn && !sensors.isTaring()) {

        turnPumpOn();


        // CONFIRM PUMP ACTIVATION

        communication.ack(F("PUMP1"));


        // TRANSMIT UPDATED PUMP STATUS

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


    // SWITCH TO AUTO MODE

    case Communication::AUTO:

      changeMode(AUTO);

      break;


    // SWITCH TO MAINTENANCE MODE

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

      // SD CARD AND SLEEP COMMANDS
      // WILL BE IMPLEMENTED LATER.

      break;

  }

}


// PROCESS SENSOR REQUESTS

void serviceSensorRequests() {


  // CHECK LOAD CELL TARE RESULT

  byte result = sensors.takeTareResult();

  if (result) {


    // MANUAL TARE REQUEST

    if (manualTare) {

      manualTare = false;


      // SUCCESSFUL TARE

      if (result == 1) {

        communication.ack(F("TARE"));

        Serial.println(F("[HX711] Tare successful"));


        // COLLECT NEW SENSOR READINGS

        sensors.requestRead();

        manualReading = true;

      }


      // FAILED TARE

      else {

        communication.output().println(F("ERR,HX711"));

        Serial.println(F("[HX711] Tare timeout"));

      }

    }


    // STARTUP TARE RESULT

    else if (result == 2) {

      Serial.println(F("[HX711] Not ready"));

    } else if (result == 1) {

      Serial.println(F("[HX711] Startup tare successful"));

    }

  }


  // CHECK MANUAL SENSOR READING

  if (manualReading && sensors.readingDone()) {

    manualReading = false;


    // SEND FRESH SENSOR READINGS

    sendLiveData();

    Serial.println(F("[MAINT] Sensor readings sent"));

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


  // LOAD CELL READING

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

  Serial.println(pumpOn ? F("ON") : F("OFF"));

}


// AUTOMATIC MODE

void autoMode() {


  // CONTINUOUS SENSOR READINGS

  sensors.update(!pumpOn, true);


  // CHECK PUMP TIMER

  checkPumpTime();


  // PROCESS PENDING SENSOR REQUESTS

  serviceSensorRequests();


  // AUTOMATIC HAND DETECTION

  automaticPump();


  // TRANSMIT LIVE DATA EVERY TWO SECONDS

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    sendLiveData();


    // DISPLAY READINGS WHEN PUMP IS OFF

    if (!pumpOn) {

      displayReadings();

    }

  }

}


// MAINTENANCE MODE

void maintenanceMode() {


  // DISABLE AUTOMATIC HAND DETECTION
  // DISABLE CONTINUOUS SENSOR SAMPLING

  sensors.update(false, false);


  // PROCESS MANUAL READ AND TARE REQUESTS

  serviceSensorRequests();


  // CHECK MANUAL PUMP TIMER

  checkPumpTime();

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


  // DEFAULT OPERATING MODE

  currentMode = AUTO;


  // NOTIFY ESP-01 OF ARDUINO STARTUP

  communication.output().println(F("BOOT"));


  lastLive = millis();

}


// MAIN LOOP

void loop() {


  // CHECK PUMP TIMER

  checkPumpTime();


  // RECEIVE ESP-01 COMMANDS

  communication.update(processCommand);

  checkPumpTime();


  // EXECUTE CURRENT OPERATING MODE

  switch (currentMode) {


    case AUTO:

      autoMode();

      break;


    case MAINTENANCE:

      maintenanceMode();

      break;

  }


  // TRANSMIT STATUS AFTER MODE CHANGE

  if (modeLivePending) {

    modeLivePending = false;

    sendLiveData();

  }


  // RECEIVE ADDITIONAL COMMANDS

  communication.update(processCommand);


  // FINAL PUMP TIMER CHECK

  checkPumpTime();

}
