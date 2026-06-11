/*
  Ctrl+Alt+Defib - ESP32 Ethernet UPS Server Controller
  
  Programmed by Shane Aune - shaneaune.com
  Github: https://github.com/shaneaune

  Purpose:
    Monitors a mains-present signal and coordinates a controlled
    shutdown/startup sequence for a target server.

  Hardware:
    - ESP32-ETH02 with LAN8720 Ethernet PHY
    - GPIO36 reads a mains-present signal
    - The external sensing circuit must already level-shift/isolate
      the source signal to a safe ESP32 voltage

  Power-loss behavior:
    1. Detect loss of mains
    2. Ignore brief glitches using input debounce and outage validation
    3. After a confirmed outage, wait shutdownDelaySec
    4. Send an HTTP shutdown request to the target service
    5. When mains returns and remains stable, wait startupDelaySec
    6. Send Wake-on-LAN packets to restart the target server

  Design notes:
    - Main control flow is millis()-driven and mostly non-blocking
    - Ethernet is required for HTTP shutdown and Wake-on-LAN actions
    - Settings and outage counters persist in Preferences (NVS)
    - A watchdog timer is enabled to recover from unexpected stalls
*/

#include <Arduino.h>

#define ETH_PHY_TYPE ETH_PHY_LAN8720
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_PHY_POWER 16
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN

#include <ETH.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <time.h>

// GPIO36 is input-only on ESP32 and is used only for mains sensing.
// The external circuit must convert the source signal to a safe logic level.
constexpr uint8_t MAINS_PIN = 36;

constexpr const char *PROJECT_NAME = "Ctrl+Alt+Defib";
constexpr const char *FIRMWARE_VERSION = "v0.1.0";

constexpr const char *BUILD_DATE = __DATE__;
constexpr const char *BUILD_TIME = __TIME__;

constexpr const char *UI_USERNAME = "admin";
constexpr const char *UI_PASSWORD = "CHANGE-ME";

// Log type labels are centralized to avoid typo-prone string literals.
constexpr const char *LOG_INFO = "INFO";
constexpr const char *LOG_POWER_LOSS = "POWER_LOSS";
constexpr const char *LOG_CANCELLED = "CANCELLED";
constexpr const char *LOG_SHUTDOWN = "SHUTDOWN";
constexpr const char *LOG_RETURN = "RETURN";
constexpr const char *LOG_STARTUP = "STARTUP";
constexpr const char *LOG_ERROR = "ERROR";

constexpr unsigned long MAINS_DEBOUNCE_MS = 1000;
constexpr int LOG_SIZE = 50;
constexpr uint32_t WDT_TIMEOUT_MS = 30000;
constexpr unsigned long MILLIS_PER_SEC = 1000UL;

WebServer server(80);
Preferences prefs;

bool ethConnected = false;
bool mainsPresent = false;
bool shutdownSent = false;
bool wolSent = false;
bool timeSynced = false;
bool webServerStarted = false;

// lastRawMainsReading tracks the immediate GPIO level.
// mainsPresent is the debounced signal consumed by the state machine.
bool lastRawMainsReading = false;
unsigned long rawMainsChangedMs = 0;

uint32_t powerLossTriggerCount = 0;
uint32_t fullCycleCount = 0;
uint64_t totalPowerOutSec = 0;
uint32_t lastOutageSec = 0;
String lastCycleResult = "None";

unsigned long outageStartMs = 0;
unsigned long outageDetectStartMs = 0;
unsigned long powerReturnStartMs = 0;
unsigned long startupDelayStartMs = 0;
unsigned long lastWolMs = 0;
uint8_t wolRetryCounter = 0;

// State machine overview:
// - POWER_ON_IDLE: normal operation, mains present
// - POWER_OUTAGE_DETECTED: outage seen, waiting to confirm it is real
// - POWER_FAIL_PENDING: outage confirmed, shutdown countdown running
// - SHUTDOWN_SENT: shutdown already requested, waiting for mains return
// - POWER_RETURN_PENDING: mains returned, checking that it remains stable
// - WOL_PENDING: stable power confirmed, waiting to send WOL sequence
enum SystemState {
  POWER_ON_IDLE,
  POWER_OUTAGE_DETECTED,
  POWER_FAIL_PENDING,
  SHUTDOWN_SENT,
  POWER_RETURN_PENDING,
  WOL_PENDING
};

SystemState state = POWER_ON_IDLE;

struct Config {
  uint32_t shutdownDelaySec = 300;
  uint32_t minOutageTriggerSec = 3;
  uint32_t powerStableDelaySec = 120;
  uint32_t startupDelaySec = 120;
  uint8_t wolRetries = 3;
  uint32_t wolRetrySpacingSec = 10;

  char shutdownHost[32] = "10.0.0.117";
  uint16_t shutdownPort = 8080;
  char shutdownToken[96] = "8f3c91a2b7d44e1c9a5f2b8d7c6e4a11";

  uint8_t wolMac[6] = { 0xC4, 0x34, 0x6B, 0x7E, 0x16, 0x8E };
};

Config cfg;

struct LogEntry {
  String type;
  String time;
  String message;
};

LogEntry eventLog[LOG_SIZE];
int logIndex = 0;
int logCount = 0;

unsigned long secondsToMs(uint32_t seconds) {
  return static_cast<unsigned long>(seconds) * MILLIS_PER_SEC;
}

String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(
    buf,
    sizeof(buf),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],
    mac[1],
    mac[2],
    mac[3],
    mac[4],
    mac[5]);
  return String(buf);
}

bool parseMac(const String &s, uint8_t mac[6]) {
  int values[6];
  if (sscanf(
        s.c_str(),
        "%x:%x:%x:%x:%x:%x",
        &values[0],
        &values[1],
        &values[2],
        &values[3],
        &values[4],
        &values[5])
      != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    mac[i] = static_cast<uint8_t>(values[i]);
  }

  return true;
}

String nowString() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
  }

  const unsigned long seconds = millis() / MILLIS_PER_SEC;
  char buf[24];
  snprintf(buf, sizeof(buf), "uptime %lu sec", seconds);
  return String(buf);
}

// Appends a log entry to the ring buffer and mirrors it to Serial.
// The ring buffer keeps the newest LOG_SIZE events only.
void addLog(const String &type, const String &msg) {
  eventLog[logIndex].type = type;
  eventLog[logIndex].time = nowString();
  eventLog[logIndex].message = msg;

  const String line = eventLog[logIndex].time + " [" + type + "] " + msg;

  logIndex = (logIndex + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) {
    logCount++;
  }

  Serial.println(line);
}

void addLog(const String &msg) {
  addLog(LOG_INFO, msg);
}

String formatDuration(uint64_t totalSec) {
  const uint64_t days = totalSec / 86400;
  const uint8_t hours = (totalSec % 86400) / 3600;
  const uint8_t minutes = (totalSec % 3600) / 60;
  const uint8_t seconds = totalSec % 60;

  char buf[32];

  if (days > 0) {
    snprintf(
      buf,
      sizeof(buf),
      "%llu d %02u:%02u:%02u",
      days,
      hours,
      minutes,
      seconds);
  } else {
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hours, minutes, seconds);
  }

  return String(buf);
}

String formatCountdown(unsigned long remainingMs) {
  const uint32_t totalSec = remainingMs / MILLIS_PER_SEC;
  const uint8_t minutes = totalSec / 60;
  const uint8_t seconds = totalSec % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u", minutes, seconds);
  return String(buf);
}

// Returns the user-facing countdown/status text for the current state.
String currentCountdownText() {
  const unsigned long now = millis();

  if (state == POWER_OUTAGE_DETECTED) {
    const unsigned long elapsed = now - outageDetectStartMs;
    const unsigned long total = secondsToMs(cfg.minOutageTriggerSec);

    if (elapsed >= total) {
      return "Outage confirmed";
    }

    return "Validating outage: " + formatCountdown(total - elapsed);
  }

  if (state == POWER_FAIL_PENDING) {
    const unsigned long elapsed = now - outageStartMs;
    const unsigned long total = secondsToMs(cfg.shutdownDelaySec);

    if (elapsed >= total) {
      return "Shutdown pending";
    }

    return "Shutdown in " + formatCountdown(total - elapsed);
  }

  if (state == POWER_RETURN_PENDING) {
    const unsigned long elapsed = now - powerReturnStartMs;
    const unsigned long total = secondsToMs(cfg.powerStableDelaySec);

    if (elapsed >= total) {
      return "Startup delay pending";
    }

    return "Power stable check: " + formatCountdown(total - elapsed);
  }

  if (state == WOL_PENDING) {
    const unsigned long elapsed = now - startupDelayStartMs;
    const unsigned long total = secondsToMs(cfg.startupDelaySec);

    if (elapsed >= total) {
      return "WOL sending";
    }

    return "WOL in " + formatCountdown(total - elapsed);
  }

  return "None";
}

String friendlyStateName(SystemState s) {
  switch (s) {
    case POWER_ON_IDLE:
      return "<span style='color:green;font-weight:bold;'>Normal - mains power present</span>";

    case POWER_OUTAGE_DETECTED:
      return "<span style='color:orange;font-weight:bold;'>Power outage detected - validating outage duration</span>";

    case POWER_FAIL_PENDING:
      return "<span style='color:red;font-weight:bold;'>Power outage detected - shutdown countdown running</span>";

    case SHUTDOWN_SENT:
      return "<span style='color:darkred;font-weight:bold;'>Shutdown command sent - waiting for power return</span>";

    case POWER_RETURN_PENDING:
      return "<span style='color:orange;font-weight:bold;'>Power restored - waiting for stable power</span>";

    case WOL_PENDING:
      return "<span style='color:blue;font-weight:bold;'>Power stable - waiting to send WOL</span>";

    default:
      return "<span style='color:gray;'>Unknown state</span>";
  }
}

// Applies the same effective limits as the original code, but keeps
// the policy in one place instead of repeating it in request handlers.
void validateConfigValues() {
  if (cfg.shutdownDelaySec < 60) {
    cfg.shutdownDelaySec = 60;
  }

  if (cfg.powerStableDelaySec < 60) {
    cfg.powerStableDelaySec = 60;
  }

  if (cfg.minOutageTriggerSec < 1) {
    cfg.minOutageTriggerSec = 1;
  }

  if (cfg.minOutageTriggerSec > 60) {
    cfg.minOutageTriggerSec = 60;
  }

  if (cfg.startupDelaySec < 1) {
    cfg.startupDelaySec = 1;
  }

  if (cfg.wolRetries < 1) {
    cfg.wolRetries = 1;
  }

  if (cfg.wolRetrySpacingSec < 1) {
    cfg.wolRetrySpacingSec = 1;
  }
}

// Loads both user settings and long-lived outage statistics from NVS.
// Defaults come from the in-memory cfg struct if a key is missing.
void loadConfig() {
  prefs.begin("config", true);

  cfg.shutdownDelaySec = prefs.getUInt("shutdown", cfg.shutdownDelaySec);
  cfg.minOutageTriggerSec = prefs.getUInt("minTrigger", cfg.minOutageTriggerSec);
  cfg.powerStableDelaySec = prefs.getUInt("stable", cfg.powerStableDelaySec);
  cfg.startupDelaySec = prefs.getUInt("startup", cfg.startupDelaySec);
  cfg.wolRetries = prefs.getUChar("wolRetries", cfg.wolRetries);
  cfg.wolRetrySpacingSec = prefs.getUInt("wolSpacing", cfg.wolRetrySpacingSec);
  cfg.shutdownPort = prefs.getUShort("port", cfg.shutdownPort);

  powerLossTriggerCount = prefs.getUInt("lossCount", 0);
  fullCycleCount = prefs.getUInt("cycleCount", 0);
  totalPowerOutSec = prefs.getULong64("totalOut", 0);
  lastOutageSec = prefs.getUInt("lastOut", 0);
  lastCycleResult = prefs.getString("lastCycle", "None");

  const String host = prefs.getString("host", cfg.shutdownHost);
  const String token = prefs.getString("token", cfg.shutdownToken);
  const String mac = prefs.getString("mac", macToString(cfg.wolMac));

  host.toCharArray(cfg.shutdownHost, sizeof(cfg.shutdownHost));
  token.toCharArray(cfg.shutdownToken, sizeof(cfg.shutdownToken));
  parseMac(mac, cfg.wolMac);

  prefs.end();

  validateConfigValues();
}

// Persists runtime configuration and outage counters so the controller
// can recover its settings and historical stats after reboot.
void saveConfig() {
  validateConfigValues();

  prefs.begin("config", false);

  prefs.putUInt("shutdown", cfg.shutdownDelaySec);
  prefs.putUInt("minTrigger", cfg.minOutageTriggerSec);
  prefs.putUInt("stable", cfg.powerStableDelaySec);
  prefs.putUInt("startup", cfg.startupDelaySec);
  prefs.putUChar("wolRetries", cfg.wolRetries);
  prefs.putUInt("wolSpacing", cfg.wolRetrySpacingSec);
  prefs.putUShort("port", cfg.shutdownPort);
  prefs.putString("host", cfg.shutdownHost);
  prefs.putString("token", cfg.shutdownToken);
  prefs.putString("mac", macToString(cfg.wolMac));

  prefs.putUInt("lossCount", powerLossTriggerCount);
  prefs.putUInt("cycleCount", fullCycleCount);
  prefs.putULong64("totalOut", totalPowerOutSec);
  prefs.putUInt("lastOut", lastOutageSec);
  prefs.putString("lastCycle", lastCycleResult);

  prefs.end();
}

// Sends the shutdown request to the external shutdown-service container.
// If Ethernet is unavailable, the request is skipped and only logged.
void sendShutdownCommand() {
  if (!ethConnected) {
    addLog("Shutdown skipped: Ethernet disconnected");
    return;
  }

  HTTPClient http;
  const String url =
    String("http://") + cfg.shutdownHost + ":" + cfg.shutdownPort + "/shutdown";

  addLog("Sending shutdown request to " + url);

  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + cfg.shutdownToken);

  const int code = http.POST("");

  addLog("Shutdown HTTP response: " + String(code));

  http.end();
  shutdownSent = true;
}

// Builds and sends a standard Wake-on-LAN magic packet:
// 6 bytes of 0xFF followed by the target MAC repeated 16 times.
void sendOneWolPacket() {
  WiFiUDP udp;
  uint8_t packet[102];

  for (int i = 0; i < 6; i++) {
    packet[i] = 0xFF;
  }

  for (int i = 0; i < 16; i++) {
    memcpy(&packet[6 + i * 6], cfg.wolMac, 6);
  }

  udp.beginPacket("255.255.255.255", 9);
  udp.write(packet, sizeof(packet));
  udp.endPacket();

  addLog("WOL packet sent to " + macToString(cfg.wolMac));
}

// Resets the retry bookkeeping for the pending WOL sequence.
// Actual WOL packets are sent later by updateWolRetries().
void startWolSequence() {
  if (!ethConnected) {
    addLog("WOL skipped: Ethernet disconnected");
    return;
  }

  wolRetryCounter = 0;
  lastWolMs = 0;
  wolSent = false;
}

// Sends repeated WOL packets with spacing to improve wake reliability.
// The sequence completes after cfg.wolRetries packets have been sent.
void updateWolRetries() {
  if (state != WOL_PENDING || !ethConnected) {
    return;
  }

  const unsigned long now = millis();
  const unsigned long retrySpacingMs = secondsToMs(cfg.wolRetrySpacingSec);

  if (wolRetryCounter == 0 || now - lastWolMs >= retrySpacingMs) {
    sendOneWolPacket();
    wolRetryCounter++;
    lastWolMs = now;

    if (wolRetryCounter >= cfg.wolRetries) {
      wolSent = true;

      fullCycleCount++;
      lastCycleResult = "Full shutdown/startup cycle";
      saveConfig();

      addLog(LOG_STARTUP, "Startup sequence complete");

      state = POWER_ON_IDLE;
    }
  }
}

// Debounces the raw mains-present input before exposing it as mainsPresent.
// The state machine only consumes the debounced signal, not the raw GPIO.
void updateMainsInput() {
  const bool reading = digitalRead(MAINS_PIN) == HIGH;
  const unsigned long now = millis();

  if (reading != lastRawMainsReading) {
    lastRawMainsReading = reading;
    rawMainsChangedMs = now;
  }

  if (now - rawMainsChangedMs >= MAINS_DEBOUNCE_MS) {
    mainsPresent = reading;
  }
}

// Records outage length in whole seconds.
// Very short measured outages are rounded up to 1 second so they are not
// silently stored as zero-duration events.
void recordOutageDuration(unsigned long now) {
  const unsigned long outageMs = now - outageStartMs;

  lastOutageSec = outageMs / MILLIS_PER_SEC;
  if (outageMs > 0 && lastOutageSec == 0) {
    lastOutageSec = 1;
  }

  totalPowerOutSec += lastOutageSec;

  saveConfig();

  addLog(LOG_INFO, "Outage duration recorded: " + formatDuration(lastOutageSec));
}

// Drives the outage/shutdown/restart lifecycle.
// All waits are millis()-based so the controller remains responsive while
// serving HTTP and resetting the watchdog.
void updateStateMachine() {
  const unsigned long now = millis();

  switch (state) {
    case POWER_ON_IDLE:
      if (!mainsPresent) {
        outageDetectStartMs = now;

        // First loss indication seen; do not start shutdown timing yet.
        // Wait until the outage survives the validation window.
        addLog(LOG_INFO, "Power loss detected; validating outage duration");
        state = POWER_OUTAGE_DETECTED;
      }
      break;

    case POWER_OUTAGE_DETECTED:
      if (mainsPresent) {
        addLog(LOG_INFO, "Short power loss ignored");
        state = POWER_ON_IDLE;
      } else if (now - outageDetectStartMs >= secondsToMs(cfg.minOutageTriggerSec)) {
        outageStartMs = now;
        shutdownSent = false;
        wolSent = false;

        powerLossTriggerCount++;
        saveConfig();

        addLog(LOG_POWER_LOSS, "Power loss confirmed; shutdown timer started");
        state = POWER_FAIL_PENDING;
      }
      break;

    case POWER_FAIL_PENDING:
      if (mainsPresent) {
        recordOutageDuration(now);

        lastCycleResult = "Cancelled outage";
        saveConfig();

        addLog(
          LOG_CANCELLED,
          "Power returned before shutdown delay; shutdown canceled");
        state = POWER_ON_IDLE;
      } else if (now - outageStartMs >= secondsToMs(cfg.shutdownDelaySec)) {
        addLog(LOG_SHUTDOWN, "Shutdown delay expired");
        sendShutdownCommand();
        state = SHUTDOWN_SENT;
      }
      break;

    case SHUTDOWN_SENT:
      if (mainsPresent) {
        recordOutageDuration(now);

        powerReturnStartMs = now;
        addLog(LOG_RETURN, "Power returned; stability timer started");
        state = POWER_RETURN_PENDING;
      }
      break;

    case POWER_RETURN_PENDING:
      if (!mainsPresent) {
        addLog("Power dropped again during stability delay");
        state = SHUTDOWN_SENT;
      } else if (now - powerReturnStartMs >= secondsToMs(cfg.powerStableDelaySec)) {
        startupDelayStartMs = now;
        addLog(LOG_RETURN, "Power stable; startup delay started");
        state = WOL_PENDING;
        startWolSequence();
      }
      break;

    case WOL_PENDING:
      if (!mainsPresent) {
        addLog("Power dropped again before WOL complete");
        state = SHUTDOWN_SENT;
      } else if (now - startupDelayStartMs >= secondsToMs(cfg.startupDelaySec)) {
        updateWolRetries();
      }
      break;
  }
}

// Protects the web UI with HTTP Basic Auth.
bool requireAuth() {
  if (!server.authenticate(UI_USERNAME, UI_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }

  return true;
}

// Shared HTML shell for the minimal built-in UI.
// Styling is inline to keep deployment self-contained on the ESP32.
String htmlHeader(const String &title) {
  String h;
  h += "<!doctype html><html><head>";
  h += "<title>" + title + "</title>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<style>";
  h += "body{font-family:Arial;margin:24px;max-width:900px;}";
  h += "table{border-collapse:collapse;width:100%;margin-bottom:20px;}";
  h += "td,th{padding:7px 10px;border-bottom:1px solid #ccc;text-align:left;}";
  h += "input{padding:6px;width:260px;}";
  h += "button{padding:8px 14px;margin:4px;}";
  h += ".nav a{margin-right:14px;}";
  h += ".ok{color:green;font-weight:bold;}.bad{color:red;font-weight:bold;}";
  h += ".log-info{background:#f5f5f5;}";
  h += ".log-power-loss{background:#fff3cd;}";
  h += ".log-cancelled{background:#fff8e1;}";
  h += ".log-shutdown{background:#f8d7da;}";
  h += ".log-return{background:#d1ecf1;}";
  h += ".log-startup{background:#d4edda;}";
  h += ".log-error{background:#f5c6cb;font-weight:bold;}";
  h += ".badge{font-weight:bold;padding:2px 6px;border-radius:4px;}";
  h += "</style></head><body>";
  h += "<h1>" + String(PROJECT_NAME) + " " + String(FIRMWARE_VERSION) + "</h1>";
  h += "<h2>" + title + "</h2>";
  h += "<div class='nav'>";
  h += "<a href='/'>Status</a>";
  h += "<a href='/config'>Config</a>";
  h += "<a href='/log'>Log</a>";
  h += "</div><hr>";
  return h;
}

String htmlFooter() {
  return "</body></html>";
}

// Renders the live controller status page.
// The page auto-refreshes every 2 seconds to reflect current timers/state.
void handleRoot() {
  if (!requireAuth()) {
    return;
  }

  String html = htmlHeader("ESP32 Status");
  html += "<meta http-equiv='refresh' content='2'>";

  html += "<h2>Status</h2><table>";
  html += "<tr><td>Ethernet</td><td>";
  html += ethConnected ? "<span class='ok'>CONNECTED</span>"
                       : "<span class='bad'>DISCONNECTED</span>";
  html += "</td></tr>";

  html += "<tr><td>IP Address</td><td>" + ETH.localIP().toString() + "</td></tr>";
  html += "<tr><td>Mains</td><td>";
  html += mainsPresent ? "<span class='ok'>ON</span>" : "<span class='bad'>OFF</span>";
  html += "</td></tr>";

  html += "<tr><td>State</td><td>" + friendlyStateName(state) + "</td></tr>";
  html += "<tr><td>Countdown</td><td>" + currentCountdownText() + "</td></tr>";
  html += "<tr><td>Shutdown Sent</td><td>" + String(shutdownSent ? "YES" : "NO") + "</td></tr>";
  html += "<tr><td>WOL Sent</td><td>" + String(wolSent ? "YES" : "NO") + "</td></tr>";
  html += "<tr><td>Last Cycle Result</td><td>" + lastCycleResult + "</td></tr>";
  html += "<tr><td>Power Loss Triggers</td><td>" + String(powerLossTriggerCount) + "</td></tr>";
  html += "<tr><td>Full Shutdown/Startup Cycles</td><td>" + String(fullCycleCount) + "</td></tr>";
  html += "<tr><td>Total Power-Out Time</td><td>" + formatDuration(totalPowerOutSec) + "</td></tr>";
  html += "<tr><td>Last Outage Duration</td><td>" + formatDuration(lastOutageSec) + "</td></tr>";
  html += "<tr><td>Current Time</td><td>" + nowString() + "</td></tr>";
  html += "<tr><td>Firmware Uptime</td><td>" + formatDuration(millis() / MILLIS_PER_SEC) + "</td></tr>";
  html += "<tr><td>Build</td><td>" + String(BUILD_DATE) + " " + String(BUILD_TIME) + "</td></tr>";
  html += "<tr><td>WOL MAC</td><td>" + macToString(cfg.wolMac) + "</td></tr>";
  html += "<tr><td>Shutdown Host</td><td>" + String(cfg.shutdownHost) + ":" + String(cfg.shutdownPort) + "</td></tr>";
  html += "</table>";

  html += "<h2>Timing</h2><table>";
  html += "<tr><td>Shutdown delay</td><td>" + String(cfg.shutdownDelaySec / 60) + " min</td></tr>";
  html += "<tr><td>Power stable delay</td><td>" + String(cfg.powerStableDelaySec / 60) + " min</td></tr>";
  html += "<tr><td>Startup delay</td><td>" + String(cfg.startupDelaySec) + " sec</td></tr>";
  html += "<tr><td>WOL retries</td><td>" + String(cfg.wolRetries) + "</td></tr>";
  html += "<tr><td>WOL retry spacing</td><td>" + String(cfg.wolRetrySpacingSec) + " sec</td></tr>";
  html += "</table>";

  html += "<h2>Counters</h2>";
  html += "<form method='POST' action='/reset-counters' ";
  html += "onsubmit=\"return confirm('Reset power event counters?');\">";
  html += "<button type='submit'>Reset Counters</button>";
  html += "</form>";

  html += "<h2>Manual Controls</h2>";

  html += "<form method='POST' action='/manual-shutdown' ";
  html += "onsubmit=\"return confirm('Send shutdown command now?');\">";
  html += "<button type='submit'>Manual Shutdown</button>";
  html += "</form>";

  html += "<form method='POST' action='/manual-wol' ";
  html += "onsubmit=\"return confirm('Send Wake-on-LAN packet now?');\">";
  html += "<button type='submit'>Manual WOL</button>";
  html += "</form>";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

// Renders the configuration form.
// Values are shown in human-friendly units where useful.
void handleConfig() {
  if (!requireAuth()) {
    return;
  }

  String html = htmlHeader("ESP32 Config");

  html += "<h2>Configuration</h2>";
  html += "<form method='POST' action='/save-config'>";
  html += "<table>";

  html += "<tr><td>Shutdown delay (minutes)</td><td><input name='shutdown' value='" + String(cfg.shutdownDelaySec / 60) + "'></td></tr>";
  html +=
    "<tr><td>Minimum outage trigger (seconds)</td><td><input name='minTrigger' value='" + String(cfg.minOutageTriggerSec) + "'></td></tr>";
  html += "<tr><td>Power stable delay (minutes)</td><td><input name='stable' value='" + String(cfg.powerStableDelaySec / 60) + "'></td></tr>";
  html +=
    "<tr><td>Startup delay before WOL (seconds)</td><td><input name='startup' value='" + String(cfg.startupDelaySec) + "'></td></tr>";
  html += "<tr><td>WOL retries</td><td><input name='wolRetries' value='" + String(cfg.wolRetries) + "'></td></tr>";
  html += "<tr><td>WOL retry spacing (seconds)</td><td><input name='wolSpacing' value='" + String(cfg.wolRetrySpacingSec) + "'></td></tr>";
  html += "<tr><td>Shutdown service IP</td><td><input name='host' value='" + String(cfg.shutdownHost) + "'></td></tr>";
  html += "<tr><td>Shutdown service port</td><td><input name='port' value='" + String(cfg.shutdownPort) + "'></td></tr>";
  html += "<tr><td>Shutdown token</td><td><input type='password' name='token' value='" + String(cfg.shutdownToken) + "'></td></tr>";
  html += "<tr><td>WOL MAC</td><td><input name='mac' value='" + macToString(cfg.wolMac) + "'></td></tr>";

  html += "</table>";
  html += "<button type='submit'>Save Config</button>";
  html += "</form>";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

// Applies form values from the web UI, preserves the original effective
// validation rules, then persists the updated configuration.
void handleSaveConfig() {
  if (!requireAuth()) {
    return;
  }

  cfg.shutdownDelaySec = server.arg("shutdown").toInt() * 60;
  cfg.minOutageTriggerSec = server.arg("minTrigger").toInt();
  cfg.powerStableDelaySec = server.arg("stable").toInt() * 60;
  cfg.startupDelaySec = server.arg("startup").toInt();
  cfg.wolRetries = static_cast<uint8_t>(server.arg("wolRetries").toInt());
  cfg.wolRetrySpacingSec = server.arg("wolSpacing").toInt();
  cfg.shutdownPort = static_cast<uint16_t>(server.arg("port").toInt());

  server.arg("host").toCharArray(cfg.shutdownHost, sizeof(cfg.shutdownHost));
  server.arg("token").toCharArray(cfg.shutdownToken, sizeof(cfg.shutdownToken));

  uint8_t parsedMac[6];
  if (parseMac(server.arg("mac"), parsedMac)) {
    memcpy(cfg.wolMac, parsedMac, 6);
  } else {
    addLog("Invalid MAC entered; old MAC retained");
  }

  validateConfigValues();
  saveConfig();
  addLog("Configuration saved");

  server.sendHeader("Location", "/config");
  server.send(303);
}

String logClass(const String &type) {
  if (type == LOG_POWER_LOSS) {
    return "log-power-loss";
  }
  if (type == LOG_CANCELLED) {
    return "log-cancelled";
  }
  if (type == LOG_SHUTDOWN) {
    return "log-shutdown";
  }
  if (type == LOG_RETURN) {
    return "log-return";
  }
  if (type == LOG_STARTUP) {
    return "log-startup";
  }
  if (type == LOG_ERROR) {
    return "log-error";
  }
  return "log-info";
}

// Renders the in-memory event log from newest to oldest.
void handleLog() {
  if (!requireAuth()) {
    return;
  }

  String html = htmlHeader("ESP32 Event Log");
  html += "<h2>Event Log</h2>";
  html += "<form method='POST' action='/clear-log' ";
  html += "onsubmit=\"return confirm('Clear the event log?');\">";
  html += "<button type='submit'>Clear Log</button>";
  html += "</form>";
  html += "<table>";
  html += "<tr><th>Time</th><th>Type</th><th>Event</th></tr>";

  for (int i = 0; i < logCount; i++) {
    const int idx = (logIndex - 1 - i + LOG_SIZE) % LOG_SIZE;
    const String cls = logClass(eventLog[idx].type);

    html += "<tr class='" + cls + "'>";
    html += "<td>" + eventLog[idx].time + "</td>";
    html += "<td><span class='badge'>" + eventLog[idx].type + "</span></td>";
    html += "<td>" + eventLog[idx].message + "</td>";
    html += "</tr>";
  }

  html += "</table>";
  html += htmlFooter();

  server.send(200, "text/html", html);
}

// Manual override for testing the shutdown path without waiting for a real outage.
void handleManualShutdown() {
  if (!requireAuth()) {
    return;
  }

  addLog("Manual shutdown requested from web UI");
  sendShutdownCommand();

  server.sendHeader("Location", "/");
  server.send(303);
}

// Manual override for testing Wake-on-LAN independently of the state machine.
void handleManualWol() {
  if (!requireAuth()) {
    return;
  }

  addLog("Manual WOL requested from web UI");
  sendOneWolPacket();
  wolSent = true;

  server.sendHeader("Location", "/");
  server.send(303);
}

// Clears only the in-memory display log; persistent counters remain unchanged.
void handleClearLog() {
  if (!requireAuth()) {
    return;
  }

  for (int i = 0; i < LOG_SIZE; i++) {
    eventLog[i].type = "";
    eventLog[i].time = "";
    eventLog[i].message = "";
  }

  logIndex = 0;
  logCount = 0;

  addLog(LOG_INFO, "Event log cleared from web UI");

  server.sendHeader("Location", "/log");
  server.send(303);
}

// Resets persisted outage counters/statistics without changing configuration.
void handleResetCounters() {
  if (!requireAuth()) {
    return;
  }

  powerLossTriggerCount = 0;
  fullCycleCount = 0;
  totalPowerOutSec = 0;
  lastOutageSec = 0;
  lastCycleResult = "None";

  saveConfig();

  addLog(LOG_INFO, "Counters reset from web UI");

  server.sendHeader("Location", "/");
  server.send(303);
}

// Registers all HTTP routes and starts the embedded server.
// This is called after Ethernet has a valid IP address.
void startWebServer() {
  if (webServerStarted) {
    return;
  }

  server.on("/", handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save-config", HTTP_POST, handleSaveConfig);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/manual-shutdown", HTTP_POST, handleManualShutdown);
  server.on("/manual-wol", HTTP_POST, handleManualWol);
  server.on("/reset-counters", HTTP_POST, handleResetCounters);
  server.on("/clear-log", HTTP_POST, handleClearLog);
  server.begin();

  webServerStarted = true;
  addLog("Web server started");
}

// Attempts to sync wall-clock time over NTP so logs show real timestamps.
// If time sync fails, the logger falls back to uptime-based timestamps.
void syncTime() {
  configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
}

void checkTimeSyncStatus() {
  if (timeSynced) return;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    timeSynced = true;
    addLog("INFO", "NTP time synchronized");
  }
}

// Centralized network event handler for Ethernet lifecycle events.
// The web server starts only after a usable IP address is assigned.
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH started");
      ETH.setHostname("esp32-server-controller");
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH link connected");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      ethConnected = true;
      Serial.print("ETH got IP: ");
      Serial.println(ETH.localIP());
      addLog("Ethernet connected, IP " + ETH.localIP().toString());

      syncTime();

      startWebServer();
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ethConnected = false;
      addLog("Ethernet disconnected");
      break;

    case ARDUINO_EVENT_ETH_STOP:
      ethConnected = false;
      addLog("Ethernet stopped");
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // Give Serial a moment to come up so early boot logs are visible.
  delay(1000);

  Serial.println();
  Serial.println("ESP32 Server Startup/Shutdown Controller");

  loadConfig();
  addLog("Controller booted");

  // Input-only pin used for the mains-present sense signal.
  pinMode(MAINS_PIN, INPUT);

  // Seed the debounced input state from the current reading so startup
  // begins with a stable baseline instead of forcing an immediate transition.
  const bool initialMainsReading = digitalRead(MAINS_PIN) == HIGH;
  lastRawMainsReading = initialMainsReading;
  mainsPresent = initialMainsReading;
  rawMainsChangedMs = millis();

  if (mainsPresent) {
    addLog("Initial mains state: ON");
    state = POWER_ON_IDLE;
  } else {
    // Booting while mains is already absent is treated as an active outage.
    // This preserves the original behavior.
    addLog("Initial mains state: OFF");
    outageStartMs = millis();
    state = POWER_FAIL_PENDING;
  }

  Network.onEvent(onEvent);

  if (!ETH.begin()) {
    addLog("ETH.begin failed");
  } else {
    addLog("ETH.begin accepted");
  }

  // Enable the task watchdog so unexpected stalls trigger recovery.
  // The main loop must continue calling esp_task_wdt_reset().
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  addLog("Watchdog timer initialized");
}

void loop() {
  updateMainsInput();
  updateStateMachine();

  if (ethConnected) {
    server.handleClient();
    checkTimeSyncStatus();
  }

  esp_task_wdt_reset();

  // Short pacing delay keeps CPU usage reasonable while preserving
  // responsive outage handling and UI servicing.
  delay(50);
}
