// ESP-01 side of the water monitoring system
//
// IMPORTANT: Serial (GPIO1/GPIO3) is wired to the Arduino Pro Mini, so this
// sketch must NOT print debug text on Serial. Only protocol lines are sent.
//
// ESP-01 -> Arduino : WIFI,1 / WIFI,0            (every 1 s)
//                     TIME,yyyy,mm,dd,hh,mm,ss   (every 60 s once NTP is synced)
//                     ACK,<record id>            (after the record is in Firebase)
// Arduino -> ESP-01 : DATA,<id>,<date>,<time>,<tof>,<hx711>,<float>,<w1>,<w2>,<relay>
//
// Each record is stored at /records/<id> with PUT (setJSON), so a record that
// is re-sent after a missing ACK overwrites itself and never duplicates.

#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <time.h>

// ---------- Settings ----------
const char* ssid     = "Converge_2.4G_v3D8";
const char* password = "pUsfd9z3";

#define API_KEY       "AIzaSyDXMgYgjPJ7BDXhO2Tb-9H_StmtwVFW7k4"
#define DATABASE_URL  "https://e3embeddedsystem-4e47e-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL    "jb.r.on.fi.g.02@gmail.com"
#define USER_PASSWORD "HOTDOG1234"

#define TZ_OFFSET     (8 * 3600)   // UTC+8
#define TIME_EVERY_MS 60000UL

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseJson json;

char line[100];
byte lineLen = 0;
unsigned long lastBeat = 0;
unsigned long lastTime = 0;
bool timeSent = false;

// ---------- Upload one record: "id,date,time,tof,hx,float,w1,w2,relay" ----------
void uploadRecord(char *rec) {
  char *f[9];
  byte n = 0;
  f[n++] = rec;
  for (char *p = rec; *p && n < 9; p++) {
    if (*p == ',') {
      *p = '\0';
      f[n++] = p + 1;
    }
  }
  if (n < 9) return;                       // incomplete line, ignore

  json.clear();
  json.set("date",   f[1]);
  json.set("time",   f[2]);
  json.set("tof_mm", (int)atol(f[3]));
  json.set("hx711",  (int)atol(f[4]));
  json.set("float",  (int)atol(f[5]));
  json.set("water1", (int)atol(f[6]));
  json.set("water2", (int)atol(f[7]));
  json.set("relay",  (int)atol(f[8]));

  String path = String("/records/") + f[0];

  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.print(F("ACK,"));
    Serial.println(f[0]);
  }
  // On failure nothing is sent: the Arduino resends after its timeout.
}

// ---------- Send current time to the Arduino ----------
void sendTime() {
  time_t now = time(nullptr);
  if (now < 1700000000UL) return;          // NTP not synced yet

  now += TZ_OFFSET;
  struct tm *t = gmtime(&now);
  Serial.printf("TIME,%d,%d,%d,%d,%d,%d\n",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec);
  lastTime = millis();
  timeSent = true;
}

void setup() {
  Serial.setRxBufferSize(1024);            // room for records while uploading
  Serial.begin(9600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // offset added in sendTime()

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.timeout.serverResponse = 5000;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  bool online = (WiFi.status() == WL_CONNECTED) && Firebase.ready();

  // Heartbeat
  if (millis() - lastBeat >= 1000) {
    lastBeat = millis();
    Serial.println(online ? F("WIFI,1") : F("WIFI,0"));
  }

  static bool ipSent = false;
if (WiFi.status() == WL_CONNECTED) {
  if (!ipSent) {
    Serial.print(F("IP,"));
    Serial.println(WiFi.localIP());
    ipSent = true;
  }
} else {
  ipSent = false;
}

  // Time for the Arduino clock
  if (WiFi.status() == WL_CONNECTED &&
      (!timeSent || millis() - lastTime >= TIME_EVERY_MS)) {
    sendTime();
  }

  // Records from the Arduino
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[lineLen] = '\0';
      lineLen = 0;
      if (online && strncmp(line, "DATA,", 5) == 0) {
        uploadRecord(line + 5);
      }
    } else if (c != '\r' && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}
