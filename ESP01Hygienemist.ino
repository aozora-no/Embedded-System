
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ctype.h>

// ==========================================
// CONFIGURATION
// ==========================================

// ==========================================
// WIFI AND FIREBASE CONFIGURATION
// ==========================================

const char WIFI_SSID[] = "HUAWEI-2.4G-5Fb6";
const char WIFI_PASSWORD[] = "sDzZc6N6";

const char FIREBASE_API_KEY[] = "AIzaSyDXMgYgjPJ7BDXhO2Tb-9H_StmtwVFW7k4";
const char FIREBASE_USER_EMAIL[] = "jb.r.on.fi.g.02@gmail.com";
const char FIREBASE_USER_PASSWORD[] = "HOTDOG1234";

const char FIREBASE_DATABASE_URL[] =
  "https://e3embeddedsystem-4e47e-default-rtdb.asia-southeast1.firebasedatabase.app";

const char DEVICE_ID[] = "WM001";


const uint8_t ARDUINO_WAKE_PIN = 2;
const uint32_t SERIAL_BAUD = 9600;
const uint32_t IP_INTERVAL_MS = 10000UL;
const uint32_t SERIAL_RETRY_MS = 4500UL;
const uint32_t NETWORK_RETRY_MS = 6000UL;
const uint32_t NO_NEW_ROW_POLL_MS = 2500UL;
const uint32_t SD_ERROR_POLL_MS = 7000UL;
const uint32_t AUTH_VALID_MS = 3000000UL;
const uint32_t LIVE_STALE_MS = 12000UL;

ESP8266WebServer web(80);

// ==========================================
// STATE
// ==========================================

String authToken;
String rowJson;

uint32_t authTime = 0;
uint32_t lastIpSent = 0;
uint32_t nextActionAt = 0;
uint32_t lastRowRequest = 0;

uint32_t nextId = 1;
uint32_t rowId = 0;
uint32_t lastReadingAt = 0;

bool cursorLoaded = false;
bool haveIp = false;
bool liveValid = false;

uint8_t serialRetries = 0;

// 0 = AUTO
// 1 = MAINTENANCE
// 2 = SLEEP

uint8_t mode = 0;

long liveUptime = 0;
long liveDistance = -1;

float liveWeight = -999;

int liveFloat = 0;
int liveWater1 = 0;
int liveWater2 = 0;
int liveRelay = 0;

String lastAck = "";
String lastError = "";

uint64_t lastChangeEpochMs = 0;

// ==========================================
// SLEEP LOGGING
// ==========================================

String sleepId = "";

uint64_t sleepStartedEpochMs = 0;
uint64_t sleepEndedEpochMs = 0;

uint32_t sleepStartedMillis = 0;
uint32_t sleepDurationMs = 0;

bool sleepEventPending = false;
bool sleepCompleted = false;

uint32_t nextSleepUploadAt = 0;

// ==========================================
// SD SYNCHRONIZATION
// ==========================================

enum SyncState {
  WANT_ROW,
  WAIT_ROW,
  UPLOAD_ROW,
  COMMIT_ID
};

SyncState syncState = WANT_ROW;

char receiveLine[160];

size_t receiveLen = 0;
bool receiveOverflow = false;

// ==========================================
// HELPERS
// ==========================================

bool due(uint32_t target) {
  return (int32_t)(millis() - target) >= 0;
}

uint64_t epochMs() {
  time_t now = time(nullptr);

  if (now > 1700000000) {
    return (uint64_t)now * 1000ULL;
  }

  return 0ULL;
}

String uint64String(uint64_t n) {
  char buf[24];

  snprintf(
    buf,
    sizeof(buf),
    "%llu",
    (unsigned long long)n
  );

  return String(buf);
}

// ==========================================
// WIFI
// ==========================================

void announceIp() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print(F("IP,"));
  Serial.println(WiFi.localIP());

  haveIp = true;
  lastIpSent = millis();
}

bool wifiOnline() {
  if (WiFi.status() != WL_CONNECTED) {
    haveIp = false;
    return false;
  }

  if (!haveIp ||
      millis() - lastIpSent >= IP_INTERVAL_MS) {
    announceIp();
  }

  return true;
}

// ==========================================
// WAKE ARDUINO
// ==========================================

void pulseArduinoWake() {
  digitalWrite(ARDUINO_WAKE_PIN, LOW);
  delay(180);
  digitalWrite(ARDUINO_WAKE_PIN, HIGH);
}

// ==========================================
// CRC16
// ==========================================

uint16_t crc16(const char *p) {
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

// ==========================================
// VALIDATION
// ==========================================

bool parseUnsigned(const char *s, uint32_t &out) {
  if (!s || !*s) return false;

  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9') return false;
  }

  char *end;

  unsigned long n = strtoul(s, &end, 10);

  if (*end) return false;

  out = (uint32_t)n;
  return true;
}

bool parseSigned(const char *s) {
  if (!s || !*s) return false;

  if (*s == '-') s++;
  if (!*s) return false;

  while (*s) {
    if (*s < '0' || *s > '9') return false;
    s++;
  }

  return true;
}

bool parseFloatString(const char *s) {
  if (!s || !*s) return false;

  if (*s == '-') s++;

  bool dot = false;
  bool digit = false;

  while (*s) {
    if (*s == '.') {
      if (dot) return false;
      dot = true;
    }
    else if (*s >= '0' && *s <= '9') {
      digit = true;
    }
    else {
      return false;
    }

    s++;
  }

  return digit;
}

bool binaryFlag(const char *s) {
  return strcmp(s, "0") == 0 ||
         strcmp(s, "1") == 0;
}

// ==========================================
// PARSE SD LOG
// ==========================================

bool parseLogRow(
  char *line,
  uint32_t expectedId,
  String &json
) {
  if (strncmp(line, "L,", 2) != 0) return false;

  char *csv = line + 2;
  char *star = strrchr(csv, '*');

  if (!star || strlen(star + 1) != 4) {
    return false;
  }

  for (char *p = star + 1; *p; p++) {
    if (!isxdigit((unsigned char)*p)) {
      return false;
    }
  }

  unsigned long rx = strtoul(star + 1, nullptr, 16);

  *star = '\0';

  if (crc16(csv) != (uint16_t)rx) {
    return false;
  }

  char *fields[8];
  char *p = csv;

  for (int i = 0; i < 8; i++) {
    fields[i] = p;

    if (i < 7) {
      char *comma = strchr(p, ',');

      if (!comma) return false;

      *comma = '\0';
      p = comma + 1;
    }
    else if (strchr(p, ',')) {
      return false;
    }
  }

  uint32_t parsedId = 0;
  uint32_t uptime = 0;

  if (!parseUnsigned(fields[0], parsedId) ||
      parsedId != expectedId ||
      !parseUnsigned(fields[1], uptime) ||
      !parseSigned(fields[2]) ||
      !(strcmp(fields[3], "ERR") == 0 ||
        parseFloatString(fields[3]))) {
    return false;
  }

  for (int i = 4; i < 8; i++) {
    if (!binaryFlag(fields[i])) return false;
  }

  json = F("{\"id\":");
  json += String(parsedId);

  json += F(",\"uptime_s\":");
  json += String(uptime);

  json += F(",\"distance_mm\":");
  json += fields[2];

  json += F(",\"weight\":");

  if (strcmp(fields[3], "ERR") == 0) {
    json += F("null");
  } else {
    json += fields[3];
  }

  json += F(",\"float\":");
  json += fields[4];

  json += F(",\"water1\":");
  json += fields[5];

  json += F(",\"water2\":");
  json += fields[6];

  json += F(",\"relay\":");
  json += fields[7];

  json += F(",\"uploaded_at\":{\".sv\":\"timestamp\"}}");

  return true;
}

// ==========================================
// MODE MANAGEMENT
// ==========================================

void setMode(uint8_t newMode) {
  if (mode == newMode) return;

  mode = newMode;

  lastChangeEpochMs = epochMs();

  if (mode == 2) {
    syncState = WANT_ROW;
  }
}

// ==========================================
// LIVE SENSOR DATA
// ==========================================

void processLive(char *line) {
  char *fields[8];
  char *saveptr = nullptr;

  char *p = strtok_r(line + 2, ",", &saveptr);

  for (int i = 0; i < 8; i++) {
    if (!p) return;

    fields[i] = p;
    p = strtok_r(nullptr, ",", &saveptr);
  }

  uint32_t parsedUptime = 0;

  if (p ||
      !parseUnsigned(fields[0], parsedUptime) ||
      !parseSigned(fields[1]) ||
      !parseFloatString(fields[2])) {
    return;
  }

  for (int i = 3; i < 7; i++) {
    if (!binaryFlag(fields[i])) return;
  }

  if (strcmp(fields[7], "0") != 0 &&
      strcmp(fields[7], "1") != 0) {
    return;
  }

  liveUptime = atol(fields[0]);
  liveDistance = atol(fields[1]);
  liveWeight = atof(fields[2]);

  liveFloat = atoi(fields[3]);
  liveWater1 = atoi(fields[4]);
  liveWater2 = atoi(fields[5]);
  liveRelay = atoi(fields[6]);

  lastReadingAt = millis();
  liveValid = true;

  setMode((uint8_t)atoi(fields[7]));
}

// ==========================================
// ARDUINO RESPONSES
// ==========================================

void handleArduinoLine(char *line) {

  if (strcmp(line, "BOOT") == 0) {
    announceIp();

    if (cursorLoaded) {
      Serial.print(F("BASE,"));
      Serial.println(nextId - 1);
    }

    return;
  }

  if (strncmp(line, "D,", 2) == 0) {
    processLive(line);
    return;
  }

  if (strncmp(line, "ACK,", 4) == 0) {
    lastAck = String(line + 4);

    if (lastAck == "AUTO") {
      setMode(0);
    }
    else if (lastAck == "MAINT") {
      setMode(1);
      liveRelay = 0;
    }
    else if (lastAck == "SLEEP") {
      setMode(2);

      sleepStartedMillis = millis();
      sleepStartedEpochMs = epochMs();

      sleepEndedEpochMs = 0;
      sleepDurationMs = 0;

      sleepCompleted = false;

      sleepId = uint64String(
        sleepStartedEpochMs
          ? sleepStartedEpochMs
          : (uint64_t)millis()
      );

      sleepEventPending = true;
      nextSleepUploadAt = millis();
    }
    else if (lastAck == "WAKE") {
      sleepDurationMs = millis() - sleepStartedMillis;
      sleepEndedEpochMs = epochMs();

      sleepCompleted = true;

      setMode(0);

      sleepEventPending = true;
      nextSleepUploadAt = millis();
    }
    else if (lastAck == "RELAY1") {
      liveRelay = 1;
    }
    else if (lastAck == "RELAY0") {
      liveRelay = 0;
    }

    return;
  }

  if (syncState != WAIT_ROW || mode == 2) {
    return;
  }

  if (strncmp(line, "L,", 2) == 0) {
    String parsed;

    if (parseLogRow(line, nextId, parsed)) {
      rowJson = parsed;
      rowId = nextId;

      syncState = UPLOAD_ROW;

      serialRetries = 0;
      nextActionAt = millis();
    }

    return;
  }

  uint32_t reported = 0;

  if (strncmp(line, "END,", 4) == 0 &&
      parseUnsigned(line + 4, reported) &&
      reported == nextId) {

    syncState = WANT_ROW;
    serialRetries = 0;

    nextActionAt = millis() + NO_NEW_ROW_POLL_MS;
  }
  else if (strcmp(line, "ERR,SD") == 0) {
    syncState = WANT_ROW;
    serialRetries = 0;

    nextActionAt = millis() + SD_ERROR_POLL_MS;

    lastError = "Arduino SD card unavailable";
  }
  else if (strncmp(line, "GAP,", 4) == 0 &&
           parseUnsigned(line + 4, reported) &&
           reported == nextId) {

    syncState = WANT_ROW;
    serialRetries = 0;

    nextActionAt = millis() + SD_ERROR_POLL_MS;

    lastError = "Missing SD record ID " + String(nextId);
  }
}

// ==========================================
// SERIAL RECEIVE
// ==========================================

void serviceSerial() {
  while (Serial.available()) {
    int c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (receiveLen > 0 && !receiveOverflow) {
        receiveLine[receiveLen] = '\0';
        handleArduinoLine(receiveLine);
      }

      receiveLen = 0;
      receiveOverflow = false;
    }
    else if (c >= 32 && c <= 126 && !receiveOverflow) {
      if (receiveLen < sizeof(receiveLine) - 1) {
        receiveLine[receiveLen++] = (char)c;
      }
      else {
        receiveOverflow = true;
      }
    }
    else {
      receiveLen = 0;
      receiveOverflow = true;
    }
  }
}

// ==========================================
// REQUEST SD RECORD
// ==========================================

void sendRowRequest() {
  Serial.print(F("GET,"));
  Serial.println(nextId);

  lastRowRequest = millis();
  syncState = WAIT_ROW;
}

// ==========================================
// FIREBASE AUTHENTICATION
// ==========================================

bool signIn() {
  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);

  String url =
    F("https://identitytoolkit.googleapis.com/v1/"
      "accounts:signInWithPassword?key=");

  url += FIREBASE_API_KEY;

  if (!http.begin(client, url)) return false;

  http.addHeader(
    F("Content-Type"),
    F("application/json")
  );

  StaticJsonDocument<384> request;

  request["email"] = FIREBASE_USER_EMAIL;
  request["password"] = FIREBASE_USER_PASSWORD;
  request["returnSecureToken"] = true;

  String body;
  serializeJson(request, body);

  int status = http.POST(body);

  String response;

  if (status == HTTP_CODE_OK) {
    response = http.getString();
  }

  http.end();

  if (status != HTTP_CODE_OK) return false;

  DynamicJsonDocument result(4096);

  if (deserializeJson(result, response)) {
    return false;
  }

  const char *token = result["idToken"] | "";

  if (!*token) return false;

  authToken = token;
  authTime = millis();

  return true;
}

// ==========================================
// FIREBASE REQUEST
// ==========================================

bool firebaseRequest(
  const char *method,
  const String &path,
  const String &payload,
  String &answer,
  const String &query = ""
) {
  if (WiFi.status() != WL_CONNECTED ||
      authToken.length() == 0) {
    return false;
  }

  String url = String(FIREBASE_DATABASE_URL);

  url += F("/devices/");
  url += DEVICE_ID;
  url += "/";
  url += path;
  url += F(".json?auth=");
  url += authToken;
  url += query;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(client, url)) return false;

  int status = -1;

  if (strcmp(method, "PUT") == 0) {
    http.addHeader(
      F("Content-Type"),
      F("application/json")
    );

    status = http.PUT(payload);
  }
  else {
    status = http.GET();
  }

  if (status == HTTP_CODE_OK) {
    answer = http.getString();
  }

  http.end();

  if (status == HTTP_CODE_UNAUTHORIZED ||
      status == HTTP_CODE_FORBIDDEN) {
    authToken = "";
  }

  return status == HTTP_CODE_OK;
}

// ==========================================
// FIREBASE CURSOR
// ==========================================

bool getCommittedId(uint32_t &id) {
  String response;

  if (!firebaseRequest(
        "GET",
        "lastUploadedId",
        "",
        response
      )) {
    return false;
  }

  response.trim();

  if (response == "null") {
    id = 0;
    return true;
  }

  return parseUnsigned(response.c_str(), id);
}

// ==========================================
// FIREBASE LOGGING
// ==========================================

bool storeRow() {
  String ignored;

  return firebaseRequest(
    "PUT",
    String(F("logs/")) + String(rowId),
    rowJson,
    ignored
  );
}

bool storeCursor() {
  String ignored;

  return firebaseRequest(
    "PUT",
    "lastUploadedId",
    String(rowId),
    ignored
  );
}

bool persistSleepEvent() {
  if (!sleepEventPending || !sleepId.length()) {
    return true;
  }

  StaticJsonDocument<256> doc;

  if (sleepStartedEpochMs) {
    doc["startAt"] = sleepStartedEpochMs;
  }
  else {
    doc["startAt"] = nullptr;
  }

  if (sleepCompleted && sleepEndedEpochMs) {
    doc["wakeAt"] = sleepEndedEpochMs;
  }
  else {
    doc["wakeAt"] = nullptr;
  }

  if (sleepCompleted) {
    doc["durationMs"] = sleepDurationMs;
  }
  else {
    doc["durationMs"] = nullptr;
  }

  doc["state"] =
    sleepCompleted ? "completed" : "sleeping";

  String payload;
  String ignored;

  serializeJson(doc, payload);

  bool ok = firebaseRequest(
    "PUT",
    String(F("sleepLogs/")) + sleepId,
    payload,
    ignored
  );

  if (ok) {
    sleepEventPending = false;
  }

  return ok;
}

// ==========================================
// WEBSITE: HTML + CSS + JAVASCRIPT
// ALL INSIDE THIS ESP FILE
// ==========================================

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">

<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Water Monitoring System</title>

<style>
* {
  box-sizing: border-box;
}

body {
  font-family: Arial, sans-serif;
  background: #f1f5f9;
  margin: 0;
  padding: 20px;
  color: #1e293b;
}

.container {
  max-width: 1100px;
  margin: auto;
}

header {
  background: #2563eb;
  color: white;
  padding: 22px;
  border-radius: 12px;
  margin-bottom: 20px;
}

header h1 {
  margin: 0 0 8px;
  font-size: 25px;
}

header p {
  margin: 0;
}

.card {
  background: white;
  padding: 20px;
  margin-bottom: 18px;
  border-radius: 12px;
  box-shadow: 0 2px 8px #00000012;
}

h2 {
  margin-top: 0;
  font-size: 20px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  gap: 12px;
}

.sensor {
  background: #eff6ff;
  padding: 16px;
  border-radius: 10px;
  text-align: center;
}

.sensor h3 {
  font-size: 14px;
  margin: 0;
}

.value {
  font-size: 25px;
  font-weight: bold;
  margin-top: 12px;
}

button {
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 7px;
  padding: 12px 18px;
  margin: 5px;
  cursor: pointer;
  font-weight: bold;
}

button:hover {
  background: #1d4ed8;
}

button:disabled {
  opacity: .4;
  cursor: not-allowed;
}

button.active {
  background: #059669;
}

.warning {
  background: #fef3c7;
  padding: 15px;
  border-radius: 8px;
  color: #92400e;
}

.hidden {
  display: none;
}

.tablewrap {
  overflow-x: auto;
}

table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

th, td {
  padding: 10px;
  border-bottom: 1px solid #e2e8f0;
  text-align: left;
  white-space: nowrap;
}

th {
  background: #e2e8f0;
}

.status {
  font-weight: bold;
  color: #059669;
}

small {
  color: #64748b;
}

@media(max-width: 600px) {
  body {
    padding: 10px;
  }

  button {
    padding: 10px;
  }
}
</style>
</head>

<body>

<div class="container">

<header>
<h1>Water Monitoring System</h1>
<p>ESP-01 Web Dashboard | WM001</p>
</header>

<div class="card">
<h2>System Status</h2>
<p>Connection: <span id="connection">Connecting...</span></p>
<p>Current Mode: <span id="mode">AUTO</span></p>
<p>Last Mode Change: <span id="changed">--</span></p>
<p>Sensor Update: <span id="freshness">Waiting...</span></p>
<p id="message"></p>
</div>

<div class="card">
<h2>Operating Modes</h2>

<button id="autoBtn" onclick="sendCommand('AUTO')">
AUTO
</button>

<button id="maintBtn" onclick="sendCommand('MAINT')">
MAINTENANCE
</button>

<button id="sleepBtn" onclick="sendCommand('SLEEP')">
SLEEP
</button>

<button id="wakeBtn" onclick="sendCommand('WAKE')">
WAKE
</button>

</div>

<div class="card">
<h2>Live Sensor Dashboard</h2>

<div class="grid">

<div class="sensor">
<h3>Distance</h3>
<div class="value" id="distance">--</div>
<small>mm</small>
</div>

<div class="sensor">
<h3>Weight</h3>
<div class="value" id="weight">--</div>
</div>

<div class="sensor">
<h3>Float Switch</h3>
<div class="value" id="float">--</div>
</div>

<div class="sensor">
<h3>Water Sensor 1</h3>
<div class="value" id="water1">--</div>
</div>

<div class="sensor">
<h3>Water Sensor 2</h3>
<div class="value" id="water2">--</div>
</div>

<div class="sensor">
<h3>Relay</h3>
<div class="value" id="relay">--</div>
</div>

</div>
</div>

<div class="card">
<h2>Recommendations and Alerts</h2>
<div class="warning" id="recommendation">
Waiting for sensor readings...
</div>
</div>

<div class="card hidden" id="maintenance">
<h2>Maintenance Control</h2>

<p>Automatic operation is paused.</p>

<button onclick="sendCommand('READ')">
Check Sensors
</button>

<button onclick="sendCommand('RELAY1')">
Relay ON
</button>

<button onclick="sendCommand('RELAY0')">
Relay OFF
</button>

<button onclick="sendCommand('TARE')">
Tare Weight
</button>

</div>

<div class="card">
<h2>Sensor Data Logs</h2>

<p>Readings are saved every 3 minutes.</p>

<button onclick="loadLogs()">Refresh Logs</button>

<div class="tablewrap">
<table>
<thead>
<tr>
<th>ID</th>
<th>Date / Time</th>
<th>Uptime</th>
<th>Distance</th>
<th>Weight</th>
<th>Float</th>
<th>Water 1</th>
<th>Water 2</th>
<th>Relay</th>
</tr>
</thead>

<tbody id="logTable">
<tr>
<td colspan="9">Loading logs...</td>
</tr>
</tbody>
</table>
</div>
</div>

<div class="card">
<h2>Sleep History</h2>

<button onclick="loadSleeps()">Refresh Sleep Logs</button>

<div class="tablewrap">
<table>
<thead>
<tr>
<th>Sleep Start</th>
<th>Wake Time</th>
<th>Duration</th>
<th>Status</th>
</tr>
</thead>

<tbody id="sleepTable">
<tr>
<td colspan="4">Loading...</td>
</tr>
</tbody>
</table>
</div>
</div>

<div class="card">
<small id="footer">ESP-01 Water Monitoring System</small>
</div>

</div>

<script>

// ==========================================
// JAVASCRIPT
// ==========================================

const $ = id => document.getElementById(id);

let currentMode = 0;
let commandPending = false;

const THRESHOLDS = {
  distanceMin: 30,
  distanceMax: 1000,
  weightMax: 10000
};

function formatTime(ms) {
  if (!ms || Number(ms) < 1700000000000) {
    return "Not available";
  }

  return new Date(Number(ms)).toLocaleString();
}

function bit(value, yes, no) {
  return Number(value) === 1 ? yes : no;
}

async function requestJSON(url, options) {
  const response = await fetch(url, {
    cache: "no-store",
    ...options
  });

  if (!response.ok) {
    throw new Error("HTTP " + response.status);
  }

  return response.json();
}

// ==========================================
// SEND COMMANDS TO ESP
// ==========================================

async function sendCommand(cmd) {
  if (commandPending) return;

  if (cmd === "SLEEP") {
    if (!confirm("Put Arduino into sleep mode?")) {
      return;
    }
  }

  commandPending = true;

  $("message").textContent = "Sending " + cmd + "...";

  try {
    const result = await requestJSON(
      "/api/command?cmd=" + encodeURIComponent(cmd),
      { method: "POST" }
    );

    $("message").textContent =
      result.message || "Command sent";
  }
  catch (error) {
    $("message").textContent =
      "Command failed: " + error.message;
  }
  finally {
    commandPending = false;

    setTimeout(refreshDashboard, 600);
  }
}

// ==========================================
// BUTTON STATES
// ==========================================

function updateButtons() {
  const auto = $("autoBtn");
  const maint = $("maintBtn");
  const sleep = $("sleepBtn");
  const wake = $("wakeBtn");

  auto.classList.remove("active");
  maint.classList.remove("active");
  sleep.classList.remove("active");
  wake.classList.remove("active");

  auto.disabled = commandPending || currentMode === 2;
  maint.disabled = commandPending || currentMode === 2;
  sleep.disabled = commandPending || currentMode === 2;
  wake.disabled = commandPending || currentMode !== 2;

  if (currentMode === 0) auto.classList.add("active");
  if (currentMode === 1) maint.classList.add("active");
  if (currentMode === 2) sleep.classList.add("active");

  $("maintenance").classList.toggle(
    "hidden",
    currentMode !== 1
  );
}

// ==========================================
// RECOMMENDATIONS
// ==========================================

function getRecommendation(s) {
  if (s.mode === 2) {
    return "Arduino is sleeping. Live sensor readings are paused.";
  }

  if (s.mode === 1) {
    return "Maintenance mode active. Automatic operation is paused.";
  }

  if (!s.liveValid || s.stale) {
    return "No recent sensor data. Check Arduino and ESP connection.";
  }

  let alerts = [];

  if (s.distance < 0) {
    alerts.push("Distance sensor error.");
  }
  else if (s.distance < THRESHOLDS.distanceMin) {
    alerts.push("Distance is below the minimum threshold.");
  }
  else if (s.distance > THRESHOLDS.distanceMax) {
    alerts.push("Distance exceeds the maximum threshold.");
  }

  if (s.weight === -999) {
    alerts.push("Weight sensor is not ready.");
  }
  else if (s.weight > THRESHOLDS.weightMax) {
    alerts.push("Weight exceeds the configured limit.");
  }

  if (s.float === 1) {
    alerts.push("Float switch triggered.");
  }

  if (s.water1 === 1) {
    alerts.push("Water detected by sensor 1.");
  }

  if (s.water2 === 1) {
    alerts.push("Water detected by sensor 2.");
  }

  if (alerts.length === 0) {
    return "System operating normally.";
  }

  return alerts.join(" ");
}

// ==========================================
// LIVE DASHBOARD
// ==========================================

async function refreshDashboard() {
  try {
    const s = await requestJSON("/api/status");

    currentMode = s.mode;

    $("connection").textContent =
      s.wifi ? "Connected" : "Disconnected";

    $("mode").textContent =
      ["AUTO", "MAINTENANCE", "SLEEP"][s.mode] || "UNKNOWN";

    $("changed").textContent =
      formatTime(s.lastChangeAt);

    if (s.mode === 2) {
      $("freshness").textContent = "Arduino sleeping";
    }
    else if (s.stale) {
      $("freshness").textContent = "No recent data";
    }
    else {
      $("freshness").textContent =
        "Updated " + s.ageSec + " seconds ago";
    }

    if (s.liveValid) {
      $("distance").textContent = s.distance;

      $("weight").textContent =
        s.weight === -999 ? "ERROR" : s.weight;

      $("float").textContent =
        bit(s.float, "TRIGGERED", "Normal");

      $("water1").textContent =
        bit(s.water1, "WET", "Dry");

      $("water2").textContent =
        bit(s.water2, "WET", "Dry");

      $("relay").textContent =
        bit(s.relay, "ON", "OFF");
    }

    $("recommendation").textContent =
      getRecommendation(s);

    $("footer").textContent =
      "ESP IP: " + s.ip +
      " | Last Firebase ID: " + s.uploadedId +
      " | Next ID: " + s.nextId +
      (s.error ? " | " + s.error : "");

    updateButtons();
  }
  catch (error) {
    $("connection").textContent = "Disconnected";
  }
}

// ==========================================
// SAFE TABLE VALUES
// ==========================================

function safe(value) {
  return String(value == null ? "--" : value)
    .replace(/[&<>"']/g, c => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      '"': "&quot;",
      "'": "&#39;"
    }[c]));
}

// ==========================================
// SENSOR LOGS
// ==========================================

async function loadLogs() {
  try {
    const data = await requestJSON("/api/logs");

    const rows = Object.values(data || {})
      .filter(x => x && typeof x === "object")
      .sort((a, b) => Number(b.id) - Number(a.id));

    if (!rows.length) {
      $("logTable").innerHTML =
        '<tr><td colspan="9">No logs available.</td></tr>';
      return;
    }

    $("logTable").innerHTML = rows.map(x => {
      const values = [
        x.id,
        formatTime(x.uploaded_at),
        x.uptime_s,
        x.distance_mm,
        x.weight,
        x.float,
        x.water1,
        x.water2,
        x.relay
      ];

      return "<tr>" +
        values.map(v => "<td>" + safe(v) + "</td>").join("") +
        "</tr>";
    }).join("");
  }
  catch (error) {
    $("logTable").innerHTML =
      '<tr><td colspan="9">Unable to load logs.</td></tr>';
  }
}

// ==========================================
// SLEEP DURATION
// ==========================================

function duration(ms) {
  if (ms == null) return "--";

  const seconds = Math.floor(Number(ms) / 1000);

  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;

  return h + "h " + m + "m " + s + "s";
}

// ==========================================
// SLEEP LOGS
// ==========================================

async function loadSleeps() {
  try {
    const data = await requestJSON("/api/sleeps");

    const rows = Object.values(data || {})
      .filter(x => x && typeof x === "object")
      .sort((a, b) =>
        Number(b.startAt || 0) - Number(a.startAt || 0)
      );

    if (!rows.length) {
      $("sleepTable").innerHTML =
        '<tr><td colspan="4">No sleep logs.</td></tr>';
      return;
    }

    $("sleepTable").innerHTML = rows.map(x => {
      const values = [
        formatTime(x.startAt),
        formatTime(x.wakeAt),
        duration(x.durationMs),
        x.state
      ];

      return "<tr>" +
        values.map(v => "<td>" + safe(v) + "</td>").join("") +
        "</tr>";
    }).join("");
  }
  catch (error) {
    $("sleepTable").innerHTML =
      '<tr><td colspan="4">Unable to load sleep logs.</td></tr>';
  }
}

// ==========================================
// AUTOMATIC REFRESH
// ==========================================

updateButtons();

refreshDashboard();
loadLogs();
loadSleeps();

setInterval(refreshDashboard, 2000);

setInterval(() => {
  loadLogs();
  loadSleeps();
}, 60000);

</script>
</body>
</html>
)HTML";

// ==========================================
// API: SYSTEM STATUS
// ==========================================

void sendStatus() {
  StaticJsonDocument<768> d;

  d["wifi"] = WiFi.status() == WL_CONNECTED;
  d["ip"] = WiFi.localIP().toString();

  d["mode"] = mode;

  d["liveValid"] = liveValid;

  d["stale"] =
    !liveValid ||
    (mode != 2 &&
     millis() - lastReadingAt > LIVE_STALE_MS);

  d["ageSec"] =
    liveValid ? (millis() - lastReadingAt) / 1000 : 0;

  d["uptime"] = liveUptime;
  d["distance"] = liveDistance;
  d["weight"] = liveWeight;

  d["float"] = liveFloat;
  d["water1"] = liveWater1;
  d["water2"] = liveWater2;
  d["relay"] = liveRelay;

  d["lastChangeAt"] = lastChangeEpochMs;

  d["nextId"] = nextId;
  d["uploadedId"] = nextId ? nextId - 1 : 0;

  d["error"] = lastError;

  String out;
  serializeJson(d, out);

  web.send(200, "application/json", out);
}

// ==========================================
// API: COMMANDS
// ==========================================

void commandRoute() {
  String cmd = web.arg("cmd");

  if (cmd != "AUTO" &&
      cmd != "MAINT" &&
      cmd != "SLEEP" &&
      cmd != "WAKE" &&
      cmd != "READ" &&
      cmd != "TARE" &&
      cmd != "RELAY1" &&
      cmd != "RELAY0") {

    web.send(
      400,
      "application/json",
      "{\"message\":\"Unknown command\"}"
    );

    return;
  }

  if (cmd == "WAKE") {
    if (mode != 2) {
      web.send(
        409,
        "application/json",
        "{\"message\":\"Arduino is not asleep\"}"
      );

      return;
    }

    pulseArduinoWake();

    web.send(
      200,
      "application/json",
      "{\"message\":\"Wake pulse sent\"}"
    );

    return;
  }

  if (mode == 2) {
    web.send(
      409,
      "application/json",
      "{\"message\":\"Wake Arduino first\"}"
    );

    return;
  }

  if ((cmd == "READ" ||
       cmd == "TARE" ||
       cmd == "RELAY1" ||
       cmd == "RELAY0") &&
      mode != 1) {

    web.send(
      409,
      "application/json",
      "{\"message\":\"Switch to MAINTENANCE first\"}"
    );

    return;
  }

  Serial.println(cmd);

  web.send(
    200,
    "application/json",
    "{\"message\":\"Command sent to Arduino\"}"
  );
}

// ==========================================
// API: FIREBASE LOGS
// ==========================================

void firebaseLogsRoute(const String &path) {
  String response;

  bool ok = firebaseRequest(
    "GET",
    path,
    "",
    response,
    F("&orderBy=%22%24key%22&limitToLast=30")
  );

  if (!ok) {
    web.send(
      503,
      "application/json",
      "{\"error\":\"Firebase unavailable\"}"
    );

    return;
  }

  web.send(
    200,
    "application/json",
    response.length() ? response : "null"
  );
}

// ==========================================
// WEBSITE ROUTES
// ==========================================

void setupWeb() {

  web.on("/", HTTP_GET, []() {
    web.send_P(
      200,
      "text/html; charset=utf-8",
      DASHBOARD_HTML
    );
  });

  web.on("/api/status", HTTP_GET, sendStatus);

  web.on("/api/command", HTTP_POST, commandRoute);

  web.on("/api/logs", HTTP_GET, []() {
    firebaseLogsRoute("logs");
  });

  web.on("/api/sleeps", HTTP_GET, []() {
    firebaseLogsRoute("sleepLogs");
  });

  web.onNotFound([]() {
    web.send(404, "text/plain", "Not found");
  });

  web.begin();
}

// ==========================================
// SETUP
// ==========================================

void setup() {

  digitalWrite(ARDUINO_WAKE_PIN, HIGH);
  pinMode(ARDUINO_WAKE_PIN, OUTPUT);

  Serial.setRxBufferSize(512);
  Serial.begin(SERIAL_BAUD);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.google.com"
  );

  setupWeb();

  nextActionAt = millis();
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {

  web.handleClient();
  serviceSerial();

  if (!wifiOnline()) {
    yield();
    return;
  }

  // ======================================
  // FIREBASE AUTHENTICATION
  // ======================================

  if (authToken.length() == 0 ||
      millis() - authTime >= AUTH_VALID_MS) {

    if (!due(nextActionAt)) {
      yield();
      return;
    }

    if (!signIn()) {
      nextActionAt = millis() + NETWORK_RETRY_MS;

      yield();
      return;
    }

    nextActionAt = millis();
  }

  // ======================================
  // RECOVER FIREBASE RECORD ID
  // ======================================

  if (!cursorLoaded) {

    if (!due(nextActionAt)) {
      yield();
      return;
    }

    uint32_t committed = 0;

    if (!getCommittedId(committed) ||
        committed == UINT32_MAX) {

      nextActionAt = millis() + NETWORK_RETRY_MS;

      yield();
      return;
    }

    nextId = committed + 1;

    cursorLoaded = true;
    syncState = WANT_ROW;

    Serial.print(F("BASE,"));
    Serial.println(committed);

    nextActionAt = millis() + 500;
  }

  // ======================================
  // UPLOAD SLEEP HISTORY
  // ======================================

  if (sleepEventPending &&
      due(nextSleepUploadAt)) {

    if (!persistSleepEvent()) {
      nextSleepUploadAt =
        millis() + NETWORK_RETRY_MS;
    }
  }

  // ======================================
  // SD TO FIREBASE SYNCHRONIZATION
  // ======================================

  if (mode != 2) {

    switch (syncState) {

      case WANT_ROW:

        if (due(nextActionAt)) {
          sendRowRequest();
        }

        break;

      case WAIT_ROW:

        if (millis() - lastRowRequest >= SERIAL_RETRY_MS) {

          serialRetries++;

          sendRowRequest();

          if (serialRetries >= 3) {
            serialRetries = 0;

            lastError =
              "No SD response; retrying same ID";
          }
        }

        break;

      case UPLOAD_ROW:

        if (due(nextActionAt)) {

          if (storeRow()) {
            syncState = COMMIT_ID;
            nextActionAt = millis();
          }
          else {
            nextActionAt =
              millis() + NETWORK_RETRY_MS;
          }
        }

        break;

      case COMMIT_ID:

        if (due(nextActionAt)) {

          if (storeCursor()) {

            rowJson = "";
            lastError = "";

            if (rowId != UINT32_MAX) {

              nextId = rowId + 1;

              syncState = WANT_ROW;
              nextActionAt = millis();
            }
            else {
              nextActionAt =
                millis() + 3600000UL;
            }
          }
          else {
            nextActionAt =
              millis() + NETWORK_RETRY_MS;
          }
        }

        break;
    }
  }

  web.handleClient();
  serviceSerial();

  yield();
}
