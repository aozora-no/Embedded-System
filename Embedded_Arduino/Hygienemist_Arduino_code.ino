#include <Arduino.h>
#include "Sensors.h"


// PIN ASSIGNMENTS

const byte wakePin = 3;
const byte pumpPin = 7;

const byte espRx = 9;
const byte espTx = 8;

const byte sdPin = 10;


// SENSOR OBJECT

Sensors sensors;


// TIMING SETTINGS

const unsigned long pumpTime = 3000;
const unsigned long liveTime = 2000;


// PUMP VARIABLES

bool pumpOn = false;
bool pumpTriggered = true;


// TIMING VARIABLES

unsigned long pumpStart = 0;
unsigned long lastLive = 0;


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

  // Prevent activation while dispensing or taring.

  if (pumpOn || sensors.isTaring()) return;


  // Allow another dispensing cycle
  // after the hand is removed.

  if (sensors.handRemoved()) {

    pumpTriggered = false;

  }


  // Activate pump when a hand is detected.

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


  // Turn off the pump after three seconds.

  turnPumpOff();

  sensors.resetDetection();

  Serial.println(F("[PUMP] OFF - timeout"));

}


// DISPLAY SENSOR READINGS

void displayReadings() {

  Serial.println(F("-------------------------"));


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

  // Disable hand detection while the pump is running.
  // Continue updating sensor readings.

  sensors.update(!pumpOn, true);


  // Check the pump timer.

  checkPumpTime();


  // Check the load cell tare result.

  checkTare();


  // Check for automatic dispensing.

  automaticPump();


  // Display sensor readings every two seconds.

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    if (!pumpOn) {

      displayReadings();

    }

  }

}


// SETUP

void setup() {

  initializePins();

  Serial.begin(9600);

  Serial.println(F("=== HYGIENEMIST ==="));

  sensors.begin();

  lastLive = millis();

}


// MAIN LOOP

void loop() {

  checkPumpTime();

  autoMode();

  checkPumpTime();

}
