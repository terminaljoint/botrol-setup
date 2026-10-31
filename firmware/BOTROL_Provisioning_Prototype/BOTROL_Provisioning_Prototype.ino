#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HijelHID_BLEKeyboard.h>

// BOTROL experimental cloud-connected provisioning firmware.
// HTTPS certificate validation is intentionally marked for hardening below.
static const char* SETUP_URL = "https://botrol-setup.vercel.app/provision.html";
static const char* DEVICE_NAME = "BOTROL";
static const char* BOTROL_SERVER = "https://botrol-cloud.onrender.com";
static const char* DEVICE_ID = "BOTROL-ESP32-001";
static const char* FIRMWARE_VERSION = "0.4.0";
static const uint32_t SERIAL_BAUD = 115200;
static const uint32_t WIFI_TIMEOUT_MS = 25000;
static const uint32_t REGISTER_RETRY_MS = 30000;
static const uint32_t POLL_INTERVAL_MS = 5000;

HijelHID_BLEKeyboard keyboard(DEVICE_NAME);
Preferences prefs;
WiFiClientSecure secureClient;

String activeSsid, activePassword, candidateSsid, candidatePassword;
String restoreSsid, restorePassword, deviceToken;
String serialLine;

enum WifiAttempt { WIFI_IDLE, WIFI_CANDIDATE, WIFI_RESTORE };
WifiAttempt wifiAttempt = WIFI_IDLE;
uint32_t wifiAttemptAt = 0;
uint32_t lastRegisterAt = 0;
uint32_t lastPollAt = 0;
bool setupPageOpened = false;
bool cloudRegistered = false;
bool commandInProgress = false;

void publishStatus(const char* state, const char* detail = nullptr) {
  StaticJsonDocument<384> doc;
  doc["type"] = "status";
  doc["state"] = state;
  if (detail) doc["detail"] = detail;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["cloud"] = cloudRegistered ? "registered" : "not_registered";
  if (WiFi.status() == WL_CONNECTED) doc["ssid"] = WiFi.SSID();
  serializeJson(doc, Serial);
  Serial.println();
}

bool cloudCredentialsReady() {
  return deviceToken.length() >= 16 && deviceToken != "YOUR_BOTROL_DEVICE_TOKEN";
}

bool httpsRequest(const char* method, const String& path, const String& body, String& response) {
  if (WiFi.status() != WL_CONNECTED || !cloudCredentialsReady()) return false;

  HTTPClient http;
  // DEVELOPMENT ONLY: replace setInsecure() with trusted CA validation before production.
  secureClient.setInsecure();
  const String url = String(BOTROL_SERVER) + path;
  if (!http.begin(secureClient, url)) return false;

  http.setTimeout(10000);
  http.addHeader("Authorization", "Bearer " + deviceToken);
  http.addHeader("Content-Type", "application/json");

  const int status = strcmp(method, "GET") == 0 ? http.GET() : http.POST(body);
  response = status > 0 ? http.getString() : "";
  http.end();
  return status >= 200 && status < 300;
}

bool registerDevice() {
  if (!cloudCredentialsReady() || WiFi.status() != WL_CONNECTED) return false;

  StaticJsonDocument<160> doc;
  doc["device_id"] = DEVICE_ID;
  doc["firmware_version"] = FIRMWARE_VERSION;
  String body, response;
  serializeJson(doc, body);

  lastRegisterAt = millis();
  const bool ok = httpsRequest("POST", "/api/v1/devices/register", body, response);
  cloudRegistered = ok;
  if (ok) {
    Serial.println("[BOTROL] Cloud registration successful");
    publishStatus("cloud_registered", "ESP32 registered with BOTROL Cloud");
  } else {
    Serial.println("[BOTROL] Cloud registration failed; will retry");
    publishStatus("cloud_registration_failed", "Check device token and BOTROL Cloud availability");
  }
  return ok;
}

void reportResult(const String& commandId, const char* status, const String& message) {
  StaticJsonDocument<256> doc;
  doc["status"] = status;
  doc["message"] = message;
  String body, response;
  serializeJson(doc, body);
  const String path = String("/api/v1/devices/") + DEVICE_ID +
                      "/commands/" + commandId + "/result";
  if (!httpsRequest("POST", path, body, response)) {
    Serial.println("[BOTROL] Could not report command result");
  }
}

bool openWindowsRunDialog() {
  if (!keyboard.isPaired()) return false;
  keyboard.tap(KEY_R, KEY_MOD_LGUI);
  delay(900);
  return true;
}

void processCommand(JsonObject command) {
  const String commandId = command["command_id"] | "";
  const String name = command["command"] | "";
  if (!commandId.length()) return;

  String result;
  bool success = false;

  if (name == "ping") {
    result = "pong";
    success = true;
  } else if (name == "ble_open_notepad" || name == "ble_open_calculator") {
    if (!openWindowsRunDialog()) {
      result = "BLE keyboard is not paired with the computer";
    } else {
      keyboard.print(name == "ble_open_notepad" ? "notepad" : "calc");
      delay(250);
      keyboard.tap(KEY_RETURN);
      result = name == "ble_open_notepad" ? "Sent shortcut to open Notepad" : "Sent shortcut to open Calculator";
      success = true;
    }
  } else {
    result = "Unsupported command by this firmware";
  }

  reportResult(commandId, success ? "completed" : "failed", result);
  publishStatus(success ? "command_completed" : "command_failed", result.c_str());
}

void pollCloud() {
  if (!cloudRegistered || commandInProgress || WiFi.status() != WL_CONNECTED) return;
  commandInProgress = true;

  String response;
  const String path = String("/api/v1/devices/") + DEVICE_ID + "/commands/next";
  if (httpsRequest("GET", path, "", response)) {
    StaticJsonDocument<1536> doc;
    const DeserializationError err = deserializeJson(doc, response);
    if (!err && !doc["command"].isNull()) {
      processCommand(doc["command"].as<JsonObject>());
    }
  } else {
    cloudRegistered = false;
    publishStatus("cloud_disconnected", "Cloud polling failed; registration will be retried");
  }
  commandInProgress = false;
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "network_not_found";
    case WL_CONNECT_FAILED: return "authentication_or_connection_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_IDLE_STATUS: return "idle";
    default: return "unknown";
  }
}

void beginWifiAttempt(const String& ssid, const String& password, WifiAttempt kind) {
  WiFi.disconnect(false, false);
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiAttempt = kind;
  wifiAttemptAt = millis();
}

void handleSerialCommand(const String& line) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line)) {
    publishStatus("error", "Invalid JSON command");
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "get_status") == 0) {
    publishStatus("device_status", "Current ESP32 connection status");
    return;
  }

  if (strcmp(type, "wifi_config") == 0) {
    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";
    if (!strlen(ssid) || strlen(ssid) > 32 || strlen(password) > 63) {
      publishStatus("error", "SSID or password length is invalid");
      return;
    }
    candidateSsid = ssid;
    candidatePassword = password;
    restoreSsid = activeSsid;
    restorePassword = activePassword;
    publishStatus("credentials_received", "Wi-Fi credentials received over USB");
    beginWifiAttempt(candidateSsid, candidatePassword, WIFI_CANDIDATE);
    publishStatus("connecting", "Connecting to the selected Wi-Fi network");
    return;
  }

  if (strcmp(type, "cloud_config") == 0) {
    const char* token = doc["device_token"] | "";
    if (strlen(token) < 16 || strlen(token) > 256) {
      publishStatus("error", "Device token length is invalid");
      return;
    }
    deviceToken = token;
    prefs.putString("device_token", deviceToken);
    cloudRegistered = false;
    publishStatus("cloud_token_saved", "Device token saved locally; cloud registration starting");
    if (WiFi.status() == WL_CONNECTED) registerDevice();
    return;
  }

  publishStatus("error", "Unsupported USB command");
}

void readSerialCommands() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialLine.length()) handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 768) {
      serialLine += c;
    } else {
      serialLine = "";
      publishStatus("error", "USB command exceeded maximum length");
    }
  }
}

void handleWifiState() {
  if (wifiAttempt == WIFI_IDLE) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiAttempt == WIFI_CANDIDATE) {
      prefs.putString("ssid", candidateSsid);
      prefs.putString("password", candidatePassword);
      activeSsid = candidateSsid;
      activePassword = candidatePassword;
      publishStatus("wifi_connected", "Wi-Fi connected; credentials saved locally");
    } else {
      publishStatus("wifi_failed_restored", "Previous Wi-Fi connection restored");
    }
    wifiAttempt = WIFI_IDLE;
    cloudRegistered = false;
    if (cloudCredentialsReady()) registerDevice();
    return;
  }

  if (millis() - wifiAttemptAt < WIFI_TIMEOUT_MS) return;
  if (wifiAttempt == WIFI_CANDIDATE && restoreSsid.length()) {
    publishStatus("restoring", "New Wi-Fi failed; restoring previous network");
    beginWifiAttempt(restoreSsid, restorePassword, WIFI_RESTORE);
  } else {
    const String reason = String("Wi-Fi failed: ") + wifiStatusName(WiFi.status()) +
                          " (status " + String((int)WiFi.status()) +
                          "). ESP32 requires 2.4 GHz Wi-Fi; check SSID/password and WPA2 compatibility.";
    WiFi.disconnect(false, false);
    wifiAttempt = WIFI_IDLE;
    publishStatus("wifi_failed", reason.c_str());
  }
}

void openSetupPage() {
  if (setupPageOpened || !keyboard.isPaired()) return;
  setupPageOpened = true;
  delay(2500);
  keyboard.tap(KEY_R, KEY_MOD_LGUI);
  delay(900);
  keyboard.print(SETUP_URL);
  delay(250);
  keyboard.tap(KEY_RETURN);
  publishStatus("setup_page_opened", "Configure Wi-Fi and cloud token on the setup page");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(700);
  prefs.begin("botrol", false);
  activeSsid = prefs.getString("ssid", "");
  activePassword = prefs.getString("password", "");
  deviceToken = prefs.getString("device_token", "");

  keyboard.begin();
  publishStatus("usb_ready", "BOTROL USB provisioning and cloud client ready");

  if (activeSsid.length()) {
    publishStatus("wifi_reconnecting", "Using saved Wi-Fi configuration");
    beginWifiAttempt(activeSsid, activePassword, WIFI_CANDIDATE);
  } else {
    publishStatus("setup_required", "Configure Wi-Fi over USB");
  }
  if (!cloudCredentialsReady()) {
    publishStatus("cloud_setup_required", "Add the BOTROL device token in setup page");
  }
}

void loop() {
  readSerialCommands();
  handleWifiState();
  openSetupPage();

  if (WiFi.status() == WL_CONNECTED && cloudCredentialsReady() && !cloudRegistered &&
      millis() - lastRegisterAt >= REGISTER_RETRY_MS) {
    registerDevice();
  }

  if (millis() - lastPollAt >= POLL_INTERVAL_MS) {
    lastPollAt = millis();
    pollCloud();
  }
  delay(10);
}
