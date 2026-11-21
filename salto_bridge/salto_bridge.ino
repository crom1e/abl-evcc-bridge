// salto_bridge - a thin bridge between Home Assistant and an ABL Sursum EVCC wallbox (charger fw V5.0)
// over RS485, using ABL's ASCII command protocol ("!0 02" -> ">0 02 0005"), 38400 8N1.
// Hardware: Wemos D1 mini (ESP8266) + TTL-to-RS485 module with automatic direction control.
//
// The ESP makes NO control decisions of its own. It executes commands received over REST
// (Home Assistant) and reports status. Its only safeguard: 6 A <= charging current <= MAX_A (config.h).
// The only command ever written to the charger is "!0 12" (present charging current).
//
// Build:  arduino-cli compile --fqbn esp8266:esp8266:d1_mini salto_bridge
// See README.md for wiring, OTA, telnet and the API.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include "config.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Version / build time
// FW_VERSION is set at build time:  --build-property "compiler.cpp.extra_flags=-DFW_VERSION_RAW=<version>"
// (the value is stringified here to avoid shell quoting issues)
// ---------------------------------------------------------------------------
#define STR2(x) #x
#define STR(x) STR2(x)
#ifndef FW_VERSION
#ifdef FW_VERSION_RAW
#define FW_VERSION STR(FW_VERSION_RAW)
#else
#define FW_VERSION "dev"
#endif
#endif
static const char BUILD_TIME[] = __DATE__ " " __TIME__;

#ifndef MAX_A
#define MAX_A 16
#endif
static_assert(MAX_A >= 6 && MAX_A <= 30, "MAX_A must be between 6 and 30");
#ifndef READONLY_STATUS_OPEN
#define READONLY_STATUS_OPEN 1
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const int      MIN_A           = 6;      // the charger rejects lower values (tested: 0 A returns ERR)
static const uint32_t RS_BAUD         = 38400;  // 8N1
static const uint32_t RESP_TIMEOUT_MS = 300;    // the charger typically answers within 4-7 ms
static const uint32_t WRITE_GAP_MS    = 60;     // gap between the two transmissions of a write
static const uint32_t POLL_MS         = 5000;
static const uint32_t ONLINE_MS       = 15000;
static const uint8_t  SLOW_POLL_EVERY = 12;     // "!0 26" and "!0 01" every 12th poll (1 min)
static const uint32_t PUMP_TIMEOUT_MS = 2500;   // max wait inside a REST handler
static const uint32_t AUTH_FAIL_DELAY = 400;    // ms delay after a wrong token

// ---------------------------------------------------------------------------
// Logging (USB serial + telnet)
// ---------------------------------------------------------------------------
enum LogLevel : uint8_t { L_ERR = 0, L_WARN, L_INFO, L_DEBUG };
static const char* const LVL_NAME[] = {"ERR", "WARN", "INFO", "DEBUG"};
uint8_t logLevel = L_INFO;

WiFiServer telnetServer(23);
WiFiClient telnet;
bool telnetAuthed = false;

void logf(uint8_t lvl, const char* fmt, ...) {
  if (lvl > logLevel) return;
  char msg[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char line[224];
  snprintf(line, sizeof line, "[%10lu] %-5s %s\r\n", (unsigned long)millis(), LVL_NAME[lvl], msg);
  Serial.print(line);
  if (telnetAuthed && telnet.connected()) telnet.print(line);
}
#define LOGE(...) logf(L_ERR, __VA_ARGS__)
#define LOGW(...) logf(L_WARN, __VA_ARGS__)
#define LOGI(...) logf(L_INFO, __VA_ARGS__)
#define LOGD(...) logf(L_DEBUG, __VA_ARGS__)

bool parseLevel(const char* s, uint8_t& out) {
  if (!strcasecmp(s, "debug")) out = L_DEBUG;
  else if (!strcasecmp(s, "info")) out = L_INFO;
  else if (!strcasecmp(s, "warn")) out = L_WARN;
  else if (!strcasecmp(s, "err") || !strcasecmp(s, "error")) out = L_ERR;
  else return false;
  return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
uint64_t uptimeMs() {  // millis() with rollover handling
  static uint32_t last = 0, high = 0;
  uint32_t now = millis();
  if (now < last) high++;
  last = now;
  return ((uint64_t)high << 32) | now;
}

// Constant-time comparison; timing depends only on the length of the secret.
bool ctEquals(const char* given, const char* secret) {
  size_t lg = strlen(given), ls = strlen(secret);
  uint8_t diff = (lg != ls);
  for (size_t i = 0; i < ls; i++) {
    uint8_t g = (i < lg) ? (uint8_t)given[i] : 0;
    diff |= g ^ (uint8_t)secret[i];
  }
  return diff == 0;
}

void textToHex(const char* s, char* out, size_t cap) {
  static const char HEXC[] = "0123456789ABCDEF";
  size_t p = 0;
  for (size_t i = 0; s[i] && p + 3 < cap; i++) {
    if (i) out[p++] = ' ';
    out[p++] = HEXC[(uint8_t)s[i] >> 4];
    out[p++] = HEXC[(uint8_t)s[i] & 0xF];
  }
  out[p] = 0;
}

// ---------------------------------------------------------------------------
// Ring buffers: raw frames (50) and commands (20)
// ---------------------------------------------------------------------------
struct RawFrame { uint32_t ms; bool tx; char txt[40]; };
static const uint8_t RAW_N = 50;
RawFrame rawLog[RAW_N];
uint8_t rawHead = 0, rawCount = 0;

void rawAdd(bool tx, const char* frame) {
  RawFrame& r = rawLog[rawHead];
  r.ms = millis();
  r.tx = tx;
  size_t j = 0;
  for (size_t i = 0; frame[i] && j < sizeof(r.txt) - 1; i++) {
    if (frame[i] == '\r' || frame[i] == '\n') continue;
    r.txt[j++] = frame[i];
  }
  r.txt[j] = 0;
  rawHead = (rawHead + 1) % RAW_N;
  if (rawCount < RAW_N) rawCount++;
  LOGD("%s %s", tx ? "TX" : "RX", r.txt);
}

struct CmdEntry { uint32_t ms; int16_t amps; uint16_t raw; char source[8]; bool ok; char err[28]; };
static const uint8_t CMD_N = 20;
CmdEntry cmdLog[CMD_N];
uint8_t cmdHead = 0, cmdCount = 0;

void cmdAdd(int amps, uint16_t raw, const char* source, bool ok, const char* err) {
  CmdEntry& c = cmdLog[cmdHead];
  c.ms = millis();
  c.amps = amps;
  c.raw = raw;
  strlcpy(c.source, source, sizeof c.source);
  c.ok = ok;
  strlcpy(c.err, err ? err : "", sizeof c.err);
  cmdHead = (cmdHead + 1) % CMD_N;
  if (cmdCount < CMD_N) cmdCount++;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Counters { uint32_t sent, recv, invalid, timeouts; } counters = {0, 0, 0, 0};

AblValue ablStatus = {false, "", 0};   // !0 02
AblValue ablCur    = {false, "", 0};   // !0 11  present current limit (duty x10)
AblValue ablDef    = {false, "", 0};   // !0 26  default current (duty x10)
AblValue ablFw     = {false, "", 0};   // !0 01  firmware

uint32_t lastValidMs = 0;
bool     everValid = false;

int      setpointA = -1;               // last setpoint from HA (-1 = none yet)
uint32_t lastCmdMs = 0;
bool     haveCmd = false;
char     lastCmdSource[8] = "none";

bool     otaActive = false;
uint32_t otaStartMs = 0;
uint32_t rebootAt = 0;
String   resetReason;

// ---------------------------------------------------------------------------
// RS485 job queue and state machine (ABL ASCII protocol)
// ---------------------------------------------------------------------------
SoftwareSerial rs;

struct Job {
  uint16_t id;
  Purpose  purpose;
  char     cmd[20];     // e.g. "!0 12 0267" (without CRLF)
  uint8_t  sends;       // writes: always 2 transmissions. reads: up to 2 (retry on failure)
  int16_t  amps;
  uint16_t raw;
  char     source[8];
};

struct Result {
  uint16_t id;
  bool     done, ok;
  uint32_t ms;
  char     err[32];
  char     rx[40];
};

static const uint8_t QN = 8;
Job      q[QN];
uint8_t  qHead = 0, qCount = 0;
uint16_t nextJobId = 1;

enum BusState : uint8_t { BS_IDLE, BS_WAIT, BS_GAP };
BusState bs = BS_IDLE;
Job      cur;
Result   res;
uint8_t  sendNo = 0;
bool     gotValid = false;
uint32_t tJobStart = 0, tAttempt = 0, tGap = 0;

static const size_t RXMAX = 40;
char     rxBuf[RXMAX];
size_t   rxLen = 0;

static inline bool isWrite(Purpose p) { return p == P_SET; }

uint16_t enqueue(Purpose p, const char* cmd, const char* source, int amps = 0, uint16_t raw = 0) {
  if (qCount >= QN) { LOGW("queue full - job rejected"); return 0; }
  Job& j = q[(qHead + qCount) % QN];
  j.id = nextJobId++;
  if (nextJobId == 0) nextJobId = 1;
  j.purpose = p;
  strlcpy(j.cmd, cmd, sizeof j.cmd);
  j.sends = 2;
  j.amps = amps;
  j.raw = raw;
  strlcpy(j.source, source, sizeof j.source);
  qCount++;
  return j.id;
}

// Status codes from "!0 02", mapped by observation:
//   0000 no car (also briefly during transitions)   0004 car connected, waiting
//   0005 charging                                    0064 disabled (enable input pulled low)
//   0012 brief transition while disabling (meaning unknown)
const char* statusText(const char* raw) {
  if (!strcmp(raw, "0000")) return "no car";
  if (!strcmp(raw, "0004")) return "connected, waiting";
  if (!strcmp(raw, "0005")) return "charging";
  if (!strcmp(raw, "0064")) return "disabled";
  return "unknown code";
}

// A reply to "!0 XX [value]" must be ">0 XX [value]". Returns a pointer to the value field, or nullptr.
const char* replyValue(const char* reply) {
  // compare only address + command ("0 12"), not any value in the request
  const char* rq = cur.cmd + 1;
  size_t k = strlen(rq);
  const char* sp1 = strchr(rq, ' ');
  const char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : nullptr;
  if (sp2) k = sp2 - rq;
  if (reply[0] != '>' || strncmp(reply + 1, rq, k) != 0) return nullptr;
  const char* v = reply + 1 + k;
  while (*v == ' ') v++;
  return v;
}

bool isDigits4(const char* s) {
  for (int i = 0; i < 4; i++) if (s[i] < '0' || s[i] > '9') return false;
  return s[4] == 0 || s[4] == ' ';
}

bool valueValid(Purpose p, const char* v) {
  switch (p) {
    case P_STATUS:
    case P_CUR:
    case P_DEF: return isDigits4(v);
    case P_FW:  return v[0] == 'V' && v[1];
    case P_SET: return v[0] == 0;   // acknowledgement ">0 12 " without a value
  }
  return false;
}

void store(AblValue& a, const char* v, const char* name) {
  if (!a.have || strcmp(a.raw, v) != 0) LOGI("%s = %s", name, v);
  strlcpy(a.raw, v, sizeof a.raw);
  a.have = true;
  a.ms = millis();
}

// Converts a 4-digit current value (duty x10, A = value x 0.06). -1 if unknown.
float ablAmps(const AblValue& a) {
  if (!a.have) return -1;
  return round(atol(a.raw) * 0.06 * 10) / 10.0;
}

void sendAttempt() {
  while (rs.available()) rs.read();  // drop stale bytes
  rs.print(cur.cmd);
  rs.print("\r\n");
  rawAdd(true, cur.cmd);
  counters.sent++;
  sendNo++;
  tAttempt = millis();
  rxLen = 0;
  bs = BS_WAIT;
}

void finishJob() {
  res.done = true;
  res.ok = gotValid;
  if (!gotValid && !res.err[0]) strlcpy(res.err, "timeout", sizeof res.err);
  if (isWrite(cur.purpose)) {
    cmdAdd(cur.amps, cur.raw, cur.source, res.ok, res.err);
    logf(res.ok ? L_INFO : L_ERR, "set %d A (%s): %s %s", cur.amps, cur.cmd, res.ok ? "OK" : "FAILED", res.ok ? "" : res.err);
  } else if (!res.ok) {
    LOGD("%s failed: %s", cur.cmd, res.err);
  }
  bs = BS_IDLE;
}

void attemptEnded() {
  // Writes are always sent twice. Reads are retried once, only if the first attempt failed.
  bool more = sendNo < cur.sends && (isWrite(cur.purpose) || !gotValid);
  if (more) { bs = BS_GAP; tGap = millis(); }
  else finishJob();
}

void handleReply() {
  rxBuf[rxLen] = 0;
  rawAdd(false, rxBuf);
  const char* v = replyValue(rxBuf);
  if (v && !strncmp(v, "ERR", 3)) {
    // Valid reply, but the charger rejected the command
    counters.recv++;
    lastValidMs = millis();
    everValid = true;
    if (!gotValid) {
      strlcpy(res.err, "charger replied ERR", sizeof res.err);
      strlcpy(res.rx, rxBuf, sizeof res.rx);
    }
    logf(isWrite(cur.purpose) ? L_WARN : L_DEBUG, "charger rejected %s (ERR)", cur.cmd);
  } else if (v && valueValid(cur.purpose, v)) {
    counters.recv++;
    lastValidMs = millis();
    everValid = true;
    if (!gotValid) {
      gotValid = true;
      res.err[0] = 0;
      res.ms = millis() - tJobStart;
      strlcpy(res.rx, rxBuf, sizeof res.rx);
      switch (cur.purpose) {
        case P_STATUS: store(ablStatus, v, "status (!0 02)"); break;
        case P_CUR:    store(ablCur, v, "current limit (!0 11)"); break;
        case P_DEF:    store(ablDef, v, "default current (!0 26)"); break;
        case P_FW:     store(ablFw, v, "charger firmware (!0 01)"); break;
        case P_SET:    break;
      }
    }
  } else {
    counters.invalid++;
    if (!gotValid) strlcpy(res.err, "invalid reply", sizeof res.err);
    LOGW("invalid reply to %s: %s", cur.cmd, rxBuf);
  }
  attemptEnded();
}

void rsTick() {
  uint32_t now = millis();
  switch (bs) {
    case BS_IDLE:
      if (otaActive || qCount == 0) return;
      cur = q[qHead];
      qHead = (qHead + 1) % QN;
      qCount--;
      memset(&res, 0, sizeof res);
      res.id = cur.id;
      sendNo = 0;
      gotValid = false;
      tJobStart = now;
      sendAttempt();
      return;

    case BS_WAIT:
      while (rs.available()) {
        char c = (char)rs.read();
        if (c == '\r') continue;
        if (c == '\n') { if (rxLen) { handleReply(); return; } continue; }
        if (rxLen < RXMAX - 1) rxBuf[rxLen++] = (c >= 0x20 && c < 0x7F) ? c : '.';
      }
      if (now - tAttempt > RESP_TIMEOUT_MS) {
        if (rxLen) { handleReply(); return; }   // incomplete reply without line ending
        counters.timeouts++;
        LOGD("timeout on %s (attempt %u)", cur.cmd, sendNo);
        attemptEnded();
      }
      return;

    case BS_GAP:
      if (now - tGap >= WRITE_GAP_MS) sendAttempt();
      return;
  }
}

// Runs the RS485 state machine until the job is done (used only by REST handlers).
bool pumpUntilDone(uint16_t id) {
  uint32_t t0 = millis();
  while (millis() - t0 < PUMP_TIMEOUT_MS) {
    rsTick();
    if (res.id == id && res.done) return true;
    yield();
  }
  return false;
}

bool isOnline() { return everValid && (millis() - lastValidMs) < ONLINE_MS; }

// ---------------------------------------------------------------------------
// Web / REST
// ---------------------------------------------------------------------------
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

bool hasToken() {
  String h = server.header("Authorization");  // collected automatically by ESP8266WebServer
  if (!h.startsWith("Bearer ")) return false;
  return ctEquals(h.c_str() + 7, API_TOKEN);
}

bool requireToken() {
  if (hasToken()) return true;
  LOGW("wrong/missing token from %s", server.client().remoteIP().toString().c_str());
  delay(AUTH_FAIL_DELAY);
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

bool requireReadAccess() { return READONLY_STATUS_OPEN ? true : requireToken(); }

void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendError(int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(code, d);
}

bool parseBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) { sendError(400, "missing JSON body"); return false; }
  if (deserializeJson(doc, server.arg("plain"))) { sendError(400, "invalid JSON"); return false; }
  return true;
}

void fillStatus(JsonDocument& d) {
  d["online"] = isOnline();
  if (ablStatus.have) {
    d["status"] = statusText(ablStatus.raw);
    d["status_code"] = ablStatus.raw;
  } else {
    d["status"] = "unknown";
    d["status_code"] = nullptr;
  }
  if (setpointA >= 0) d["setpoint_a"] = setpointA; else d["setpoint_a"] = nullptr;
  float ca = ablAmps(ablCur);
  if (ablCur.have) { d["applied_raw"] = ablCur.raw; d["applied_a"] = ca; }
  else { d["applied_raw"] = nullptr; d["applied_a"] = nullptr; }
  float da = ablAmps(ablDef);
  if (ablDef.have) { d["default_raw"] = ablDef.raw; d["default_a"] = da; }
  else { d["default_raw"] = nullptr; d["default_a"] = nullptr; }
  if (ablFw.have) d["charger_fw"] = ablFw.raw; else d["charger_fw"] = nullptr;
  d["min_a"] = MIN_A;
  d["max_a"] = MAX_A;
  if (haveCmd) d["seconds_since_command"] = (millis() - lastCmdMs) / 1000; else d["seconds_since_command"] = nullptr;
  d["last_command_source"] = lastCmdSource;
  JsonObject c = d["counters"].to<JsonObject>();
  c["sent"] = counters.sent;
  c["recv"] = counters.recv;
  c["invalid"] = counters.invalid;
  c["timeouts"] = counters.timeouts;
  d["mac"] = WiFi.macAddress();   // stable ID for the HA integration
  d["uptime_s"] = (uint32_t)(uptimeMs() / 1000);
  d["rssi"] = WiFi.RSSI();
  d["heap_free"] = ESP.getFreeHeap();
  d["reset_reason"] = resetReason;
  d["fw_version"] = FW_VERSION;
  d["build_time"] = BUILD_TIME;
  d["log_level"] = LVL_NAME[logLevel];
  d["ota_active"] = otaActive;
}

void handleStatus() {
  if (!requireReadAccess()) return;
  JsonDocument d;
  fillStatus(d);
  sendJson(200, d);
}

String jsonStr(const char* s) {
  String o = "\"";
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) o += '.';
    else o += c;
  }
  o += '"';
  return o;
}

// /api/log is streamed in chunks to save heap. Newest first.
void handleLog() {
  if (!requireReadAccess()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk;
  chunk.reserve(512);
  chunk = "{\"now_ms\":";
  chunk += millis();
  chunk += ",\"commands\":[";
  for (uint8_t i = 0; i < cmdCount; i++) {
    const CmdEntry& c = cmdLog[(cmdHead + CMD_N - 1 - i) % CMD_N];
    if (i) chunk += ',';
    chunk += "{\"ms\":"; chunk += c.ms;
    chunk += ",\"amps\":"; chunk += c.amps;
    chunk += ",\"raw\":"; chunk += c.raw;
    chunk += ",\"source\":"; chunk += jsonStr(c.source);
    chunk += ",\"ok\":"; chunk += c.ok ? "true" : "false";
    chunk += ",\"error\":"; chunk += jsonStr(c.err);
    chunk += '}';
    if (chunk.length() > 400) { server.sendContent(chunk); chunk = ""; }
  }
  chunk += "],\"frames\":[";
  for (uint8_t i = 0; i < rawCount; i++) {
    const RawFrame& r = rawLog[(rawHead + RAW_N - 1 - i) % RAW_N];
    char hx[120];
    textToHex(r.txt, hx, sizeof hx);
    if (i) chunk += ',';
    chunk += "{\"ms\":"; chunk += r.ms;
    chunk += ",\"dir\":"; chunk += r.tx ? "\"tx\"" : "\"rx\"";
    chunk += ",\"ascii\":"; chunk += jsonStr(r.txt);
    chunk += ",\"hex\":"; chunk += jsonStr(hx);
    chunk += '}';
    if (chunk.length() > 400) { server.sendContent(chunk); chunk = ""; }
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

void noteCommand(const char* source) {
  lastCmdMs = millis();
  haveCmd = true;
  strlcpy(lastCmdSource, source, sizeof lastCmdSource);
}

// POST /api/current {"amps": N}  ->  "!0 12 <round(N/0.06)>", N = 6..MAX_A, read back with "!0 11".
// Tested while charging: 6, 10 and 16 A work. 0 A ("!0 12 1000") is rejected by the charger with ERR.
void handleCurrent() {
  if (!requireToken()) return;
  JsonDocument body;
  if (!parseBody(body)) return;
  JsonVariant v = body["amps"];
  if (!v.is<float>() && !v.is<int>()) { sendError(400, "amps missing or not a number"); return; }
  float f = v.as<float>();
  int amps = (int)lroundf(f);
  if (amps == 0 && fabsf(f) < 0.001f) {
    cmdAdd(0, 0, "rest", false, "0 A not supported");
    LOGW("rejected 0 A: the charger does not support pausing");
    sendError(400, "0 A (pause) is not supported by the charger - use 6 A as the minimum");
    return;
  }
  if (fabsf(f - amps) > 0.001f || amps < MIN_A || amps > MAX_A) {
    char m[48];
    snprintf(m, sizeof m, "amps must be an integer %d-%d", MIN_A, MAX_A);
    cmdAdd(amps, 0, "rest", false, "rejected (400)");
    LOGW("rejected setpoint %.2f A", f);
    sendError(400, m);
    return;
  }
  noteCommand("rest");
  setpointA = amps;
  uint16_t raw = (uint16_t)lround(amps / 0.06);
  char cmd[16];
  snprintf(cmd, sizeof cmd, "!0 12 %04u", raw);
  LOGI("HA: set current %d A (%s)", amps, cmd);
  uint16_t id = enqueue(P_SET, cmd, "rest", amps, raw);
  if (!id) { sendError(503, "queue full"); return; }
  uint16_t rid = enqueue(P_CUR, "!0 11", "rest");  // read back
  bool fin = pumpUntilDone(id);
  bool ok = fin && res.id == id && res.ok;
  char err[32];
  strlcpy(err, !fin ? "timeout in queue" : res.err, sizeof err);
  if (rid) pumpUntilDone(rid);
  JsonDocument d;
  fillStatus(d);
  d["command_ok"] = ok;
  d["command_raw"] = cmd;
  if (!ok) d["command_error"] = err;
  sendJson(ok ? 200 : 502, d);
}

// POST /api/resume: there is no known pause/resume command in the charger's protocol. Returns 501.
void handleResume() {
  if (!requireToken()) return;
  sendError(501, "pause/resume is not supported by the charger");
}

// GET /api/auth: validates the token without touching the charger (used by the HA config flow)
void handleAuth() {
  if (!requireToken()) return;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLogLevel() {
  if (!requireToken()) return;
  JsonDocument body;
  if (!parseBody(body)) return;
  uint8_t lv;
  if (!parseLevel(body["level"] | "", lv)) { sendError(400, "level: debug|info|warn|err"); return; }
  logLevel = lv;
  LOGI("log level = %s", LVL_NAME[lv]);
  JsonDocument d;
  d["level"] = LVL_NAME[lv];
  sendJson(200, d);
}

void handleReboot() {
  if (!requireToken()) return;
  LOGW("reboot requested via REST");
  server.send(200, "application/json", "{\"rebooting\":true}");
  rebootAt = millis() + 300;
}

// ---------------------------------------------------------------------------
// Web page (read-only)
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Salto bridge</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1d21;--mut:#6b7280;--ok:#15803d;--bad:#b91c1c;--line:#e5e7eb}
@media(prefers-color-scheme:dark){:root{--bg:#111316;--card:#1b1e23;--fg:#e8eaed;--mut:#9aa0a6;--ok:#4ade80;--bad:#f87171;--line:#2d3137}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:16px}h1{font-size:18px;margin:0 0 12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.big{font-size:34px;font-weight:600}.mut{color:var(--mut);font-size:13px}
.ok{color:var(--ok)}.bad{color:var(--bad)}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{text-align:left;padding:3px 6px;border-bottom:1px solid var(--line);vertical-align:top}
.mono{font-family:ui-monospace,Consolas,monospace;word-break:break-all}
.scroll{max-height:340px;overflow:auto}h2{font-size:15px;margin:18px 0 8px}
</style></head><body><main>
<h1>Salto bridge <span id="on" class="mut">…</span></h1>
<div class="grid">
<div class="card"><div class="mut">Charging current limit</div><div class="big" id="amps">–</div><div class="mut" id="kw">–</div></div>
<div class="card"><div class="mut">Charger status</div><div class="big" id="st">–</div><div class="mut" id="stc">–</div></div>
<div class="card"><div class="mut">Last command from HA</div><div class="big" id="cmd">–</div><div class="mut" id="sp">–</div></div>
</div>
<h2>System</h2>
<div class="card"><table id="sys"></table></div>
<h2>Commands</h2>
<div class="card scroll"><table><thead><tr><th>Time</th><th>Value</th><th>Source</th><th>Result</th></tr></thead><tbody id="cmds"></tbody></table></div>
<h2>RS485 frames</h2>
<div class="card scroll"><table><thead><tr><th>ms</th><th></th><th>ASCII</th><th>Hex</th></tr></thead><tbody id="frames"></tbody></table></div>
</main><script>
const tok=location.hash.slice(1),H=tok?{headers:{Authorization:'Bearer '+tok}}:{};
const $=id=>document.getElementById(id),esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const dur=s=>{if(s==null)return'–';s=+s;const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m?m+'m '+(s%60)+'s':s+'s'};
let nowMs=0;
async function tick(){
 try{const s=await (await fetch('/api/status',H)).json();
  $('on').textContent=s.online?'● online':'● offline';$('on').className=s.online?'ok':'bad';
  const a=s.applied_a;$('amps').textContent=a==null?'–':a+' A';
  $('kw').textContent=a==null?'unknown':(a*230/1000).toFixed(2)+' kW (single-phase, 230 V) · raw '+s.applied_raw;
  $('st').textContent=s.status;$('stc').textContent='code '+(s.status_code??'–');
  $('cmd').textContent=s.seconds_since_command==null?'none':dur(s.seconds_since_command)+' ago';
  $('sp').textContent='setpoint '+(s.setpoint_a==null?'–':s.setpoint_a+' A')+' · allowed '+s.min_a+'–'+s.max_a+' A · source '+s.last_command_source;
  const c=s.counters,rows=[['Default current',s.default_a==null?'–':s.default_a+' A (raw '+s.default_raw+')'],
  ['Charger firmware',s.charger_fw??'–'],
  ['Counters',`sent ${c.sent} · received ${c.recv} · invalid ${c.invalid} · timeouts ${c.timeouts}`],
  ['WiFi signal',s.rssi+' dBm'],['Free heap',s.heap_free+' B'],['Uptime',dur(s.uptime_s)],['Firmware',s.fw_version],['Build time',s.build_time],
  ['Last reset',s.reset_reason],['Log level',s.log_level]];
  $('sys').innerHTML=rows.map(r=>`<tr><th>${r[0]}</th><td>${esc(r[1])}</td></tr>`).join('');
 }catch(e){$('on').textContent='● no connection';$('on').className='bad'}
 try{const l=await (await fetch('/api/log',H)).json();nowMs=l.now_ms;
  const ago=ms=>dur(Math.round((nowMs-ms)/1000))+' ago';
  $('cmds').innerHTML=l.commands.map(c=>`<tr><td>${ago(c.ms)}</td><td>${c.amps} A <span class="mut">(${c.raw})</span></td><td>${esc(c.source)}</td><td class="${c.ok?'ok':'bad'}">${c.ok?'OK':esc(c.error||'error')}</td></tr>`).join('');
  $('frames').innerHTML=l.frames.map(f=>`<tr><td>${f.ms}</td><td>${f.dir}</td><td class="mono">${esc(f.ascii)}</td><td class="mono">${esc(f.hex)}</td></tr>`).join('');
 }catch(e){}
}
tick();setInterval(tick,2000);
</script></body></html>)HTML";

void handleIndex() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

// ---------------------------------------------------------------------------
// Telnet (read-only insight: status, log, log level, reboot)
// ---------------------------------------------------------------------------
char    tLine[96];
uint8_t tLen = 0;
uint8_t tIac = 0;

void tprintf(const char* fmt, ...) {
  if (!telnet.connected()) return;
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  telnet.print(b);
}

void telnetStatus() {
  tprintf("fw %s (%s)  uptime %lu s  heap %u  rssi %d  reset: %s\r\n", FW_VERSION, BUILD_TIME,
          (unsigned long)(uptimeMs() / 1000), ESP.getFreeHeap(), WiFi.RSSI(), resetReason.c_str());
  tprintf("online: %s  status: %s (!0 02 = %s)  charger fw %s\r\n", isOnline() ? "yes" : "no",
          ablStatus.have ? statusText(ablStatus.raw) : "unknown", ablStatus.have ? ablStatus.raw : "-",
          ablFw.have ? ablFw.raw : "-");
  tprintf("current limit (!0 11): %s = %.1f A  default (!0 26): %s = %.1f A  allowed %d-%d A\r\n",
          ablCur.have ? ablCur.raw : "-", ablAmps(ablCur), ablDef.have ? ablDef.raw : "-", ablAmps(ablDef), MIN_A, MAX_A);
  tprintf("setpoint: %d A  last command: %s, %ld s ago\r\n", setpointA, lastCmdSource,
          haveCmd ? (long)((millis() - lastCmdMs) / 1000) : -1L);
  tprintf("counters: sent %u received %u invalid %u timeouts %u  log level %s  queue %u\r\n",
          counters.sent, counters.recv, counters.invalid, counters.timeouts, LVL_NAME[logLevel], qCount);
}

void telnetLog() {
  tprintf("-- commands (newest first) --\r\n");
  for (uint8_t i = 0; i < cmdCount; i++) {
    const CmdEntry& c = cmdLog[(cmdHead + CMD_N - 1 - i) % CMD_N];
    tprintf("%10lu %3d A raw %4u %-6s %s %s\r\n", (unsigned long)c.ms, c.amps, c.raw, c.source, c.ok ? "OK" : "FAILED", c.err);
  }
  tprintf("-- latest frames (newest first) --\r\n");
  for (uint8_t i = 0; i < rawCount && i < 20; i++) {
    const RawFrame& r = rawLog[(rawHead + RAW_N - 1 - i) % RAW_N];
    tprintf("%10lu %s %s\r\n", (unsigned long)r.ms, r.tx ? "TX" : "RX", r.txt);
  }
}

void telnetCommand(char* line) {
  while (*line == ' ') line++;
  if (!telnetAuthed) {
    if (ctEquals(line, API_TOKEN)) {
      telnetAuthed = true;
      tprintf("OK. Type 'help'.\r\n");
      LOGI("telnet: logged in from %s", telnet.remoteIP().toString().c_str());
    } else {
      LOGW("telnet: wrong token from %s", telnet.remoteIP().toString().c_str());
      delay(AUTH_FAIL_DELAY);
      tprintf("Wrong token.\r\n");
      telnet.stop();
    }
    return;
  }
  char* cmd = strtok(line, " ");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " ");
  if (!strcasecmp(cmd, "help")) {
    tprintf("status | log | level debug|info|warn | reboot | help | quit\r\n");
  } else if (!strcasecmp(cmd, "status")) {
    telnetStatus();
  } else if (!strcasecmp(cmd, "log")) {
    telnetLog();
  } else if (!strcasecmp(cmd, "level")) {
    uint8_t lv;
    if (a1 && parseLevel(a1, lv)) { logLevel = lv; tprintf("log level = %s\r\n", LVL_NAME[lv]); }
    else tprintf("usage: level debug|info|warn\r\n");
  } else if (!strcasecmp(cmd, "reboot")) {
    tprintf("rebooting...\r\n");
    LOGW("reboot requested via telnet");
    rebootAt = millis() + 300;
  } else if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit")) {
    telnet.stop();
    telnetAuthed = false;
  } else {
    tprintf("unknown command, type 'help'\r\n");
  }
}

void telnetTick() {
  if (telnetServer.hasClient()) {
    if (telnet.connected()) { telnet.println("New client connected - disconnecting."); telnet.stop(); }
    telnet = telnetServer.accept();
    telnet.setNoDelay(true);
    telnetAuthed = false;
    tLen = 0;
    tIac = 0;
    tprintf("salto_bridge %s\r\nToken: ", FW_VERSION);
  }
  if (!telnet.connected()) { telnetAuthed = false; return; }
  while (telnet.available()) {
    uint8_t c = telnet.read();
    if (tIac) { tIac--; continue; }
    if (c == 255) { tIac = 2; continue; }  // telnet IAC negotiation
    if (c == '\r') continue;
    if (c == '\n') {
      tLine[tLen] = 0;
      tLen = 0;
      telnetCommand(tLine);
      if (!telnet.connected()) return;
      continue;
    }
    if (c >= 0x20 && c < 0x7F && tLen < sizeof(tLine) - 1) tLine[tLen++] = c;
  }
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    LOGI("OTA (espota) started - RS485 paused");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    int pct = total ? (int)(done * 100ULL / total) : 0;
    if (pct / 10 != lastPct / 10) { lastPct = pct; LOGI("OTA %d %%", pct); }
  });
  ArduinoOTA.onEnd([]() { LOGI("OTA done - rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaActive = false;
    LOGE("OTA error %u - RS485 resumed", (unsigned)e);
  });
  ArduinoOTA.begin();

  // HTTP OTA (/update). The Updater callbacks cover this path.
  Update.onStart([]() {
    if (!otaActive) LOGI("HTTP OTA started - RS485 paused");
    otaActive = true;
  });
  Update.onProgress([](size_t done, size_t total) {
    static int lastPct = -1;
    int pct = total ? (int)(done * 100ULL / total) : 0;
    if (pct / 10 != lastPct / 10) { lastPct = pct; LOGD("Update %d %%", pct); }
  });
  Update.onError([](uint8_t e) {
    otaActive = false;
    LOGE("Update error %u", (unsigned)e);
  });
  httpUpdater.setup(&server, "/update", UPDATE_USER, UPDATE_PASS);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
uint32_t lastPollMs = 0;
uint8_t  pollNo = 0;
bool     wasConnected = false;

// WiFi status codes: 0 idle, 1 SSID not found, 3 connected, 4 failed, 6 wrong password, 7 disconnected
void wifiScanLog() {
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool me = WiFi.SSID(i) == WIFI_SSID;
    found |= me;
    LOGI("  network: %-24s channel %2d  %4d dBm%s", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), me ? "  <== configured" : "");
  }
  if (!found) LOGW("SSID \"%s\" not found among %d networks (2.4 GHz only)", WIFI_SSID, n);
  WiFi.scanDelete();
}

void enqueueSlowPolls(const char* source) {
  enqueue(P_DEF, "!0 26", source);
  // "!0 01" returns ERR while a car is charging - only ask when firmware is unknown and no car is connected
  bool idle = !ablStatus.have || !strcmp(ablStatus.raw, "0000");
  if (!ablFw.have && idle) enqueue(P_FW, "!0 01", source);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  resetReason = ESP.getResetReason();
  LOGI("salto_bridge %s (%s) - reset: %s", FW_VERSION, BUILD_TIME, resetReason.c_str());
  LOGI("allowed charging current %d-%d A, status open=%d", MIN_A, MAX_A, READONLY_STATUS_OPEN);

  rs.begin(RS_BAUD, SWSERIAL_8N1, D7, D8, false, 256);  // RX=D7, TX=D8
  if (!rs) LOGE("SoftwareSerial init failed");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(100); }
  if (WiFi.status() == WL_CONNECTED) LOGI("WiFi OK, IP %s", WiFi.localIP().toString().c_str());
  else {
    LOGW("WiFi not connected (status %d) - retrying in the background", (int)WiFi.status());
    wifiScanLog();
  }

  setupOta();  // also starts mDNS (<HOSTNAME>.local)
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/current", HTTP_POST, handleCurrent);
  server.on("/api/resume", HTTP_POST, handleResume);
  server.on("/api/auth", HTTP_GET, handleAuth);
  server.on("/api/debug/loglevel", HTTP_POST, handleLogLevel);
  server.on("/api/debug/reboot", HTTP_POST, handleReboot);
  server.onNotFound([]() { sendError(404, "not found"); });
  server.begin();

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  // Startup: read only - nothing is written until HA sends a command.
  enqueueSlowPolls("boot");
  enqueue(P_STATUS, "!0 02", "boot");
  enqueue(P_CUR, "!0 11", "boot");
  lastPollMs = millis();
  LOGI("ready - http://%s.local/  telnet %s.local 23", HOSTNAME, HOSTNAME);
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  telnetTick();

  uint32_t now = millis();
  // During OTA: no RS485 traffic (rsTick does not start new jobs). Safety net in case
  // an upload is aborted without an error callback.
  if (otaActive) {
    if (!otaStartMs) otaStartMs = now;
    if (now - otaStartMs > 120000) { otaActive = false; otaStartMs = 0; LOGW("OTA did not complete - RS485 resumed"); }
  } else {
    otaStartMs = 0;
  }
  rsTick();

  if (!otaActive && bs == BS_IDLE && qCount == 0 && now - lastPollMs >= POLL_MS) {
    lastPollMs = now;
    enqueue(P_STATUS, "!0 02", "poll");
    enqueue(P_CUR, "!0 11", "poll");
    if (++pollNo % SLOW_POLL_EVERY == 0) enqueueSlowPolls("poll");
  }

  bool conn = WiFi.status() == WL_CONNECTED;
  if (conn != wasConnected) {
    wasConnected = conn;
    if (conn) LOGI("WiFi connected, IP %s, RSSI %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else LOGW("WiFi disconnected");
  }
  static uint32_t lastWifiWarn = 0;
  if (!conn && now - lastWifiWarn > 30000) {
    lastWifiWarn = now;
    LOGW("WiFi still disconnected (status %d)", (int)WiFi.status());
  }

  if (rebootAt && (int32_t)(now - rebootAt) >= 0) {
    if (telnet.connected()) telnet.stop();
    ESP.restart();
  }
}
