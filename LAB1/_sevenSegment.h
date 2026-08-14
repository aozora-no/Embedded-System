#ifndef _SEVENSEGMENT_H
#define _SEVENSEGMENT_H

#include <Arduino.h>

class SevenSegment {
public:
  SevenSegment(const byte segPins[7]); // Constructor

  void begin(); // Initialize or set pinmode
  void showDigit(byte d); // Show display 
  void setCommonAnode(bool enabled); // enables the anode 

private:
  byte _segPins[7]; // store pin number a-g
  bool _commonAnode = false;
  static const byte _digitMap[10][7]; // Checks digit patterns
};

class Buttons {
public:
  Buttons(byte b1, byte b2, byte b3, byte b4, byte b5); // Constructorr

  void begin(); // Set pinmode 

  // Checks if a button is pressed
  bool bt1Pressed(); 
  bool bt2Pressed();
  bool bt3Pressed();
  bool bt4Pressed();
  bool bt5Pressed();

private:
  byte _b1, _b2, _b3, _b4, _b5; // Pin numbers
  bool _prev1, _prev2, _prev3, _prev4, _prev5; // State for debouncing

  bool pressedNow(byte pin, bool &prevState);
};

#endif
