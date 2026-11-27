#include "ArduinoLink.h"

// Serial and wake settings
static const uint8_t wakePin = 2;
static const uint32_t baudRate = 9600;


void ArduinoLink::begin() {
  digitalWrite(wakePin, HIGH);
  pinMode(wakePin, OUTPUT);
  Serial.setRxBufferSize(1024);
  Serial.begin(baudRate);
}

char *ArduinoLink::readLine() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      bool valid = serialLength > 0 && !serialOverflow;
      if (valid) serialLine[serialLength] = '\0';
      serialLength = 0;
      serialOverflow = false;
      if (valid) return serialLine;
    } else if (c >= 32 && c <= 126 && !serialOverflow) {
      if (serialLength < sizeof(serialLine) - 1) {
        serialLine[serialLength++] = (char)c;
      } else {
        serialOverflow = true;
      }
    } else {
      serialLength = 0;
      serialOverflow = true;
    }
  }
  return nullptr;
}

void ArduinoLink::wake() {
  digitalWrite(wakePin, LOW);
  delay(180);
  digitalWrite(wakePin, HIGH);
}

uint16_t ArduinoLink::crc16(const char *p) {
  uint16_t crc = 0xFFFF;
  while (*p) {
    crc ^= (uint16_t)(uint8_t)(*p++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x8000)
        ? (uint16_t)((crc << 1) ^ 0x1021)
        : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

