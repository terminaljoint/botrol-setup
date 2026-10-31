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
static const uint32_t WIFI_TIMEOUT_MS = 20000;
static const uint32_t REGISTER_RETRY_MS = 5000;
static const uint32_t POLL_INTERVAL_MS = 1000;

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
uint8_t consecutivePollFailures = 0;
int lastCloudHttpStatus = 0;
bool setupPageOpened = false;
bool cloudRegistered = false;
bool commandInProgress = false;
bool wasWifiConnected = false;
volatile uint16_t lastWifiDisconnectReason = 0;
uint32_t lastWifiEventAt = 0;

void publishStatus(const char* state, const char* detail = nullptr) {
  StaticJsonDocument<384> doc;
  doc["type"] = "status";
  doc["state"] = state;
  if (detail) doc["detail"] = detail;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["cloud"] = cloudRegistered ? "registered" : "not_registered";
  doc["wifi_status_code"] = (int)WiFi.status();
  doc["wifi_disconnect_reason"] = (int)lastWifiDisconnectReason;
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
  if (!http.begin(secureClient, url)) { lastCloudHttpStatus = -1000; return false; }

  http.setTimeout(10000);
  http.addHeader("Authorization", "Bearer " + deviceToken);
  http.addHeader("Content-Type", "application/json");

  const int status = strcmp(method, "GET") == 0 ? http.GET() : http.POST(body);
  lastCloudHttpStatus = status;
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
    Serial.println("[BOTROL] Cloud registration successful (HTTP " + String(lastCloudHttpStatus) + ")");
    consecutivePollFailures = 0;
    publishStatus("cloud_registered", "ESP32 registered with BOTROL Cloud");
  } else {
    Serial.println("[BOTROL] Cloud registration failed (HTTP " + String(lastCloudHttpStatus) + "); will retry");
    publishStatus("cloud_registration_failed", (String("HTTP status: ") + String(lastCloudHttpStatus) + ". 401 = token mismatch; 503 = server token missing; negative = connection/TLS error.").c_str());
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
  bool reported = false;
  for (int attempt = 1; attempt <= 3 && !reported; attempt++) {
    reported = httpsRequest("POST", path, body, response);
    if (!reported) {
      Serial.println("[BOTROL] Result report failed (HTTP " + String(lastCloudHttpStatus) + "), attempt " + String(attempt) + "/3");
      if (attempt < 3) delay(500 * attempt);
    }
  }
  if (!reported) publishStatus("command_result_report_failed", "Command ran, but cloud did not confirm its result. It may remain pending.");
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
    consecutivePollFailures++;
    publishStatus("cloud_poll_failed", (String("Cloud poll HTTP status: ") + String(lastCloudHttpStatus) +
      ". Consecutive failures: " + String(consecutivePollFailures) + "/3.").c_str());
    // Do not drop registration because of one temporary Render/network hiccup.
    if (consecutivePollFailures >= 5) {
      // A poll timeout is not proof that registration was lost. Keep polling:
      // the API can recreate volatile device presence after a Render restart.
      consecutivePollFailures = 0;
      publishStatus("cloud_unstable", "Cloud is temporarily unreachable; continuing automatic retries.");
    }
  }
  if (cloudRegistered) consecutivePollFailures = 0;
  commandInProgress = false;
}

void beginWifiAttempt(const String& ssid, const String& password, WifiAttempt kind) {
  // Start from a clean station state without erasing saved BOTROL credentials.
  wifiAttempt = WIFI_IDLE;
  WiFi.disconnect(false, false);
  delay(250);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  lastWifiDisconnectReason = 0;
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiAttempt = kind;
  wifiAttemptAt = millis();
}

void scanWifiNetworks() {
  publishStatus("wifi_scan_started", "Scanning nearby 2.4 GHz Wi-Fi networks");
  WiFi.mode(WIFI_STA);
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    publishStatus("wifi_scan_failed", "Wi-Fi scan failed; retry the scan");
    return;
  }

  for (int i = 0; i < count; i++) {
    StaticJsonDocument<256> doc;
    doc["type"] = "wifi_network";
    doc["ssid"] = WiFi.SSID(i);
    doc["rssi"] = WiFi.RSSI(i);
    doc["channel"] = WiFi.channel(i);
    doc["encryption"] = (int)WiFi.encryptionType(i);
    serializeJson(doc, Serial);
    Serial.println();
  }
  WiFi.scanDelete();
  StaticJsonDocument<128> done;
  done["type"] = "wifi_scan_complete";
  done["count"] = count;
  serializeJson(done, Serial);
  Serial.println();
}

void handleSerialCommand(const String& line) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line)) {
    publishStatus("error", "Invalid JSON command");
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "wifi_scan") == 0) {
    scanWifiNetworks();
    return;
  }

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
    wasWifiConnected = true;
    cloudRegistered = false;
    if (cloudCredentialsReady()) registerDevice();
    return;
  }

  if (millis() - wifiAttemptAt < WIFI_TIMEOUT_MS) return;
  if (wifiAttempt == WIFI_CANDIDATE && restoreSsid.length()) {
    publishStatus("restoring", "New Wi-Fi failed; restoring previous network");
    beginWifiAttempt(restoreSsid, restorePassword, WIFI_RESTORE);
  } else {
    WiFi.disconnect(false, false);
    wifiAttempt = WIFI_IDLE;
    StaticJsonDocument<256> failure;
    failure["type"] = "status";
    failure["state"] = "wifi_failed";
    failure["detail"] = "Connection timed out. Check SSID, password, 2.4 GHz availability, and router security.";
    failure["wifi_status_code"] = (int)WiFi.status();
    failure["wifi_disconnect_reason"] = (int)lastWifiDisconnectReason;
    failure["wifi"] = "disconnected";
    failure["cloud"] = cloudRegistered ? "registered" : "not_registered";
    serializeJson(failure, Serial);
    Serial.println();
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

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    // Event ID 5 is STA_DISCONNECTED in Arduino-ESP32 2.x and 3.x.
    if ((int)event == 5) {
      lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
      lastWifiEventAt = millis();
    }
  });
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

  const bool wifiNowConnected = WiFi.status() == WL_CONNECTED;
  if (wifiNowConnected && !wasWifiConnected) {
    wasWifiConnected = true;
    cloudRegistered = false;
    publishStatus("wifi_reconnected", "Wi-Fi restored; reconnecting to BOTROL Cloud");
    if (cloudCredentialsReady()) registerDevice();
  } else if (!wifiNowConnected && wasWifiConnected) {
    wasWifiConnected = false;
    cloudRegistered = false;
    publishStatus("wifi_lost", "Wi-Fi connection lost; waiting for automatic reconnect");
  }

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
