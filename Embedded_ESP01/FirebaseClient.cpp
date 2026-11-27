#include "FirebaseClient.h"

#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>

// Private Wi-Fi and Firebase settings. Do not publish.
static const char wifiName[] = "Converge_2.4GHz_v3D8";
static const char wifiPassword[] = "pUsfd9z3";
static const char firebaseKey[] = "AIzaSyDXMgYgjPJ7BDXhO2Tb-9H_StmtwVFW7k4";
static const char firebaseEmail[] = "jb.r.on.fi.g.02@gmail.com";
static const char firebasePassword[] = "HOTDOG1234";
static const char databaseUrl[] = "https://e3embeddedsystem-4e47e-default-rtdb.asia-southeast1.firebasedatabase.app";
static const char deviceId[] = "HM";

bool FirebaseClient::ready() const {
  return authToken.length() && millis() - loginTime < tokenLifetime;
}

int FirebaseClient::firebaseHttp(const char *method, const String &url,
                 const String &body, String &reply, uint16_t timeout) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Local prototype; certificate validation is not enabled.
  HTTPClient http;
  http.setTimeout(timeout);
  if (!http.begin(client, url)) return -1;
  int status;
  if (strcmp(method, "GET") == 0) {
    status = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    status = strcmp(method, "PUT") == 0 ? http.PUT(body) : http.POST(body);
  }
  if (status == 200) reply = http.getString();
  http.end();
  return status;
}

bool FirebaseClient::signInFirebase(const String &email, const String &password,
                    String &token, uint16_t timeout) {
  StaticJsonDocument<512> request;
  request["email"] = email;
  request["password"] = password;
  request["returnSecureToken"] = true;
  String body, reply;
  serializeJson(request, body);
  String url = F("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=");
  url += firebaseKey;
  if (firebaseHttp("POST", url, body, reply, timeout) != 200) return false;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, reply)) return false;
  token = doc["idToken"] | "";
  return token.length() != 0;
}

bool FirebaseClient::login() {
  if (!signInFirebase(firebaseEmail, firebasePassword, authToken, 10000)) return false;
  loginTime = millis();
  return true;
}

bool FirebaseClient::request(const char *method, const String &path,
                     const String &payload, String &answer) {
  if (!authToken.length()) return false;
  String url = String(databaseUrl) + "/devices/" + deviceId + "/" + path + ".json?auth=" + authToken;
  uint16_t timeout = (path == "command" || path == "status") ? 4000 : 10000;
  int status = firebaseHttp(method, url, payload, answer, timeout);
  lastHttpCode = status;
  // Successful status updates must not hide an outstanding log failure.
  if (status != 200) firebaseError = String(method) + " " + path + " HTTP " + String(status);
  else if (firebaseError.startsWith(String(method) + " " + path + " HTTP ")) firebaseError = "";
  if (status == 401 || status == 403) authToken = "";
  return status == 200;
}

void FirebaseClient::begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(
    wifiName,
    wifiPassword
  );
  // Get internet date and time using NTP
  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.google.com"
  );
}

