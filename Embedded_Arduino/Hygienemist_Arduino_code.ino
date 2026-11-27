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


// TIMING VARIABLES

const unsigned long liveTime = 2000;

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

}


// CHECK TARE RESULT

void checkTare() {

  byte result = sensors.takeTareResult();

  if (result == 1) {

    Serial.println(F("[HX711] Tare successful"));

  } else if (result == 2) {

    Serial.println(F("[HX711] Tare timeout"));

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

  sensors.update(true, true);

  checkTare();

  if (millis() - lastLive >= liveTime) {

    lastLive = millis();

    displayReadings();

  }

}
