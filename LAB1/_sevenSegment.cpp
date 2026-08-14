#include "_sevenSegment.h" // Include header file
#include <Arduino.h> // Include Arduino Library

//SevenSegment
const byte SevenSegment::_digitMap[10][7] = {
  //a,b,c,d,e,f,g
  {1,1,1,1,1,1,0}, // 0
  {0,1,1,0,0,0,0}, // 1
  {1,1,0,1,1,0,1}, // 2
  {1,1,1,1,0,0,1}, // 3
  {0,1,1,0,0,1,1}, // 4
  {1,0,1,1,0,1,1}, // 5
  {1,0,1,1,1,1,1}, // 6
  {1,1,1,0,0,0,0}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}  // 9
};

// Constructor - saves the pin numbers
SevenSegment::SevenSegment(const byte segPins[7]) {
  for (byte i = 0; i < 7; i++) _segPins[i] = segPins[i];
}

// Sets all segment pins as outputs
void SevenSegment::begin() {
  for (byte i = 0; i < 7; i++) pinMode(_segPins[i], OUTPUT);
}

// Show a digit on the display
void SevenSegment::showDigit(byte d) {
  if (d > 9) return; // Only digits up to 9 can be displayed
  for (byte i = 0; i < 7; i++) {
    bool segOn = _digitMap[d][i]; // Check if segment should be ON
    digitalWrite(_segPins[i], _commonAnode ? (segOn ? LOW : HIGH)
                                           : (segOn ? HIGH : LOW));
  }
}

// Display type
 void SevenSegment::setCommonAnode(bool enabled) {
  _commonAnode = enabled;
}

// Buttons
Buttons::Buttons(byte b1, byte b2, byte b3, byte b4, byte b5)
  : _b1(b1), _b2(b2), _b3(b3), _b4(b4), _b5(b5),
    _prev1(HIGH), _prev2(HIGH), _prev3(HIGH), _prev4(HIGH), _prev5(HIGH) {}

// Buttons set as inputs with pull-up resistor
void Buttons::begin() {
  pinMode(_b1, INPUT_PULLUP);
  pinMode(_b2, INPUT_PULLUP);
  pinMode(_b3, INPUT_PULLUP);
  pinMode(_b4, INPUT_PULLUP);
  pinMode(_b5, INPUT_PULLUP);
}

// Detects button press
bool Buttons::pressedNow(byte pin, bool &prevState) {
  bool cur = digitalRead(pin);
  bool pressed = (prevState == HIGH && cur == LOW);
  prevState = cur;
  return pressed;
}

// Button press checker functions - returns true if button was just pressed
bool Buttons::bt1Pressed() { return pressedNow(_b1, _prev1); }
bool Buttons::bt2Pressed() { return pressedNow(_b2, _prev2); }
bool Buttons::bt3Pressed() { return pressedNow(_b3, _prev3); }
bool Buttons::bt4Pressed() { return pressedNow(_b4, _prev4); }
bool Buttons::bt5Pressed() { return pressedNow(_b5, _prev5); }
