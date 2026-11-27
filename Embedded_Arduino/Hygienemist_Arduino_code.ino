#include <Arduino.h>

// PIN ASSIGNMENTS

const byte wakePin = 3;
const byte pumpPin = 7;

const byte espRx = 9;
const byte espTx = 8;

const byte sdPin = 10;


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


// SETUP

void setup() {

  initializePins();

  Serial.begin(9600);

  Serial.println(F("=== HYGIENEMIST ==="));

}


// MAIN LOOP

void loop() {

}
