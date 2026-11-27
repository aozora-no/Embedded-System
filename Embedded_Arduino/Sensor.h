#ifndef HYGIENEMIST_SENSORS_H
#define HYGIENEMIST_SENSORS_H

#include <Arduino.h>
#include <HX711.h>


// SENSOR CLASS

class Sensors {

public:

  void begin();

  void update(bool detectHand, bool sampleLevels);

  void resetDetection();


  // SENSOR READING

  void requestRead();

  bool readingDone() const {
    return !reading;
  }


  // LOAD CELL TARE

  void requestTare();

  // 0 = no result
  // 1 = success
  // 2 = timeout

  byte takeTareResult();

  bool isTaring() const {
    return taring;
  }


  // POWER MANAGEMENT

  void sleep();

  void wake();


  // ULTRASONIC SENSOR

  int distance() const {
    return rawDistance;
  }

  int adjustedDistance() const {
    return adjusted;
  }

  bool nearObject() const {
    return near;
  }

  bool handDetected() const {
    return detected >= 3;
  }

  bool handRemoved() const {
    return clear >= 3;
  }


  // NON-CONTACT SENSORS

  bool nonContact1() const {
    return wet1;
  }

  bool nonContact2() const {
    return wet2;
  }


  // LOAD CELL

  float weight() const {
    return grams;
  }

  bool weightReady() const {
    return validWeight;
  }


  // SENSOR STATUS

  const __FlashStringHelper *liquidLevel() const;

  const __FlashStringHelper *weightStatus() const;


private:

  HX711 scale;


  // ULTRASONIC VARIABLES

  int rawDistance = -1;
  int adjusted = -1;

  bool near = false;

  byte detected = 0;
  byte clear = 0;


  // NON-CONTACT VARIABLES

  bool wet1 = false;
  bool wet2 = false;

  byte wetCount = 0;
  byte votes1 = 0;
  byte votes2 = 0;


  // LOAD CELL VARIABLES

  bool validWeight = false;

  float grams = 0;

  float samples[5];

  byte weightCount = 0;


  // SENSOR READING VARIABLES

  bool reading = false;

  bool freshWeight = false;
  bool freshWet = false;


  // TARE VARIABLES

  bool taring = false;
  bool calibrated = false;

  byte tareResult = 0;
  byte tareCount = 0;

  long tareSum = 0;


  // TIMING VARIABLES

  unsigned long lastPing = 0;
  unsigned long lastWet = 0;
  unsigned long lastWeight = 0;

  unsigned long readStart = 0;
  unsigned long tareStart = 0;


  // SENSOR FUNCTIONS

  void readUltrasonic(bool detectHand);

  void readLoadCell();

  void readNonContact();

};

#endif
