#pragma once
#include <Arduino.h>

class ArduinoLink {
public:
  void begin();
  char *readLine();
  HardwareSerial &output() { return Serial; }
  void wake();
  static uint16_t crc16(const char *p);

private:
  char serialLine[160];
  size_t serialLength = 0;
  bool serialOverflow = false;
};
