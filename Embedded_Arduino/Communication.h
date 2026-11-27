#ifndef HYGIENEMIST_COMMUNICATION_H
#define HYGIENEMIST_COMMUNICATION_H

#include <Arduino.h>
#include <SoftwareSerial.h>


// COMMUNICATION CLASS

class Communication {

public:

  // SUPPORTED COMMANDS

  enum Command {

    GET,

    BASE,

    IP,

    AUTO,

    MAINT,

    SLEEP,

    READ,

    TARE,

    PUMP1,

    SDPAGE,

    HPAGE,

    HWRITE

  };


  // COMMAND HANDLER

  typedef void (*Handler)(
    Command,
    unsigned long,
    unsigned long,
    byte,
    const char *
  );


  // CONSTRUCTOR

  Communication(byte rx, byte tx)
    : serial(rx, tx) {}


  // INITIALIZE SERIAL COMMUNICATION

  void begin(unsigned long baud) {

    serial.begin(baud);

  }


  // RECEIVE COMMANDS

  void update(Handler handler);


  // SERIAL OUTPUT

  Print &output() {

    return serial;

  }


  // SEND ACKNOWLEDGMENT

  void ack(const __FlashStringHelper *command);


  // SEND LIVE SENSOR DATA

  void live(void (*writer)(Print &, bool, bool));


  // SEND SAVED RECORD

  void record(const char *row);


  // SEND PAGINATED RECORD

  void pageRow(
    unsigned long ticket,
    byte kind,
    unsigned long cursor,
    const char *row
  );


  // END PAGE TRANSMISSION

  void pageEnd(
    unsigned long ticket,
    bool more
  );


  // SEND PAGE ERROR

  void pageError(
    unsigned long ticket,
    const __FlashStringHelper *reason
  );


  // HISTORY ACKNOWLEDGMENT

  void historyAck(const char *row);


  // RESET SERIAL INPUT

  void resetInput();


private:

  // SOFTWARE SERIAL OBJECT

  SoftwareSerial serial;


  // SERIAL BUFFER

  char buffer[72];

  byte length = 0;

  bool invalid = false;


  // COMMAND PARSER

  void dispatch(
    const char *line,
    Handler handler
  );


  // CHECK NUMERIC VALUE

  static bool isNumber(const char *value);


  // CALCULATE CHECKSUM

  static uint16_t crc(const char *data);


  // SEND RECORD WITH CHECKSUM

  void checkedRow(const char *row);

};

#endif
