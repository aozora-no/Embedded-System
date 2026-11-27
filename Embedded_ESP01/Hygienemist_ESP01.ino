#include "ArduinoLink.h"
#include "FirebaseClient.h"
#include <time.h>
#include <ctype.h>

// MAIN SYSTEM STATE AND FUNCTION DECLARATIONS
ArduinoLink communication;
FirebaseClient firebase;

static constexpr uint32_t ipInterval = 10000UL;
static constexpr uint32_t serialRetry = 4500UL;
static constexpr uint32_t networkRetry = 6000UL;
static constexpr uint32_t logPoll = 2500UL;
static constexpr uint32_t sdRetry = 7000UL;
static constexpr uint32_t staleTime = 12000UL;
static constexpr uint32_t modeAckTimeout = 15000UL;

String rowJson;

uint32_t lastIP = 0;
uint32_t nextAction = 0;
uint32_t lastRequest = 0;
uint32_t nextId = 1;
uint32_t committedId = 0;
uint32_t rowId = 0;
uint32_t lastReading = 0;

bool idLoaded = false;
bool ipSent = false;
bool liveValid = false;

uint8_t serialRetries = 0;

// 0 = AUTO
// 1 = MAINTENANCE
// 2 = SLEEP

uint8_t mode = 0;

// Requested mode is separate from the confirmed Arduino mode.
int8_t requestedMode = -1;
bool modeSent = false;
uint32_t modeRequestAt = 0;
uint32_t lastModeSend = 0;
String modeMessage;
bool historyWritePending = false;
uint32_t historyWriteAt = 0;
uint32_t uptime = 0;

long distance = -1;
float weight = -999;

int objectDetected = 0;
int sensor1 = 0;
int sensor2 = 0;
int pump = 0;

String level = "UNKNOWN";
String weightStatus = "ERROR";
String lastError = "";
String gapWarning = "";
uint32_t baseSendAt = 0;
bool basePending = false;

uint64_t modeChangedAt = 0;

// SLEEP AND MAINTENANCE SESSION HISTORY
// All timestamps are captured when Arduino reports a mode change.
struct ModeSession {
  String id;
  uint8_t type = 0; // 1 = maintenance, 2 = sleep
  uint64_t startAt = 0;
  uint64_t endAt = 0;
  uint32_t startMillis = 0;
  uint32_t durationMs = 0;
  bool finished = false;
  bool interrupted = false;
  bool sdSaved = false;
  bool cloudSaved = false;
};

ModeSession activeSession;
bool activeSessionDirty = false;
static constexpr uint8_t historyCapacity = 8;
ModeSession pendingHistory[historyCapacity];
uint8_t historyHead = 0;
uint8_t historyCount = 0;
uint32_t nextHistoryUpload = 0;
uint32_t sessionSequence = 0;
uint32_t droppedHistory = 0;
uint32_t nextHistorySD = 0;

enum SyncState {
  WANT_ROW,
  WAIT_ROW,
  UPLOAD_ROW,
  COMMIT_ID
};

SyncState syncState = WANT_ROW;
String activeCommandId;
String activeCommand;
String activeCommandUid;
uint64_t activeCreatedAt = 0;
String resultStatus;
String resultMessage;
bool awaitingMode = false;
uint8_t targetMode = 0;
uint32_t nextCommandPoll = 0;
uint32_t nextStatusUpload = 0;

bool modePending();
bool serialPaused();
void confirmMode(uint8_t confirmed);
bool timeReady(uint32_t target);
uint64_t currentTime();
String numberToString(uint64_t n);
void addDateTime(JsonDocument &doc);
void sendIP();
bool checkWiFi();
bool readUnsigned(const char *s, uint32_t &out);
bool isInteger(const char *s);
bool isDecimal(const char *s);
bool isBinary(const char *s);
bool validLevel(const char *s);
bool validWeightStatus(const char *s);
bool readLogData(
  char *line,
  uint32_t expectedId,
  String &json
);
void finishModeSession(bool interrupted);
void startModeSession(uint8_t type);
void setMode(uint8_t newMode, bool interrupted = false);
void readLiveData(char *line);
String sdHistoryKey(const String &id);
void processArduinoMessage(char *line);
void requestLog();
bool getLastId(uint32_t &id);
bool uploadLog();
bool saveLastId();
void addSessionDateTime(JsonDocument &doc, uint64_t timestamp,
                        const char *dateKey, const char *timeKey);
bool saveModeSession(const ModeSession &session);
bool uploadStatus();
bool connectFirebase();
bool loadFirebaseId();
void releaseSavedHistory();
void saveSDHistory();
void uploadModeHistory();
void syncLogs();
void handleModeSwitch();
bool writeCommandState(const char *state, const String &message);
void clearCommand();
void finishCommand(const char *state, const String &message);
void executeCommand();
void pollCommand();
void readArduino();

// MAIN SYSTEM FUNCTIONS

bool modePending() { return requestedMode >= 0; }

bool serialPaused() {
  return modePending() || mode == 2;
}

void confirmMode(uint8_t confirmed) {
  if (!modePending() || !modeSent || requestedMode != confirmed) return;
  requestedMode = -1;
  modeSent = false;
  modeMessage = confirmed == 0 ? "AUTO confirmed" :
                confirmed == 1 ? "MAINTENANCE confirmed" : "SLEEP confirmed";
}

bool timeReady(uint32_t target) {
  return (int32_t)(millis() - target) >= 0;
}

uint64_t currentTime() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    return (uint64_t)now * 1000ULL;
  }
  return 0ULL;
}

String numberToString(uint64_t n) {
  char buf[24];
  snprintf(
    buf,
    sizeof(buf),
    "%llu",
    (unsigned long long)n
  );
  return String(buf);
}

void addDateTime(JsonDocument &doc) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    doc["date"] = "Unavailable";
    doc["time"] = "Unavailable";
    return;
  }
  // Philippine Time UTC+8
  now += 8 * 3600;
  struct tm timeInfo;
  gmtime_r(&now, &timeInfo);
  char date[16];
  char clockTime[16];
  strftime(
    date,
    sizeof(date),
    "%Y-%m-%d",
    &timeInfo
  );
  strftime(
    clockTime,
    sizeof(clockTime),
    "%H:%M:%S",
    &timeInfo
  );
  doc["date"] = String(date);
  doc["time"] = String(clockTime);
}

void sendIP() {
  if (serialPaused() || WiFi.status() != WL_CONNECTED) return;
  communication.output().print(F("IP,"));
  communication.output().println(WiFi.localIP());
  ipSent = true;
  lastIP = millis();
}

bool checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    ipSent = false;
    return false;
  }
  if (!ipSent || millis() - lastIP >= ipInterval) {
    sendIP();
  }
  return true;
}

bool readUnsigned(const char *s, uint32_t &out) {
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

bool isInteger(const char *s) {
  if (!s || !*s) return false;
  if (*s == '-') s++;
  if (!*s) return false;
  while (*s) {
    if (*s < '0' || *s > '9') return false;
    s++;
  }
  return true;
}

bool isDecimal(const char *s) {
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

bool isBinary(const char *s) {
  return strcmp(s, "0") == 0 ||
         strcmp(s, "1") == 0;
}

bool validLevel(const char *s) {
  return strcmp(s, "HIGH") == 0 ||
         strcmp(s, "HALF") == 0 ||
         strcmp(s, "LOW") == 0;
}

bool validWeightStatus(const char *s) {
  return strcmp(s, "NEGATIVE") == 0 ||
         strcmp(s, "LOW") == 0 ||
         strcmp(s, "HALF") == 0 ||
         strcmp(s, "FULL") == 0 ||
         strcmp(s, "ERROR") == 0;
}

bool readLogData(
  char *line,
  uint32_t expectedId,
  String &json
) {
  if (strncmp(line, "L,", 2) != 0) return false;
  char *csv = line + 2;
  char *star = strrchr(csv, '*');
  if (!star || strlen(star + 1) != 4) return false;
  for (char *p = star + 1; *p; ++p) {
    if (!isxdigit((unsigned char)*p)) return false;
  }
  unsigned long received = strtoul(
    star + 1,
    nullptr,
    16
  );
  *star = '\0';
  if (ArduinoLink::crc16(csv) != (uint16_t)received) return false;
  char *fields[11];
  size_t count = 0;
  char *p = csv;
  // Accept both old 10-column records and new 11-column records.
  while (true) {
    if (count >= 11) return false;
    fields[count++] = p;
    char *comma = strchr(p, ',');
    if (!comma) break;
    *comma = '\0';
    p = comma + 1;
  }
  if (count != 10 && count != 11) return false;
  const char *event = count == 11 ? fields[10] : "LEGACY";
  if (count == 11 &&
      strcmp(event, "PERIODIC") != 0 &&
      strcmp(event, "AUTO_DISPENSE") != 0 &&
      strcmp(event, "MAINTENANCE_DISPENSE") != 0) {
    return false;
  }
  uint32_t id = 0;
  uint32_t uptime = 0;
  if (
    !readUnsigned(fields[0], id) ||
    id != expectedId ||
    !readUnsigned(fields[1], uptime) ||
    !isInteger(fields[2]) ||
    !(strcmp(fields[3], "ERR") == 0 ||
    isDecimal(fields[3]))
  ) {
    return false;
  }
  for (int i = 4; i <= 7; ++i) {
    if (!isBinary(fields[i])) return false;
  }
  if (
    !validLevel(fields[8]) ||
    !validWeightStatus(fields[9])
  ) {
    return false;
  }
  StaticJsonDocument<768> doc;
  doc["id"] = id;
  doc["uptime_s"] = uptime;
  doc["distance_mm"] = atol(fields[2]);
  if (strcmp(fields[3], "ERR") == 0) {
    doc["weight"] = nullptr;
  }
  else {
    doc["weight"] = atof(fields[3]);
  }
  doc["ultrasonic_zero"] = atoi(fields[4]);
  doc["noncontact1"] = atoi(fields[5]);
  doc["noncontact2"] = atoi(fields[6]);
  doc["pump"] = atoi(fields[7]);
  doc["level"] = fields[8];
  doc["weight_status"] = fields[9];
  doc["event"] = event;
  // Firebase server timestamp
  JsonObject uploaded = doc.createNestedObject("uploaded_at");
  uploaded[".sv"] = "timestamp";
  json = "";
  serializeJson(doc, json);
  return true;
}

void finishModeSession(bool interrupted) {
  if (!activeSession.id.length()) return;
  activeSession.endAt = currentTime();
  activeSession.durationMs = millis() - activeSession.startMillis;
  activeSession.finished = true;
  activeSession.interrupted = interrupted;
  // Keep completed sessions in RAM until Firebase accepts them.
  if (historyCount < historyCapacity) {
    uint8_t tail = (historyHead + historyCount) % historyCapacity;
    pendingHistory[tail] = activeSession;
    historyCount++;
  } else {
    droppedHistory++;
    lastError = "Mode history queue full; session not saved";
  }
  activeSession = ModeSession();
  activeSessionDirty = false;
  nextHistoryUpload = millis();
}

void startModeSession(uint8_t type) {
  activeSession = ModeSession();
  activeSession.type = type;
  activeSession.startAt = currentTime();
  activeSession.startMillis = millis();
  activeSession.id = numberToString(activeSession.startAt
    ? activeSession.startAt : (uint64_t)activeSession.startMillis);
  activeSession.id += "-" + String(ESP.getChipId(), HEX);
  activeSession.id += "-" + String(ESP.getCycleCount(), HEX);
  activeSession.id += "-" + String(++sessionSequence);
  activeSessionDirty = true;
  nextHistoryUpload = millis();
}

void setMode(uint8_t newMode, bool interrupted) {
  if (mode == newMode) return;
  if (mode == 1 || mode == 2) finishModeSession(interrupted);
  mode = newMode;
  modeChangedAt = currentTime();
  if (mode == 1 || mode == 2) startModeSession(mode);
  if (mode == 2) syncState = WANT_ROW;
}

void readLiveData(char *line) {
  char *fields[10];
  char *saveptr = nullptr;
  char *p = strtok_r(line + 2, ",", &saveptr);
  for (int i = 0; i < 10; ++i) {
    if (!p) return;
    fields[i] = p;
    p = strtok_r(nullptr, ",", &saveptr);
  }
  uint32_t newUptime = 0;
  if (
    p ||
    !readUnsigned(fields[0], newUptime) ||
    !isInteger(fields[1]) ||
    !isDecimal(fields[2])
  ) {
    return;
  }
  for (int i = 3; i <= 6; ++i) {
    if (!isBinary(fields[i])) return;
  }
  if (
    strcmp(fields[7], "0") != 0 &&
    strcmp(fields[7], "1") != 0
  ) {
    return;
  }
  if (
    !validLevel(fields[8]) ||
    !validWeightStatus(fields[9])
  ) {
    return;
  }
  uptime = newUptime;
  distance = atol(fields[1]);
  weight = atof(fields[2]);
  objectDetected = atoi(fields[3]);
  sensor1 = atoi(fields[4]);
  sensor2 = atoi(fields[5]);
  pump = atoi(fields[6]);
  level = fields[8];
  weightStatus = fields[9];
  lastReading = millis();
  liveValid = true;
  // A valid live packet can also recover from a missed wake ACK.
  setMode((uint8_t)atoi(fields[7]));
  confirmMode(mode);
}

String sdHistoryKey(const String &id) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned int i=0; i<id.length(); i++) {
    hash ^= (uint8_t)id[i];
    hash *= 1099511628211ULL;
  }
  char key[17];
  snprintf(key, sizeof(key), "%08lx%08lx", (unsigned long)(hash >> 32), (unsigned long)(hash & 0xffffffffULL));
  return String(key);
}

void processArduinoMessage(char *line) {
  if (strncmp(line, "HACK,", 5) == 0) {
    historyWritePending = false;
    for (byte i=0; i<historyCount; i++) {
      ModeSession &s = pendingHistory[(historyHead+i)%historyCapacity];
      if (sdHistoryKey(s.id) == line+5) s.sdSaved = true;
    }
    return;
  }
  if (strcmp(line, "BOOT") == 0) {
    if (modePending()) {
      requestedMode = -1;
      modeSent = false;
      modeMessage = "Arduino restarted during mode switch";
    }
    historyWritePending = false;
    setMode(0, true);
    sendIP();
    if (idLoaded) {
      basePending = true;
      baseSendAt = millis();
      if (syncState == WAIT_ROW) syncState = WANT_ROW;
    }
    return;
  }
  if (strncmp(line, "D,", 2) == 0) {
    readLiveData(line);
    return;
  }
  if (strncmp(line, "ACK,", 4) == 0) {
    const char *ack = line + 4;
    if (strcmp(ack, "BASE") == 0) {
      basePending = false;
      if (lastError == "Waiting for Arduino BASE acknowledgement") lastError = "";
      if (syncState == WANT_ROW) nextAction = millis();
    }
    else if (strcmp(ack, "AUTO") == 0) {
      setMode(0);
      confirmMode(0);
    }
    else if (strcmp(ack, "MAINT") == 0) {
      setMode(1);
      pump = 0;
      confirmMode(1);
    }
    else if (strcmp(ack, "SLEEP") == 0) {
      setMode(2);
      pump = 0;
      confirmMode(2);
    }
    else if (strcmp(ack, "WAKE") == 0) {
      ipSent = false;
      setMode(0);
      confirmMode(0);
    }
    else if (strcmp(ack, "PUMP1") == 0) {
      pump = 1;
    }
    else if (strcmp(ack, "PUMP0") == 0) {
      pump = 0;
    }
    return;
  }
  if (syncState != WAIT_ROW || mode == 2) {
    return;
  }
  if (strncmp(line, "L,", 2) == 0) {
    String parsed;
    if (readLogData(line, nextId, parsed)) {
      rowJson = parsed;
      rowId = nextId;
      syncState = UPLOAD_ROW;
      serialRetries = 0;
      nextAction = millis();
    } else {
      lastError = "Invalid SD record (CRC, fields or ID): " + String(nextId);
      syncState = WANT_ROW;
      nextAction = millis() + 800UL;
    }
    return;
  }
  uint32_t reported = 0;
  if (strncmp(line, "NEXT,", 5) == 0) {
    char *comma = strchr(line + 5, ',');
    uint32_t available = 0;
    if (comma) {
      *comma = '\0';
      if (readUnsigned(line + 5, reported) && reported == nextId &&
          readUnsigned(comma + 1, available) && available > nextId) {
        gapWarning = "Missing SD ID range " + String(nextId) + "-" + String(available - 1);
        nextId = available;
        syncState = WANT_ROW;
        serialRetries = 0;
        nextAction = millis() + 500UL;
      }
    }
    return;
  }
  if (
    strncmp(line, "END,", 4) == 0 &&
    readUnsigned(line + 4, reported) &&
    reported == nextId
  ) {
    syncState = WANT_ROW;
    serialRetries = 0;
    nextAction = millis() + logPoll;
    lastError = "";
  }
  else if (strncmp(line, "BUSY,", 5) == 0 &&
           readUnsigned(line + 5, reported) && reported == nextId) {
    syncState = WANT_ROW;
    nextAction = millis() + 800UL;
  }
  else if (strcmp(line, "ERR,SD") == 0) {
    syncState = WANT_ROW;
    serialRetries = 0;
    nextAction = millis() + sdRetry;
    lastError = "Arduino SD card unavailable";
  }
  else if (
    strncmp(line, "GAP,", 4) == 0 &&
    readUnsigned(line + 4, reported) &&
    reported == nextId
  ) {
    syncState = WANT_ROW;
    serialRetries = 0;
    nextAction = millis() + sdRetry;
    lastError =
      "Missing SD record ID " + String(nextId);
  }
}

void requestLog() {
  if (serialPaused() || basePending) return;
  communication.output().print(F("GET,"));
  communication.output().println(nextId);
  lastRequest = millis();
  syncState = WAIT_ROW;
}

bool getLastId(uint32_t &id) {
  String response;
  if (
    !firebase.request(
      "GET",
      "lastUploadedId",
      "",
      response
    )
  ) {
    return false;
  }
  response.trim();
  if (response == "null") {
    id = 0;
    return true;
  }
  return readUnsigned(response.c_str(), id);
}

bool uploadLog() {
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, rowJson)) {
    return false;
  }
  // Add date and time before uploading
  addDateTime(doc);
  String payload;
  String ignored;
  serializeJson(doc, payload);
  return firebase.request(
    "PUT",
    String(F("logs/")) + String(rowId),
    payload,
    ignored
  );
}

bool saveLastId() {
  String ignored;
  return firebase.request(
    "PUT",
    "lastUploadedId",
    String(rowId),
    ignored
  );
}

void addSessionDateTime(JsonDocument &doc, uint64_t timestamp,
                        const char *dateKey, const char *timeKey) {
  if (!timestamp) {
    doc[dateKey] = nullptr;
    doc[timeKey] = nullptr;
    return;
  }
  time_t local = (time_t)(timestamp / 1000ULL) + 8 * 3600;
  struct tm info;
  if (!gmtime_r(&local, &info)) {
    doc[dateKey] = nullptr;
    doc[timeKey] = nullptr;
    return;
  }
  char date[16], clockTime[16];
  strftime(date, sizeof(date), "%Y-%m-%d", &info);
  strftime(clockTime, sizeof(clockTime), "%H:%M:%S", &info);
  doc[dateKey] = String(date);
  doc[timeKey] = String(clockTime);
}

bool saveModeSession(const ModeSession &session) {
  StaticJsonDocument<512> doc;
  bool sleep = session.type == 2;
  const char *endKey = sleep ? "wakeAt" : "endAt";
  const char *endDateKey = sleep ? "wakeDate" : "endDate";
  const char *endTimeKey = sleep ? "wakeTime" : "endTime";
  if (session.startAt) doc["startAt"] = session.startAt;
  else doc["startAt"] = nullptr;
  addSessionDateTime(doc, session.startAt, "startDate", "startTime");
  if (session.finished && session.endAt) doc[endKey] = session.endAt;
  else doc[endKey] = nullptr;
  addSessionDateTime(doc, session.finished ? session.endAt : 0,
                     endDateKey, endTimeKey);
  if (session.finished) doc["durationMs"] = session.durationMs;
  else doc["durationMs"] = nullptr;
  doc["state"] = session.finished
    ? (session.interrupted ? "interrupted" : "completed")
    : (sleep ? "sleeping" : "in_progress");
  if (doc.overflowed()) return false;
  String payload, ignored;
  serializeJson(doc, payload);
  return firebase.request("PUT",
    String(sleep ? "sleepLogs/" : "maintenanceLogs/") + session.id,
    payload, ignored);
}

bool uploadStatus() {
  StaticJsonDocument<1536> d;
  d["wifi"] =
    WiFi.status() == WL_CONNECTED;
  d["ip"] =
    WiFi.localIP().toString();
  d["mode"] = mode;
  d["modePending"] = modePending();
  d["modeMessage"] = modeMessage;
  d["liveValid"] = liveValid;
  d["stale"] =
    !liveValid ||
    (mode == 0 &&
    millis() - lastReading > staleTime);
  d["ageSec"] =
    liveValid
    ? (millis() - lastReading) / 1000
    : 0;
  d["uptime"] = uptime;
  d["distance"] = distance;
  d["weight"] = weight;
  d["objectDetected"] = objectDetected;
  d["noncontact1"] = sensor1;
  d["noncontact2"] = sensor2;
  d["pump"] = pump;
  d["level"] = level;
  d["weightStatus"] = weightStatus;
  d["lastChangeAt"] = modeChangedAt;
  d["nextId"] = nextId;
  d["uploadedId"] = committedId;
  d["error"] = lastError.length() ? lastError : firebase.error();
  d["firebaseError"] = firebase.error();
  d["sdGapWarning"] = gapWarning;
  d["lastHttpCode"] = firebase.httpCode();
  d["droppedHistory"] = droppedHistory;
  d["updatedAt"][".sv"] = "timestamp";
  String out, reply;
  serializeJson(d, out);
  return firebase.request("PUT", "status", out, reply);
}

void setup() {
  communication.begin();
  firebase.begin();
  nextAction = millis();
}

bool connectFirebase() {
  if (
    firebase.ready()
  ) {
    return true;
  }
  if (!timeReady(nextAction)) {
    return false;
  }
  if (!firebase.login()) {
    nextAction = millis() + networkRetry;
    lastError = "Firebase sign-in failed";
    return false;
  }
  if (lastError == "Firebase sign-in failed") {
    lastError = "";
  }
  nextAction = millis();
  return true;
}

bool loadFirebaseId() {
  // Do not send BASE while Arduino is entering sleep or asleep.
  if (serialPaused()) return false;
  if (idLoaded) return true;
  if (!timeReady(nextAction)) return false;
  uint32_t committed = 0;
  if (
    !getLastId(committed) ||
    committed == UINT32_MAX
  ) {
    nextAction = millis() + networkRetry;
    lastError =
      "Cannot read Firebase lastUploadedId";
    return false;
  }
  committedId = committed;
  nextId = committed + 1;
  idLoaded = true;
  syncState = WANT_ROW;
  basePending = true;
  baseSendAt = millis();
  lastError = "";
  nextAction = millis();
  return true;
}

void releaseSavedHistory() {
  while (historyCount && pendingHistory[historyHead].sdSaved && pendingHistory[historyHead].cloudSaved) {
    pendingHistory[historyHead] = ModeSession();
    historyHead = (historyHead + 1) % historyCapacity;
    historyCount--;
  }
}

void saveSDHistory() {
  if (serialPaused() || pump || syncState == WAIT_ROW || historyWritePending || !timeReady(nextHistorySD)) return;
  for (byte i=0; i<historyCount; i++) {
    ModeSession &s = pendingHistory[(historyHead+i)%historyCapacity];
    if (s.sdSaved) continue;
    communication.output().print(F("HWRITE,")); communication.output().print(s.type); communication.output().print(',');
    communication.output().print(sdHistoryKey(s.id)); communication.output().print(',');
    communication.output().print((uint32_t)(s.startAt / 1000ULL)); communication.output().print(',');
    communication.output().print((uint32_t)(s.endAt / 1000ULL)); communication.output().print(',');
    communication.output().print(s.durationMs); communication.output().print(','); communication.output().println(s.interrupted ? 1 : 0);
    historyWritePending = true;
    historyWriteAt = millis();
    nextHistorySD = millis() + 3500UL;
    return;
  }
}

void uploadModeHistory() {
  if (!timeReady(nextHistoryUpload)) return;
  bool ok;
  for (byte i=0; i<historyCount; i++) {
    ModeSession &s = pendingHistory[(historyHead+i)%historyCapacity];
    if (s.cloudSaved) continue;
    ok = saveModeSession(s);
    if (ok) s.cloudSaved = true;
    nextHistoryUpload = millis() + (ok ? 0 : networkRetry);
    return;
  }
  if (!activeSessionDirty) return;
  ok = saveModeSession(activeSession);
  if (ok) activeSessionDirty = false;
  nextHistoryUpload = millis() + (ok ? 0 : networkRetry);
}

void syncLogs() {
  if (serialPaused()) return;
  if (basePending) {
    if (timeReady(baseSendAt)) {
      communication.output().print(F("BASE,"));
      communication.output().println(committedId);
      baseSendAt = millis() + 1500UL;
      lastError = "Waiting for Arduino BASE acknowledgement";
    }
    return;
  }
  switch (syncState) {
    case WANT_ROW:
      if (timeReady(nextAction)) {
        requestLog();
      }
      break;
    case WAIT_ROW:
      if (millis() - lastRequest >= serialRetry) {
        requestLog();
        serialRetries++;
        if (serialRetries >= 3) {
          serialRetries = 0;
          lastError =
            "No SD response; retrying same ID";
        }
      }
      break;
    case UPLOAD_ROW:
      if (!timeReady(nextAction)) break;
      if (uploadLog()) {
        syncState = COMMIT_ID;
        nextAction = millis();
      }
      else {
        lastError =
          "Firebase log upload failed";
        nextAction =
          millis() + networkRetry;
      }
      break;
    case COMMIT_ID:
      if (!timeReady(nextAction)) break;
      if (!saveLastId()) {
        lastError =
          "Firebase ID save failed";
        nextAction =
          millis() + networkRetry;
        break;
      }
      rowJson = "";
      lastError = "";
      if (rowId == UINT32_MAX) {
        nextAction =
          millis() + 3600000UL;
        break;
      }
      committedId = rowId;
      nextId = rowId + 1;
      syncState = WANT_ROW;
      nextAction = millis() + 800UL;
      break;
  }
}

void handleModeSwitch() {
  if (!modePending()) return;
  if (!modeSent) {
    // Let the in-flight serial record finish before sending another command.
    if (historyWritePending) return;
    if (syncState == WAIT_ROW) {
      if (millis() - lastRequest < serialRetry) return;
      syncState = WANT_ROW;
      nextAction = millis();
    }
    modeSent = true;
    modeRequestAt = millis();
    lastModeSend = millis();
    if (mode == 2) communication.wake();
    else communication.output().println(requestedMode == 0 ? "AUTO" : requestedMode == 1 ? "MAINT" : "SLEEP");
    return;
  }
  if (millis() - modeRequestAt >= modeAckTimeout) {
    requestedMode = -1;
    modeSent = false;
    modeMessage = "Mode not confirmed; check Arduino connection";
    return;
  }
  // AUTO and MAINT are idempotent; never retry pump, tare, or sleep.
  if (mode != 2 && (requestedMode == 0 || requestedMode == 1) &&
      millis() - lastModeSend >= 1800UL) {
    lastModeSend = millis();
    communication.output().println(requestedMode == 0 ? "AUTO" : "MAINT");
  }
}

bool writeCommandState(const char *state, const String &message) {
  StaticJsonDocument<768> d;
  d["id"] = activeCommandId;
  d["cmd"] = activeCommand;
  d["uid"] = activeCommandUid;
  d["createdAt"] = activeCreatedAt;
  d["status"] = state;
  d["message"] = message;
  d["updatedAt"][".sv"] = "timestamp";
  String body, reply;
  serializeJson(d, body);
  return firebase.request("PUT", "command", body, reply);
}

void clearCommand() {
  activeCommandId = "";
  activeCommand = "";
  activeCommandUid = "";
  resultStatus = "";
  resultMessage = "";
  awaitingMode = false;
}

void finishCommand(const char *state, const String &message) {
  resultStatus = state;
  resultMessage = message;
  awaitingMode = false;
  nextStatusUpload = millis();
}

void executeCommand() {
  String cmd = activeCommand;
  bool isMode = cmd == "AUTO" || cmd == "MAINT" || cmd == "SLEEP" || cmd == "WAKE";
  if (mode == 2 && cmd != "WAKE") { finishCommand("rejected", "Wake Arduino first"); return; }
  if (cmd == "WAKE" && mode != 2) { finishCommand("rejected", "Arduino is not asleep"); return; }
  if (isMode) {
    targetMode = cmd == "MAINT" ? 1 : cmd == "SLEEP" ? 2 : 0;
    if (mode == targetMode) { finishCommand("confirmed", "Already in this mode"); return; }
    requestedMode = targetMode;
    modeSent = false;
    modeRequestAt = millis();
    modeMessage = String("Switching to ") + cmd;
    awaitingMode = true;
    return;
  }
  if (mode != 1) { finishCommand("rejected", "Switch to MAINTENANCE first"); return; }
  if (cmd != "READ" && cmd != "PUMP1" && cmd != "TARE") {
    finishCommand("rejected", "Unknown command"); return;
  }
  // Original Arduino protocol: these commands are sent, not assumed confirmed.
  communication.output().println(cmd);
  finishCommand("sent", "Sent to Arduino; check live readings for result");
}

void pollCommand() {
  if (activeCommandId.length()) {
    if (resultStatus.length() && writeCommandState(resultStatus.c_str(), resultMessage)) clearCommand();
    return;
  }
  String response;
  if (!firebase.request("GET", "command", "", response)) return;
  StaticJsonDocument<1024> d;
  if (deserializeJson(d, response) || d.isNull()) return;
  String state = d["status"] | "";
  if (state != "pending" && state != "processing") return;
  // Do not send serial commands while an SD transaction is still in flight.
  String incoming = d["cmd"] | "";
  bool incomingMode = incoming == "AUTO" || incoming == "MAINT" ||
                      incoming == "SLEEP" || incoming == "WAKE";
  if (state == "pending" && (historyWritePending || syncState == WAIT_ROW ||
      (pump && !incomingMode))) return;
  activeCommandId = d["id"] | "";
  activeCommand = d["cmd"] | "";
  activeCommandUid = d["uid"] | "";
  activeCreatedAt = d["createdAt"].as<uint64_t>();
  if (!activeCommandId.length()) { clearCommand(); return; }
  if (state == "processing") {
    finishCommand("failed", "Previous attempt interrupted; not repeated"); return;
  }
  uint64_t now = currentTime();
  // NTP time is required to prevent stale commands executing after reconnection.
  if (!now || !activeCreatedAt || activeCreatedAt > now + 5000ULL || now > activeCreatedAt + 30000ULL) {
    finishCommand("expired", "Command expired or device clock not synchronized"); return;
  }
  if (!writeCommandState("processing", "Accepted by ESP-01")) { clearCommand(); return; }
  // Recheck after the blocking HTTPS call: never execute an expired command.
  now = currentTime();
  if (!now || now > activeCreatedAt + 30000ULL) {
    finishCommand("expired", "Command expired before transmission"); return;
  }
  readArduino();
  executeCommand();
}

void loop() {
  readArduino();
  if (historyWritePending && millis() - historyWriteAt >= 3500UL) historyWritePending = false;
  handleModeSwitch();
  if (modePending()) { yield(); return; }
  if (awaitingMode) {
    if (mode == targetMode && modeMessage.endsWith("confirmed")) finishCommand("confirmed", modeMessage);
    else finishCommand("failed", modeMessage.length() ? modeMessage : String("Mode not confirmed"));
  }
  releaseSavedHistory();
  if (WiFi.status() != WL_CONNECTED) { ipSent = false; yield(); return; }
  if (!historyWritePending && syncState != WAIT_ROW && (!ipSent || millis() - lastIP >= ipInterval)) sendIP();
  if (!firebase.ready()) {
    connectFirebase(); yield(); return;
  }
  static uint8_t turn = 0;
  // While waiting for an SD reply, let serial processing finish or time out.
  // If no reply arrives, release the slot so mode commands can still be read.
  if (syncState == WAIT_ROW && millis() - lastRequest >= serialRetry) {
    syncState = WANT_ROW;
    nextAction = millis() + 500;
    lastError = "No SD response; retrying same ID";
  }
  if (historyWritePending || syncState == WAIT_ROW) { yield(); return; }
  if (timeReady(nextCommandPoll)) {
    pollCommand();
    nextCommandPoll = millis() + 1500UL;
    readArduino();
    yield();
    return;
  }
  if (turn == 1) {
    if (timeReady(nextStatusUpload)) {
      uploadStatus();
      nextStatusUpload = millis() + 3000UL;
    }
  } else {
    saveSDHistory();
    if (!historyWritePending) {
      if (!serialPaused() && !idLoaded) loadFirebaseId();
      else {
        bool historyDue = activeSessionDirty;
        for (byte i=0; i<historyCount; i++) {
          if (!pendingHistory[(historyHead+i)%historyCapacity].cloudSaved) historyDue = true;
        }
        if (historyDue && timeReady(nextHistoryUpload)) uploadModeHistory();
        else if (!serialPaused() && idLoaded) syncLogs();
      }
    }
  }
  turn = (turn + 1) % 3;
  readArduino();
  yield();
}

void readArduino() {
  char *line;
  while ((line = communication.readLine()) != nullptr) {
    processArduinoMessage(line);
  }
}
