const uint8_t PIN_TRIG = A4;
const uint8_t PIN_ECHO = A5;
const unsigned long LIVE_INTERVAL_MS = 2000UL;
const unsigned long ULTRASONIC_READ_INTERVAL_MS = 200UL;
const uint16_t ULTRASONIC_ZERO_MM = 50;
const uint16_t ULTRASONIC_MIN_MM = 20;
const uint16_t ULTRASONIC_MAX_MM = 4000;
const unsigned long ULTRASONIC_TIMEOUT_US = 30000UL;
const unsigned long ULTRASONIC_MIN_INTERVAL_MS = 60UL;
bool ultrasonicZero = false;
int distanceMm = -1;
int distanceFromZeroMm = -1;
unsigned long lastLive = 0;
unsigned long lastUltrasonicRead = 0;
unsigned long lastUltrasonicPing = 0;
bool ultrasonicPinged = false;

void readUltrasonic() {
  unsigned long now = millis();
  if (ultrasonicPinged &&
      now - lastUltrasonicPing < ULTRASONIC_MIN_INTERVAL_MS) {
    return;
  }
  lastUltrasonicPing = now;
  ultrasonicPinged = true;
  distanceMm = -1;
  distanceFromZeroMm = -1;
  ultrasonicZero = false;
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration == 0) return;
  
  unsigned long d = (duration * 343UL + 1000UL) / 2000UL;
  if (d < ULTRASONIC_MIN_MM || d > ULTRASONIC_MAX_MM) return;
  distanceMm = (int)d;
  if (distanceMm <= ULTRASONIC_ZERO_MM) {
    distanceFromZeroMm = 0;
    ultrasonicZero = true;
  } else {
    distanceFromZeroMm = distanceMm - ULTRASONIC_ZERO_MM;
  }
}

void readSensors() {
  readUltrasonic();
}

void setup() {
  Serial.begin(9600);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  Serial.println(F("[ULTRASONIC] HC-SR04 configured"));
  lastLive = millis();
  lastUltrasonicRead = millis() - ULTRASONIC_READ_INTERVAL_MS;
  readSensors();
}

void loop() {
  unsigned long now = millis();
  if (now - lastUltrasonicRead >= ULTRASONIC_READ_INTERVAL_MS) {
    lastUltrasonicRead = now;
    readUltrasonic();
  }
  if (now - lastLive >= LIVE_INTERVAL_MS) {
    lastLive = now;
    readSensors();
    Serial.print(F("[ULTRASONIC] Raw: "));
    Serial.print(distanceMm);
    Serial.print(F(" mm | Adjusted: "));
    Serial.print(distanceFromZeroMm);
    Serial.print(F(" mm"));
    Serial.println();
  }
}
