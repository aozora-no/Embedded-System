#include "_sevenSegment.h" // Calls out the class from the .h file

const byte segPins[7] = {13, 12, 11, 10, 9, 8, 7}; // 7S pins
const byte BT1 = 6, BT2 = 5, BT3 = 4, BT4 = 3, BT5 = 2; // Button pins

SevenSegment display(segPins); // Creates the display object from the class
Buttons buttons(BT1, BT2, BT3, BT4, BT5); // Creates the button object from the class

int value = 0; // Stores the current value

enum AutoMode { AUTO_NONE, AUTO_UP, AUTO_DOWN };
AutoMode autoMode = AUTO_NONE;
bool autoRunning = false;

// millis() variable
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

// Checks if BT1 is pressed and controls Increment
  if (buttons.bt1Pressed()) { 
    if (value < 9) value++; 
      display.showDigit(value); 
      Serial.print("BT1"); 
      Serial.println(value);
    }

  // Checks if BT2 is pressed and controls Decrement
  if (buttons.bt2Pressed()) { 
    if (value > 0) value--;
     display.showDigit(value); 
     Serial.print("BT2"); 
      Serial.println(value);
  }

// Checks if BT3 is pressed and controls Automatic Up
  if (buttons.bt3Pressed()) {
    if (autoMode != AUTO_UP || !autoRunning) {
      autoMode = AUTO_UP;
      autoRunning = true;
      lastStepMs = millis();  
  }
  }

// Checks if BT4 is pressed and controls Automatic Down
  if (buttons.bt4Pressed()) {
    if (autoMode != AUTO_DOWN || !autoRunning) {
      autoMode = AUTO_DOWN;
      autoRunning = true;
      lastStepMs = millis();   
  }
    
// Checks if BT5 is pressed and controls Play or Pause
  if (buttons.bt5Pressed()) {
    if (autoMode != AUTO_NONE) {
      autoRunning = !autoRunning;
      lastStepMs = millis();
    }
  }

  if (autoRunning && (millis() - lastStepMs >= autoInterval)) {
    lastStepMs = millis();

    if (autoMode == AUTO_UP) { // Checks if the mode is automatic up
      if (value < 9) value++; // increase value automatically
      else autoRunning = false;
    } else if (autoMode == AUTO_DOWN) { // Checks if the mode is automatic down
      if (value > 0) value--; // decrease value automatically
      else autoRunning = false;
    }

    display.showDigit(value); // Shows the value on the display
  }

  delay(20);
}
