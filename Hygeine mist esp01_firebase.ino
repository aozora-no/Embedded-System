// ESP-01 side of the water monitoring system
//
// IMPORTANT: Serial (GPIO1/GPIO3) is wired to the Arduino Pro Mini, so this
// sketch must NOT print debug text on Serial. Only protocol lines are sent.
//
// ESP-01 -> Arduino : WIFI,1 / WIFI,0            (every 1 s)
//                     TIME,yyyy,mm,dd,hh,mm,ss   (every 60 s once NTP is synced)
//                     IP,<address>               (every 30 s while connected)
//                     ACK,<record id>            (after the record is in Firebase)
// Arduino -> ESP-01 : DATA,<id>,<date>,<time>,<tof>,<hx711>,<float>,<w1>,<w2>,<relay>
//
// Each record is stored at /records/<id> with PUT (setJSON), so a record that
// is re-sent after a missing ACK overwrites itself and never duplicates.
//
// Local web page: open http://<ESP IP>/ on the same Wi-Fi. The IP is printed
// on the Pro Mini serial monitor as "ESP IP: x.x.x.x".


#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <time.h>

// ---------- Settings ----------
const char* ssid     = "Converge_2.4GHz_v3D8";
const char* password = "pUsfd9z3";

#define API_KEY       "AIzaSyDXMgYgjPJ7BDXhO2Tb-9H_StmtwVFW7k4"
#define DATABASE_URL  "https://e3embeddedsystem-4e47e-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL    "jb.r.on.fi.g.02@gmail.com"
#define USER_PASSWORD "HOTDOG1234"

#define TZ_OFFSET     (8 * 3600)   // UTC+8
#define TIME_EVERY_MS 60000UL
#define IP_EVERY_MS   30000UL

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseJson json;
ESP8266WebServer server(80);

char line[100];
byte lineLen = 0;
unsigned long lastBeat = 0;
unsigned long lastTime = 0;
unsigned long lastIp = 0;
bool timeSent = false;

// Latest reading, shown on the local web page
struct Latest {
  bool has;
  char id[20], date[14], time[12];
  int tof, hx, flt, w1, w2, rly;
  unsigned long at;                // millis() when it arrived
} latest;

// ---------- Local web page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Monitor</title>
<style>
body{margin:0;font:16px system-ui,sans-serif;background:#f4f6f8;color:#1c2733}
main{max-width:600px;margin:0 auto;padding:16px}
h1{font-size:1.3rem}
#s{color:#6b7a89;font-size:.85rem;margin:0 0 10px}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.c{background:#fff;border:1px solid #e1e6eb;border-radius:12px;padding:12px}
.l{color:#6b7a89;font-size:.8rem}.v{font-size:1.4rem;font-weight:600}
.on .v{color:#d93025}.off .v{color:#1a9a55}
</style></head><body><main>
<h1>Water Monitor</h1><p id="s">Loading...</p><div class="g" id="g"></div>
<script>
function card(l,v,st){var d=document.createElement('div');d.className='c'+(st?' '+st:'');
var a=document.createElement('div');a.className='l';a.textContent=l;
var b=document.createElement('div');b.className='v';b.textContent=v;d.append(a,b);return d}
function wet(x){return x==1?'WATER':'DRY'}
function load(){fetch('/data').then(function(r){return r.json()}).then(function(d){
var s=document.getElementById('s'),g=document.getElementById('g');
if(!d.has){s.textContent='No readings yet.';return}
s.textContent=d.id+' | '+d.date+' '+d.time+' | updated '+d.age+' s ago'+(d.age>10?' (no new data)':'');
g.replaceChildren(card('Distance (ToF)',d.tof<0?'n/a':d.tof+' mm'),
card('HX711',d.hx==-1?'n/a':d.hx),
card('Float',wet(d.flt),d.flt?'on':'off'),
card('Water 1',wet(d.w1),d.w1?'on':'off'),
card('Water 2',wet(d.w2),d.w2?'on':'off'),
card('Relay',d.rly?'ON':'OFF',d.rly?'on':'off'))
}).catch(function(){document.getElementById('s').textContent='Connection lost...'})}
load();setInterval(load,2000);
</script></main></body></html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleData() {
  char out[220];
  if (!latest.has) {
    server.send(200, "application/json", "{\"has\":0}");
    return;
  }
  snprintf(out, sizeof(out),
           "{\"has\":1,\"id\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
           "\"tof\":%d,\"hx\":%d,\"flt\":%d,\"w1\":%d,\"w2\":%d,\"rly\":%d,\"age\":%lu}",
           latest.id, latest.date, latest.time,
           latest.tof, latest.hx, latest.flt, latest.w1, latest.w2, latest.rly,
           (millis() - latest.at) / 1000UL);
  server.send(200, "application/json", out);
}

// ---------- Handle one record: "id,date,time,tof,hx,float,w1,w2,relay" ----------
void handleRecord(char *rec) {
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

  // Keep for the local web page
  strlcpy(latest.id,   f[0], sizeof(latest.id));
  strlcpy(latest.date, f[1], sizeof(latest.date));
  strlcpy(latest.time, f[2], sizeof(latest.time));
  latest.tof = (int)atol(f[3]);
  latest.hx  = (int)atol(f[4]);
  latest.flt = (int)atol(f[5]);
  latest.w1  = (int)atol(f[6]);
  latest.w2  = (int)atol(f[7]);
  latest.rly = (int)atol(f[8]);
  latest.at  = millis();
  latest.has = true;

  // Upload to Firebase
  json.clear();
  json.set("date",   f[1]);
  json.set("time",   f[2]);
  json.set("tof_mm", latest.tof);
  json.set("hx711",  latest.hx);
  json.set("float",  latest.flt);
  json.set("water1", latest.w1);
  json.set("water2", latest.w2);
  json.set("relay",  latest.rly);

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

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

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
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool online = wifiOK && Firebase.ready();

  server.handleClient();

  // Heartbeat
  if (millis() - lastBeat >= 1000) {
    lastBeat = millis();
    Serial.println(online ? F("WIFI,1") : F("WIFI,0"));
  }

  // Time for the Arduino clock
  if (wifiOK && (!timeSent || millis() - lastTime >= TIME_EVERY_MS)) {
    sendTime();
  }

  // IP address for the Arduino serial monitor
  if (wifiOK && (lastIp == 0 || millis() - lastIp >= IP_EVERY_MS)) {
    lastIp = millis();
    Serial.print(F("IP,"));
    Serial.println(WiFi.localIP());
  }

  // Records from the Arduino
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[lineLen] = '\0';
      lineLen = 0;
      if (online && strncmp(line, "DATA,", 5) == 0) {
        handleRecord(line + 5);
      }
    } else if (c != '\r' && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}
