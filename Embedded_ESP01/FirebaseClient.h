#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

class FirebaseClient {
public:
  void begin();
  bool login();
  bool ready() const;
  bool request(const char *method, const String &path,
               const String &payload, String &answer);
  const String &error() const { return firebaseError; }
  int httpCode() const { return lastHttpCode; }

private:
  String authToken;
  uint32_t loginTime = 0;
  String firebaseError;
  int lastHttpCode = 0;
  static constexpr uint32_t tokenLifetime = 3000000UL;
  int firebaseHttp(const char *method, const String &url,
                   const String &body, String &reply, uint16_t timeout);
  bool signInFirebase(const String &email, const String &password,
                      String &token, uint16_t timeout);
};
