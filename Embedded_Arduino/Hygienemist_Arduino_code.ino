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


// TIMING SETTINGS

const unsigned long pumpTime = 3000;
const unsigned long liveTime = 2000;


// PUMP VARIABLES

bool pumpOn = false;
bool pumpTriggered = true;


// TIMING VARIABLES

unsigned long pumpStart = 0;
unsigned long lastLive = 0;


// FUNCTION DECLARATIONS

void checkPumpTime();

void sendLiveData();

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

  if (!pumpOn) return;

  if (millis() - pumpStart < pumpTime) return;

  turnPumpOff();

  sensors.resetDetection();

  Serial.println(F("[PUMP] OFF - timeout"));

}


// PREPARE LIVE SENSOR DATA

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

  output.print(0);

  output.print(',');


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


// PROCESS RECEIVED COMMANDS

void processCommand(
  Communication::Command command,
  unsigned long a,
  unsigned long b,
  byte kind,
  const char *text
) {

  switch (command) {

    case Communication::IP:

      Serial.print(F("[ESP] IP Address: "));

      Serial.println(text);

      break;


    case Communication::AUTO:

      // AUTO is the only operating mode
      // implemented at this stage.

      communication.ack(F("AUTO"));

      Serial.println(F("[MODE] AUTO"));

      sendLiveData();

      break;


    default:

      // Other commands will be implemented
      // in the succeeding commits.

      break;

  }

}


// DISPLAY SENSOR READINGS

void displayReadings() {

  Serial.println(F("-------------------------"));

  Serial.print(F("Distance: "));

  Serial.print(sensors.distance());

  Serial.println(F(" mm"));


  Serial.print(F("Adjusted distance: "));

  Serial.print(sensors.adjustedDistance());

  Serial.println(F(" mm"));


  Serial.print(F("Object detected: "));

  Serial.println(sensors.nearObject());


  Serial.print(F("Weight: "));

  if (sensors.weightReady()) {

    Serial.print(sensors.weight(), 1);

    Serial.println(F(" g"));

  } else {

    Serial.println(F("ERROR"));

  }


  Serial.print(F("Non-contact 1: "));

  Serial.println(sensors.nonContact1());


  Serial.print(F("Non-contact 2: "));

  Serial.println(sensors.nonContact2());


  Serial.print(F("Non-contact level: "));

  Serial.println(sensors.liquidLevel());


  Serial.print(F("Weight status: "));

  Serial.println(sensors.weightStatus());


  Serial.print(F("Pump: "));

  Serial.println(pumpOn ? F("ON") : F("OFF"));

}


// CHECK LOAD CELL TARE RESULT

void checkTare() {

  byte result = sensors.takeTareResult();

  if (result == 1) {

    Serial.println(F("[HX711] Tare successful"));

  } else if (result == 2) {

    Serial.println(F("[HX711] Tare timeout"));

  }

}


// AUTOMATIC MODE

void autoMode() {

  sensors.update(!pumpOn, true);

  checkPumpTime();

  checkTare();

  automaticPump();


  // SEND LIVE DATA EVERY TWO SECONDS

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    sendLiveData();

    if (!pumpOn) {

      displayReadings();

    }

  }

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


  // INFORM ESP-01 THAT ARDUINO HAS STARTED

  communication.output().println(F("BOOT"));


  lastLive = millis();

}


// MAIN LOOP

void loop() {

  checkPumpTime();


  // RECEIVE ESP-01 COMMANDS

  communication.update(processCommand);

  checkPumpTime();


  // AUTOMATIC OPERATION

  autoMode();


  // CHECK FOR ADDITIONAL COMMANDS

  communication.update(processCommand);

  checkPumpTime();

}
