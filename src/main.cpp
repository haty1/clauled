// Generated with Claude

// ============================================================
//  Clauled - Claude usage monitor for ESP32-C3 + SSD1306 OLED
//
//  Shows your Claude subscription usage (5h session + weekly)
//  on a small OLED, read from the Anthropic API rate headers.
//
//  Fill in the values below before compiling.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <vector>
#include <time.h>

// ── Pins ──────────────────────────────────────────────────────
#define SDA_PIN   4
#define SCL_PIN   5
#define BOOT_BTN  9

// ── Display ───────────────────────────────────────────────────
#define OLED_ADDR  0x3C
#define SCREEN_W   128
#define SCREEN_H   64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);

// ============================================================
//  USER CONFIG
//  Fill in your credentials before compiling.
//  Everything here can also be set later via the browser.
// ============================================================

// WiFi - both must be set to skip the setup portal on first boot
#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""

// OAuth token from `claude setup-token` (sk-ant-oat01-...)
// Leave blank to enter it later via the config page.
#define OAUTH_TOKEN    ""

// OAuth client_id - this is the Claude Code CLI application ID, not yours.
// Extract it from your local Claude Code binary:
//
//   node -e "
//   const fs = require('fs');
//   const b = fs.readFileSync(require('which').sync('claude'));
//   const m = b.toString('latin1').match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/g);
//   const id = m && m.find(x => x.startsWith('9d1c'));
//   console.log(id || 'not found');
//   "
//
// Paste the result below.
#define OAUTH_CLIENT_ID ""

// ── OAuth refresh endpoint ────────────────────────────────────
#define OAUTH_REFRESH_URL "https://console.anthropic.com/api/oauth/token"

// Fail at compile time if the client_id was not filled in
static_assert(OAUTH_CLIENT_ID[0] != '\0',
  "Fill in OAUTH_CLIENT_ID in src/main.cpp. "
  "Extract it from your Claude Code binary using the command in the comment above.");

// ── Config ────────────────────────────────────────────────────
struct Config {
  String wifiSSID      = WIFI_SSID;
  String wifiPass      = WIFI_PASSWORD;
  String oauthAccess   = OAUTH_TOKEN;
  String oauthRefresh  = "";

  int  pollInterval       = 60;
  int  cycleTime          = 5;
  bool showWeeklySonnet   = false;  // Sonnet-only weekly bucket (Max plans)
  bool showUptime         = false;
} cfg;

// ── Live API data ─────────────────────────────────────────────
long   reqLimit        = -1;
long   reqRemaining    = -1;
long   tokInLimit      = -1;
long   tokInRemaining  = -1;

int    unified5hPct       = -1;
int    unified7dPct       = -1;
int    unified7dSonnetPct = -1;
String reset5h            = "";
String reset7d            = "";
String reset7dSonnet      = "";

// ── Runtime ───────────────────────────────────────────────────
int  currentPage  = 0;
int  pageCount    = 0;
bool wifiOk       = false;
bool apMode       = false;
bool timeSynced   = false;
unsigned long lastPoll  = 0;
unsigned long lastCycle = 0;
String deviceIP = "";

// ── Page layout ───────────────────────────────────────────────
enum PageType { PAGE_BAR, PAGE_ROWS };
struct BarPage { String title; String valText; int pct; };
struct RowPage { struct Row { String label; String value; }; std::vector<Row> rows; };
struct Page    { PageType type; BarPage bar; RowPage rows; };
std::vector<Page> pages;

// ── Helpers ───────────────────────────────────────────────────
String fmtLong(long v) {
  if (v < 0)        return "--";
  if (v >= 1000000) return String(v / 1000000) + "M";
  if (v >= 1000)    return String(v / 1000)    + "k";
  return String(v);
}

String fmtUptime() {
  unsigned long s = millis() / 1000;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (s/3600)%24, (s/60)%60, s%60);
  return String(buf);
}

// Format a Unix epoch timestamp into a human-readable countdown.
// Returns "1h 21m", "45m", "now", or "--" if NTP not synced yet.
String fmtEpoch(const String& epochStr) {
  if (epochStr.length() == 0) return "--";
  if (!timeSynced) return "--";
  long epoch = epochStr.toInt();
  if (epoch <= 0) return "--";
  time_t now = time(nullptr);
  long secs = epoch - (long)now;
  if (secs <= 0) return "now";
  long h = secs / 3600;
  long m = (secs % 3600) / 60;
  if (h > 48) {
    char buf[16];
    time_t t = (time_t)epoch;
    struct tm* lt = localtime(&t);
    strftime(buf, sizeof(buf), "%a %H:%M", lt);
    return String(buf);
  }
  if (h > 0) return String(h) + "h " + String(m) + "m";
  return String(m) + "m";
}

int parseUtilFloat(const String& s) {
  if (s.length() == 0) return -1;
  return (int)constrain(s.toFloat() * 100.0f, 0.0f, 100.0f);
}

// ── OAuth token refresh ───────────────────────────────────────
bool refreshOAuthToken() {
  if (cfg.oauthRefresh.length() == 0) {
    Serial.println("[oauth] no refresh token stored");
    return false;
  }
  Serial.println("[oauth] refreshing access token...");

  HTTPClient http;
  http.begin(OAUTH_REFRESH_URL);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"grant_type\":\"refresh_token\","
                "\"refresh_token\":\"" + cfg.oauthRefresh + "\","
                "\"client_id\":\"" OAUTH_CLIENT_ID "\"}";

  int code = http.POST(body);
  bool ok = false;

  if (code == 200) {
    JsonDocument resp;
    String payload = http.getString();
    if (!deserializeJson(resp, payload)) {
      String newAccess = resp["access_token"] | "";
      if (newAccess.length() > 0) {
        cfg.oauthAccess = newAccess;
        JsonDocument saved;
        File f = LittleFS.open("/config.json", "r");
        if (f) { deserializeJson(saved, f); f.close(); }
        saved["oauthAccess"] = cfg.oauthAccess;
        f = LittleFS.open("/config.json", "w");
        if (f) { serializeJson(saved, f); f.close(); }
        Serial.println("[oauth] token refreshed and saved");
        ok = true;
      }
    }
  }

  if (!ok) Serial.printf("[oauth] refresh failed HTTP %d\n", code);
  http.end();
  return ok;
}

// ── Page builder ─────────────────────────────────────────────
void addBarPage(const String& title, const String& val, int p) {
  Page pg; pg.type = PAGE_BAR; pg.bar = {title, val, p};
  pages.push_back(pg);
}

void addRowsPage(std::vector<RowPage::Row> rows) {
  Page pg; pg.type = PAGE_ROWS; pg.rows.rows = rows;
  pages.push_back(pg);
}

void buildPages() {
  pages.clear();

  // Helper: bar value is "X% used" when known, "Resets in Xh Ym" when only
  // the reset time is available, or "--" when we have nothing.
  auto barVal = [](int pct, const String& resetEpoch) -> String {
    if (pct >= 0) return String(pct) + "% used";
    String r = fmtEpoch(resetEpoch);
    if (r != "--") return "Resets in " + r;
    return "--";
  };

  // Always shown
  addBarPage("Current session",   barVal(unified5hPct, reset5h),       unified5hPct);
  addBarPage("Weekly all models", barVal(unified7dPct, reset7d),       unified7dPct);

  // Optional: Sonnet weekly (Max plans)
  if (cfg.showWeeklySonnet) {
    addBarPage("Weekly Sonnet", barVal(unified7dSonnetPct, reset7dSonnet), unified7dSonnetPct);
  }

  // Optional text rows
  std::vector<RowPage::Row> rows;
  auto flush = [&]() { if (!rows.empty()) { addRowsPage(rows); rows.clear(); } };
  auto pushRow = [&](const String& label, const String& value) {
    rows.push_back({label, value});
    if ((int)rows.size() >= 3) flush();
  };

  if (cfg.showUptime) pushRow("Uptime", fmtUptime());

  flush();
  pageCount = pages.size();
  if (currentPage >= pageCount) currentPage = 0;
}

// ── OLED rendering ────────────────────────────────────────────
void oledStatus(const char* l1, const char* l2 = "", const char* l3 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 8);  display.print(l1);
  display.setCursor(0, 26); display.print(l2);
  display.setCursor(0, 44); display.print(l3);
  display.display();
}

void drawTextRow(int y, const String& label, const String& value) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, y);
  display.print(label.substring(0, 13));
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(value, 0, y, &x1, &y1, &w, &h);
  display.setCursor(128 - (int)w, y);
  display.print(value);
}

int drawBar(int y, const String& title, const String& valText, int p) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, yy1; uint16_t tw, th;

  String pctStr = (p >= 0) ? String(p) + "%" : "--";
  display.getTextBounds(pctStr, 0, y, &x1, &yy1, &tw, &th);
  display.setCursor(128 - (int)tw, y);
  display.print(pctStr);

  display.setCursor(0, y);
  int maxChars = max(1, (128 - (int)tw - 4) / 6);
  display.print(title.substring(0, maxChars));
  y += 10;

  display.drawRect(0, y, 128, 6, SSD1306_WHITE);
  if (p >= 0) {
    int fillW = (int)(128L * p / 100);
    if (fillW > 0) display.fillRect(0, y, fillW, 6, SSD1306_WHITE);
  }
  y += 8;

  display.getTextBounds(valText, 0, y, &x1, &yy1, &tw, &th);
  display.setCursor((128 - (int)tw) / 2, y);
  display.print(valText);
  y += 10;
  return y;
}

void drawHeader(int page, int total) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Claude");
  if (total > 1) {
    String pg = String(page + 1) + "/" + String(total);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(pg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(128 - (int)w, 0);
    display.print(pg);
  }
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

void drawFooter() {
  display.drawLine(0, 54, 127, 54, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Left: next poll countdown
  display.setCursor(0, 57);
  long cd = (long)cfg.pollInterval - (long)((millis() - lastPoll) / 1000);
  display.print("in " + String(max(0L, cd)) + "s");

  // Right: device IP
  String ip = deviceIP.length() ? deviceIP : "192.168.4.1";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(ip, 0, 57, &x1, &y1, &w, &h);
  display.setCursor(128 - (int)w, 57);
  display.print(ip);
}

void drawScreen() {
  buildPages();
  display.clearDisplay();

  if (wifiOk && cfg.oauthAccess.length() == 0) {
    oledStatus("Add your token", "Open this IP:", deviceIP.c_str());
    return;
  }

  if (pageCount == 0) {
    oledStatus("No metrics.", "Enable some in:", deviceIP.c_str());
    return;
  }

  Page& pg = pages[currentPage];
  drawHeader(currentPage, pageCount);
  if (pg.type == PAGE_BAR) {
    drawBar(12, pg.bar.title, pg.bar.valText, pg.bar.pct);
  } else {
    int y = 13;
    for (auto& row : pg.rows.rows) {
      drawTextRow(y, row.label, row.value);
      y += 14;
    }
  }
  drawFooter();
  display.display();
}

// ── API poll ─────────────────────────────────────────────────
void doPoll(bool isRetry = false) {
  HTTPClient http;
  http.begin("https://api.anthropic.com/v1/messages");
  http.addHeader("Content-Type",      "application/json");
  http.addHeader("anthropic-version", "2023-06-01");
  http.addHeader("Authorization",     "Bearer " + cfg.oauthAccess);

  const char* hdrs[] = {
    "anthropic-ratelimit-requests-limit",
    "anthropic-ratelimit-requests-remaining",
    "anthropic-ratelimit-tokens-limit",
    "anthropic-ratelimit-tokens-remaining",
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-7d-reset",
    "anthropic-ratelimit-unified-7d_sonnet-utilization",
    "anthropic-ratelimit-unified-7d_sonnet-reset",
    "retry-after"
  };
  http.collectHeaders(hdrs, 11);

  int code = http.POST(
    "{\"model\":\"claude-haiku-4-5-20251001\","
    "\"max_tokens\":1,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}"
  );

  Serial.printf("[poll] HTTP %d\n", code);

  if (code == 401 && !isRetry) {
    http.end();
    if (refreshOAuthToken()) doPoll(true);
    else oledStatus("Auth failed", "Update token in", "config page");
    return;
  }

  if (code > 0 && code != 401) {
    auto hdrLong = [&](const char* name) -> long {
      String v = http.header(name);
      return v.length() == 0 ? -1L : (long)v.toInt();
    };
    auto hdrStr = [&](const char* name) -> String {
      return http.header(name);
    };

    reqLimit       = hdrLong("anthropic-ratelimit-requests-limit");
    reqRemaining   = hdrLong("anthropic-ratelimit-requests-remaining");
    tokInLimit     = hdrLong("anthropic-ratelimit-tokens-limit");
    tokInRemaining = hdrLong("anthropic-ratelimit-tokens-remaining");

    unified5hPct       = parseUtilFloat(hdrStr("anthropic-ratelimit-unified-5h-utilization"));
    unified7dPct       = parseUtilFloat(hdrStr("anthropic-ratelimit-unified-7d-utilization"));
    unified7dSonnetPct = parseUtilFloat(hdrStr("anthropic-ratelimit-unified-7d_sonnet-utilization"));
    reset5h       = hdrStr("anthropic-ratelimit-unified-5h-reset");
    reset7d       = hdrStr("anthropic-ratelimit-unified-7d-reset");
    reset7dSonnet = hdrStr("anthropic-ratelimit-unified-7d_sonnet-reset");

    Serial.printf("[poll] 5h=%d%%  7d=%d%%  sonnet=%d%%\n",
      unified5hPct, unified7dPct, unified7dSonnetPct);
  } else {
    Serial.printf("[poll] error: %s\n", http.errorToString(code).c_str());
  }

  http.end();
}

void pollAPI() {
  if (!wifiOk) return;
  if (cfg.oauthAccess.length() == 0) {
    Serial.println("[poll] skipped, no token set");
    return;
  }
  oledStatus("Polling...", "api.anthropic.com");
  doPoll();
  lastPoll = millis();
}

// ── Config I/O ────────────────────────────────────────────────
bool loadConfig() {
  if (!LittleFS.exists("/config.json")) {
    Serial.println("[cfg] no saved config, using compile-time values");
    return cfg.wifiSSID.length() > 0 && cfg.wifiPass.length() > 0;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f) return cfg.wifiSSID.length() > 0 && cfg.wifiPass.length() > 0;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return false; }
  f.close();

  String s;
  s = doc["wifiSSID"]    | ""; if (s.length()) cfg.wifiSSID    = s;
  s = doc["wifiPass"]    | ""; if (s.length()) cfg.wifiPass    = s;
  s = doc["oauthAccess"] | ""; if (s.length()) cfg.oauthAccess = s;
  s = doc["oauthRefresh"]| ""; if (s.length()) cfg.oauthRefresh= s;

  cfg.pollInterval     = doc["pollInterval"]     | cfg.pollInterval;
  cfg.cycleTime        = doc["cycleTime"]        | cfg.cycleTime;
  cfg.showWeeklySonnet = doc["showWeeklySonnet"] | cfg.showWeeklySonnet;
  cfg.showUptime       = doc["showUptime"]       | cfg.showUptime;

  return cfg.wifiSSID.length() > 0 && cfg.wifiPass.length() > 0;
}

void persistConfig() {
  JsonDocument doc;
  doc["wifiSSID"]        = cfg.wifiSSID;
  doc["wifiPass"]        = cfg.wifiPass;
  doc["oauthAccess"]     = cfg.oauthAccess;
  doc["oauthRefresh"]    = cfg.oauthRefresh;
  doc["pollInterval"]    = cfg.pollInterval;
  doc["cycleTime"]       = cfg.cycleTime;
  doc["showWeeklySonnet"]= cfg.showWeeklySonnet;
  doc["showUptime"]      = cfg.showUptime;
  File f = LittleFS.open("/config.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// ── HTTP handlers ─────────────────────────────────────────────
void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) { server.send(500, "text/plain", "index.html missing - re-upload LittleFS"); return; }
  server.streamFile(f, "text/html");
  f.close();
}

void handleGetConfig() {
  JsonDocument doc;
  doc["wifiSSID"]        = cfg.wifiSSID;
  doc["wifiPass"]        = "";
  doc["hasOauthAccess"]  = cfg.oauthAccess.length() > 0;
  doc["hasOauthRefresh"] = cfg.oauthRefresh.length() > 0;
  doc["pollInterval"]    = cfg.pollInterval;
  doc["cycleTime"]       = cfg.cycleTime;
  doc["showWeeklySonnet"]= cfg.showWeeklySonnet;
  doc["showUptime"]      = cfg.showUptime;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSaveConfig() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "bad JSON"); return; }

  bool wifiChanged       = false;
  String newSSID         = doc["wifiSSID"]      | "";
  String newPass         = doc["wifiPass"]       | "";
  String newOauthAccess  = doc["oauthAccess"]    | "";
  String newOauthRefresh = doc["oauthRefresh"]   | "";

  if (newSSID.length()          && newSSID != cfg.wifiSSID) { cfg.wifiSSID  = newSSID; wifiChanged = true; }
  if (newPass.length())                                      { cfg.wifiPass  = newPass; wifiChanged = true; }
  if (newOauthAccess.length())   cfg.oauthAccess  = newOauthAccess;
  if (newOauthRefresh.length())  cfg.oauthRefresh = newOauthRefresh;

  cfg.pollInterval     = doc["pollInterval"]     | cfg.pollInterval;
  cfg.cycleTime        = doc["cycleTime"]        | cfg.cycleTime;
  cfg.showWeeklySonnet = doc["showWeeklySonnet"] | cfg.showWeeklySonnet;
  cfg.showUptime       = doc["showUptime"]       | cfg.showUptime;

  persistConfig();

  server.send(200, "application/json",
    "{\"ok\":true,\"reboot\":" + String(wifiChanged ? "true":"false") + "}");

  if (wifiChanged) {
    oledStatus("Rebooting...", "WiFi updated.");
    delay(1500);
    ESP.restart();
  }
}

void handleStatus() {
  JsonDocument doc;
  doc["ip"]               = deviceIP;
  doc["wifiSSID"]         = WiFi.SSID();
  doc["uptime"]           = millis() / 1000;
  doc["unified5hPct"]     = unified5hPct;
  doc["unified7dPct"]     = unified7dPct;
  doc["unified7dSonnetPct"] = unified7dSonnetPct;
  doc["reset5h"]          = fmtEpoch(reset5h);
  doc["reset7d"]          = fmtEpoch(reset7d);
  doc["reset7dSonnet"]    = fmtEpoch(reset7dSonnet);
  doc["lastPollAgo"]      = (millis() - lastPoll) / 1000;
  doc["nextPollIn"]       = max(0L, (long)cfg.pollInterval - (long)((millis() - lastPoll) / 1000));
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePollNow() {
  pollAPI(); drawScreen();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ── Network init ──────────────────────────────────────────────
void startAPMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ClaudeMonitor");
  deviceIP = "192.168.4.1";
  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/config", HTTP_GET,  handleGetConfig);
  server.on("/config", HTTP_POST, handleSaveConfig);
  server.on("/status", HTTP_GET,  handleStatus);
  server.begin();
  oledStatus("Setup mode", "Join WiFi:", "ClaudeMonitor");
  delay(2000);
  oledStatus("Then open:", "192.168.4.1");
}

void startStationMode() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  Serial.printf("[wifi] connecting to '%s'\n", cfg.wifiSSID.c_str());
  WiFi.begin(cfg.wifiSSID.c_str(), cfg.wifiPass.c_str());
  oledStatus("Connecting...", cfg.wifiSSID.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] failed, starting AP");
    startAPMode();
    return;
  }

  wifiOk   = true;
  deviceIP = WiFi.localIP().toString();
  Serial.printf("[wifi] connected, IP=%s\n", deviceIP.c_str());
  oledStatus("Connected!", deviceIP.c_str());

  // NTP sync — needed to convert Unix epoch reset times to countdowns
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  unsigned long ts = millis();
  while (time(nullptr) < 100000 && millis() - ts < 5000) delay(200);
  timeSynced = time(nullptr) > 100000;
  Serial.printf("[ntp] synced=%s\n", timeSynced ? "yes" : "no");

  delay(800);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/config",  HTTP_GET,  handleGetConfig);
  server.on("/config",  HTTP_POST, handleSaveConfig);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/pollnow", HTTP_POST, handlePollNow);
  server.begin();
}

// ── Setup / loop ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[oled] not found");
    while (true) delay(100);
  }
  display.clearDisplay(); display.display();

  if (!LittleFS.begin(true)) {
    oledStatus("LittleFS error!", "Re-upload data/.");
    while (true) delay(100);
  }

  bool configured = loadConfig();
  Serial.printf("[cfg] configured=%s  ssid=%s  token_len=%d\n",
    configured ? "yes" : "no",
    cfg.wifiSSID.c_str(),
    cfg.oauthAccess.length());

  if (!configured) startAPMode();
  else {
    startStationMode();
    if (wifiOk) pollAPI();
  }

  drawScreen();
  lastCycle = millis();
}

void loop() {
  server.handleClient();
  if (apMode) return;

  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    WiFi.reconnect();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(400);
    wifiOk = WiFi.status() == WL_CONNECTED;
    if (wifiOk) deviceIP = WiFi.localIP().toString();
  }

  if (wifiOk && millis() - lastPoll >= (unsigned long)cfg.pollInterval * 1000UL) {
    pollAPI(); drawScreen();
  }

  if (cfg.cycleTime > 0 && pageCount > 1
      && millis() - lastCycle >= (unsigned long)cfg.cycleTime * 1000UL) {
    currentPage = (currentPage + 1) % pageCount;
    drawScreen();
    lastCycle = millis();
  }

  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (btn == LOW && lastBtn == HIGH) {
    currentPage = (currentPage + 1) % max(1, pageCount);
    drawScreen();
    delay(200);
  }
  lastBtn = btn;
}