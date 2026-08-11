#include "_sevenSegment.h"
//hello test -jastien
const byte segPins[7] = {13, 12, 11, 10, 9, 8, 7};
const byte BT1 = 6, BT2 = 5, BT3 = 4, BT4 = 3, BT5 = 2;

SevenSegment display(segPins);
Buttons buttons(BT1, BT2, BT3, BT4, BT5);

int value = 0;
int test = 1;

enum AutoMode { AUTO_NONE, AUTO_UP, AUTO_DOWN };
AutoMode autoMode = AUTO_NONE;
bool autoRunning = false;

unsigned long lastStepMs = 0;
const unsigned long autoInterval = 500;

void setup() {
  Serial.begin(9600);
  display.begin();
  display.setCommonAnode(true); // change the 7S if its anode or cathode
  buttons.begin();
  display.showDigit(value);
}

void loop() {
  if (buttons.bt1Pressed()) { 
    if (value < 9) value++; 
      display.showDigit(value); 
      Serial.print("BT1"); 
      Serial.println(value);
    }

  if (buttons.bt2Pressed()) { 
    if (value > 0) value--;
     display.showDigit(value); 
     Serial.print("BT2"); 
      Serial.println(value);
  }
  if (buttons.bt3Pressed()) {
    if (autoMode != AUTO_UP || !autoRunning) {
      autoMode = AUTO_UP;
      autoRunning = true;
      lastStepMs = millis();  
  }
  }

  if (buttons.bt4Pressed()) {
    if (autoMode != AUTO_DOWN || !autoRunning) {
      autoMode = AUTO_DOWN;
      autoRunning = true;
      lastStepMs = millis();   
  }

  if (buttons.bt5Pressed()) {
    if (autoMode != AUTO_NONE) {
      autoRunning = !autoRunning;
      lastStepMs = millis();
    }
  }

  if (autoRunning && (millis() - lastStepMs >= autoInterval)) {
    lastStepMs = millis();

    if (autoMode == AUTO_UP) {
      if (value < 9) value++;
      else autoRunning = false;
    } else if (autoMode == AUTO_DOWN) {
      if (value > 0) value--;
      else autoRunning = false;
    }

    display.showDigit(value);
  }

  delay(20);
}