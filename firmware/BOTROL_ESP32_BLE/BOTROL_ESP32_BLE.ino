#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HijelHID_BLEKeyboard.h>

// ============================================================
// BOTROL ESP32 + BLE HID bridge
// Cloud: BOTROL API over HTTPS
// Computer link: BLE HID keyboard
// Supported commands: ping, ble_open_notepad, ble_open_calculator
// ============================================================

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* BOTROL_SERVER = "https://botrol-cloud.onrender.com";
const char* DEVICE_ID = "BOTROL-ESP32-001";
const char* DEVICE_TOKEN = "YOUR_BOTROL_DEVICE_TOKEN";
const char* FIRMWARE_VERSION = "0.2.0";

const unsigned long POLL_INTERVAL_MS = 5000;
unsigned long lastPollAt = 0;

WiFiClientSecure secureClient;
HijelHID_BLEKeyboard keyboard("BOTROL");

bool commandInProgress = false;

// ------------------------------------------------------------
// HTTPS helper
// ------------------------------------------------------------
bool httpsRequest(
  const String& method,
  const String& path,
  const String& body,
  String& response
) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected. Request skipped.");
    return false;
  }

  HTTPClient http;
  secureClient.setInsecure(); // Prototype only. Replace with CA validation for production.

  String url = String(BOTROL_SERVER) + path;
  if (!http.begin(secureClient, url)) {
    Serial.println("[HTTP] Could not start request.");
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  http.addHeader("Content-Type", "application/json");

  int status = -1;
  if (method == "GET") {
    status = http.GET();
  } else {
    status = http.POST(body);
  }

  response = status > 0 ? http.getString() : "";
  Serial.printf("[HTTP] Status: %d\n", status);
  if (status <= 0) {
    Serial.printf("[HTTP] Error: %s\n", http.errorToString(status).c_str());
  }

  http.end();
  return status >= 200 && status < 300;
}

// ------------------------------------------------------------
// Register ESP32 with BOTROL Cloud
// ------------------------------------------------------------
bool registerDevice() {
  Serial.println("[BOTROL] Registering device...");

  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["firmware_version"] = FIRMWARE_VERSION;

  String body;
  serializeJson(doc, body);

  String response;
  bool ok = httpsRequest(
    "POST",
    "/api/v1/devices/register",
    body,
    response
  );

  if (ok) {
    Serial.println("[BOTROL] Device registered!");
    Serial.println(response);
  } else {
    Serial.println("[BOTROL] Registration failed.");
  }

  return ok;
}

// ------------------------------------------------------------
// Report command result to cloud
// ------------------------------------------------------------
void reportResult(
  const String& commandId,
  const char* status,
  const String& message
) {
  StaticJsonDocument<256> doc;
  doc["status"] = status;
  doc["message"] = message;

  String body;
  serializeJson(doc, body);

  String path = String("/api/v1/devices/") + DEVICE_ID +
                "/commands/" + commandId + "/result";

  String response;
  bool ok = httpsRequest("POST", path, body, response);

  if (ok) {
    Serial.println("[BOTROL] Result reported:");
    Serial.println(response);
  } else {
    Serial.println("[BOTROL] Result report failed.");
  }
}

// ------------------------------------------------------------
// BLE keyboard actions
// These shortcuts are currently Windows-specific.
// ------------------------------------------------------------
bool openWindowsRunDialog() {
  if (!keyboard.isPaired()) {
    Serial.println("[BLE] BOTROL is not paired with a computer.");
    return false;
  }

  Serial.println("[BLE] Sending Windows + R...");
  delay(500);
  keyboard.tap(KEY_R, KEY_MOD_LGUI);
  delay(1200);
  return true;
}

bool executeBleCommand(const String& command, String& resultMessage) {
  if (command == "ble_open_notepad") {
    if (!openWindowsRunDialog()) {
      resultMessage = "BLE keyboard is not paired. Pair BOTROL with Windows first.";
      return false;
    }

    keyboard.print("notepad");
    delay(300);
    keyboard.tap(KEY_RETURN);
    resultMessage = "Sent Win+R and launched Notepad.";
    return true;
  }

  if (command == "ble_open_calculator") {
    if (!openWindowsRunDialog()) {
      resultMessage = "BLE keyboard is not paired. Pair BOTROL with Windows first.";
      return false;
    }

    keyboard.print("calc");
    delay(300);
    keyboard.tap(KEY_RETURN);
    resultMessage = "Sent Win+R and launched Calculator.";
    return true;
  }

  resultMessage = "Unsupported command.";
  return false;
}

// ------------------------------------------------------------
// Process one cloud command
// ------------------------------------------------------------
void processCommand(JsonObject command) {
  String commandId = command["command_id"] | "";
  String commandName = command["command"] | "";

  Serial.println();
  Serial.println("========== NEW COMMAND ==========");
  Serial.print("Command ID: ");
  Serial.println(commandId);
  Serial.print("Command: ");
  Serial.println(commandName);

  if (commandId.length() == 0) {
    Serial.println("[BOTROL] Invalid command: missing command ID.");
    return;
  }

  if (commandName == "ping") {
    Serial.println("[BOTROL] PING received!");
    Serial.println("[BOTROL] PONG!");
    reportResult(commandId, "completed", "pong");
  } else if (
    commandName == "ble_open_notepad" ||
    commandName == "ble_open_calculator"
  ) {
    String message;
    bool success = executeBleCommand(commandName, message);

    if (success) {
      Serial.println("[BOTROL] " + message);
      reportResult(commandId, "completed", message);
    } else {
      Serial.println("[BOTROL] " + message);
      reportResult(commandId, "failed", message);
    }
  } else {
    Serial.println("[BOTROL] Command is not supported by this firmware.");
    reportResult(commandId, "failed", "Unsupported command.");
  }

  Serial.println("=================================");
}

// ------------------------------------------------------------
// Poll cloud for commands
// ------------------------------------------------------------
void pollCommands() {
  if (commandInProgress || WiFi.status() != WL_CONNECTED) {
    return;
  }

  commandInProgress = true;

  String path = String("/api/v1/devices/") + DEVICE_ID +
                "/commands/next";

  String response;
  bool ok = httpsRequest("GET", path, "", response);

  if (ok) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
      Serial.print("[BOTROL] JSON parse error: ");
      Serial.println(error.c_str());
    } else if (!doc["command"].isNull()) {
      JsonObject command = doc["command"].as<JsonObject>();
      processCommand(command);
    }
  }

  commandInProgress = false;
}

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 25000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected!");
    Serial.print("[WiFi] IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Connection timed out.");
  }
}

// ------------------------------------------------------------
// Arduino setup / loop
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("       BOTROL ESP32 BLE BRIDGE");
  Serial.println("======================================");

  // Start BLE HID keyboard. Pair the computer with device name BOTROL.
  keyboard.begin();
  Serial.println("[BLE] Keyboard service started. Pair with BOTROL.");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    registerDevice();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      registerDevice();
    }
  }

  if (millis() - lastPollAt >= POLL_INTERVAL_MS) {
    lastPollAt = millis();
    pollCommands();
  }

  delay(20);
}
