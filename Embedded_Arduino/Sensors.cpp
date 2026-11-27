#include "Sensors.h"


// NON-CONTACT SENSOR PINS

static const byte sensor1Pin = 5;
static const byte sensor2Pin = 6;


// LOAD CELL PINS

static const byte weightDT = A0;
static const byte weightSCK = A1;


// ULTRASONIC SENSOR PINS

static const byte trigPin = A4;
static const byte echoPin = A5;


// NON-CONTACT SENSOR SETTINGS

static const byte sensor1WetState = LOW;
static const byte sensor2WetState = LOW;


// ULTRASONIC SENSOR SETTINGS

static const int zeroDistance = 90;
static const int resetDistance = 100;


// LOAD CELL SETTINGS

static const int emptyWeight = 100;
static const int fullWeight = 600;

static const int halfWeight =
  (emptyWeight + fullWeight) / 2;

static const float calibration = 420.0;


// SENSOR READING INTERVAL

static const unsigned long sensorTime = 200;


// INITIALIZE SENSORS

void Sensors::begin() {

  pinMode(sensor1Pin, INPUT);

  pinMode(sensor2Pin, INPUT);

  pinMode(trigPin, OUTPUT);

  digitalWrite(trigPin, LOW);

  pinMode(echoPin, INPUT);


  // INITIALIZE LOAD CELL

  scale.begin(weightDT, weightSCK);

  scale.set_scale(calibration);

  lastPing = millis() - sensorTime;

  lastWet = millis() - 5;


  // Keep weighing platform empty
  // until startup tare completes.

  requestTare();

}


// RESET OBJECT DETECTION

void Sensors::resetDetection() {

  detected = 0;

  clear = 0;

}


// REQUEST NEW SENSOR READINGS

void Sensors::requestRead() {

  reading = true;

  freshWeight = false;

  freshWet = false;

  weightCount = 0;

  wetCount = 0;

  votes1 = 0;

  votes2 = 0;

  readStart = millis();

}


// REQUEST LOAD CELL TARE

void Sensors::requestTare() {

  taring = true;

  tareResult = 0;

  tareCount = 0;

  tareSum = 0;

  validWeight = false;

  weightCount = 0;

  tareStart = millis();

}


// GET TARE RESULT

byte Sensors::takeTareResult() {

  byte result = tareResult;

  tareResult = 0;

  return result;

}


// UPDATE ALL SENSORS

void Sensors::update(bool detectHand, bool sampleLevels) {

  if (

    (detectHand || sampleLevels || reading) &&

    millis() - lastPing >= sensorTime

  ) {

    readUltrasonic(detectHand);

  }


  // UPDATE LOAD CELL

  if (taring || sampleLevels || reading) {

    readLoadCell();

  }


  // UPDATE NON-CONTACT SENSORS

  if (sampleLevels || reading) {

    readNonContact();

  }


  // CHECK IF SENSOR READING IS COMPLETE

  if (

    reading &&

    (

      (freshWeight && freshWet) ||

      millis() - readStart >= 1800UL

    )

  ) {

    if (!freshWeight) {

      validWeight = false;

    }

    reading = false;

  }

}


// READ ULTRASONIC SENSOR

void Sensors::readUltrasonic(bool detectHand) {

  if (millis() - lastPing > 1000UL) {

    resetDetection();

  }

  lastPing = millis();


  // SEND ULTRASONIC PULSE

  digitalWrite(trigPin, LOW);

  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);

  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);


  // MEASURE ECHO

  unsigned long duration = pulseIn(

    echoPin,

    HIGH,

    30000UL

  );


  // CONVERT TO MILLIMETERS

  unsigned long mm =

    (duration * 343UL + 1000UL) / 2000UL;


  // RESET CURRENT READING

  rawDistance = -1;

  adjusted = -1;

  near = false;


  // CHECK IF HAND WAS REMOVED

  if (

    detectHand &&

    (

      duration == 0 ||

      (mm > resetDistance && mm <= 4000)

    )

  ) {

    if (clear < 3) {

      clear++;

    }

  } else {

    clear = 0;

  }


  // CHECK INVALID DISTANCE

  if (mm < 20 || mm > 4000) {

    detected = 0;

    return;

  }


  // STORE DISTANCE

  rawDistance = (int)mm;

  near = rawDistance <= zeroDistance;


  // CHECK HAND DETECTION

  if (detectHand && near) {

    if (detected < 3) {

      detected++;

    }

  } else {

    detected = 0;

  }


  // ADJUST DISTANCE BASED ON ZERO DISTANCE

  adjusted = max(0, rawDistance - zeroDistance);

}


// READ LOAD CELL

void Sensors::readLoadCell() {

  unsigned long now = millis();


  // CHECK TARE TIMEOUT

  if (

    taring &&

    now - tareStart >= 2000UL

  ) {

    taring = false;

    tareResult = 2;

    validWeight = false;

    return;

  }


  // CHECK IF HX711 IS READY

  // Collect only one available conversion
  // per call instead of waiting for HX711.

  if (!scale.is_ready()) {

    if (now - lastWeight >= 1200UL) {

      validWeight = false;

    }

    return;

  }

  lastWeight = now;

  long raw = scale.read();


  // CALCULATE TARE OFFSET

  if (taring) {

    tareSum += raw;

    if (++tareCount >= 5) {

      scale.set_offset(tareSum / 5);

      taring = false;

      calibrated = true;

      tareResult = 1;

      weightCount = 0;

    }

    return;

  }


  // PREVENT UNCALIBRATED READINGS

  if (!calibrated) return;


  // COLLECT WEIGHT SAMPLES

  samples[weightCount++] =

    (raw - scale.get_offset()) / calibration;

  if (weightCount < 5) return;


  // MEDIAN FILTER

  // Sort five samples to reject sudden spikes.

  for (byte i = 1; i < 5; i++) {

    float value = samples[i];

    int j = i - 1;

    while (j >= 0 && samples[j] > value) {

      samples[j + 1] = samples[j];

      j--;

    }

    samples[j + 1] = value;

  }


  // USE THE MIDDLE SAMPLE

  grams = samples[2];


  // REMOVE SMALL ZERO FLUCTUATIONS

  if (grams > -2.0 && grams < 2.0) {

    grams = 0;

  }

  weightCount = 0;

  validWeight = true;

  freshWeight = true;

}


// READ NON-CONTACT SENSORS

void Sensors::readNonContact() {

  if (millis() - lastWet < 5UL) return;

  lastWet = millis();


  // CHECK NON-CONTACT SENSOR 1

  if (digitalRead(sensor1Pin) == sensor1WetState) {

    votes1++;

  }


  // CHECK NON-CONTACT SENSOR 2

  if (digitalRead(sensor2Pin) == sensor2WetState) {

    votes2++;

  }


  // COLLECT NINE SAMPLES

  if (++wetCount < 9) return;


  // FILTER NON-CONTACT SENSOR 1

  if (votes1 >= 7) {

    wet1 = true;

  } else if (votes1 <= 2) {

    wet1 = false;

  }


  // FILTER NON-CONTACT SENSOR 2

  if (votes2 >= 7) {

    wet2 = true;

  } else if (votes2 <= 2) {

    wet2 = false;

  }


  // RESET SAMPLE COUNTERS

  wetCount = 0;

  votes1 = 0;

  votes2 = 0;

  freshWet = true;

}


// GET NON-CONTACT LIQUID LEVEL

const __FlashStringHelper *Sensors::liquidLevel() const {

  if (wet1 && wet2) {

    return F("HIGH");

  }

  if (wet1 || wet2) {

    return F("HALF");

  }

  return F("LOW");

}


// GET WEIGHT STATUS

const __FlashStringHelper *Sensors::weightStatus() const {

  if (!validWeight) {

    return F("ERROR");

  }

  if (grams < -5) {

    return F("NEGATIVE");

  }

  if (grams >= fullWeight) {

    return F("FULL");

  }

  if (grams >= halfWeight) {

    return F("HALF");

  }

  return F("LOW");

}


// SENSOR SLEEP

void Sensors::sleep() {

  digitalWrite(trigPin, LOW);

  scale.power_down();

  reading = false;

  taring = false;

  tareResult = 0;

  resetDetection();

}


// SENSOR WAKE

void Sensors::wake() {

  scale.power_up();

  lastPing = millis() - sensorTime;


  // Retain the existing HX711 offset.
  // Do not tare a loaded container on wake.

  if (!calibrated) {

    requestTare();

  }

  requestRead();

}
