#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HijelHID_BLEKeyboard.h>

static const char* SETUP_URL = "https://botrol-setup.vercel.app/provision.html";
static const char* DEVICE_NAME = "BOTROL";
static const uint32_t SERIAL_BAUD = 115200;

HijelHID_BLEKeyboard keyboard(DEVICE_NAME);
Preferences prefs;

String activeSsid, activePassword, candidateSsid, candidatePassword;
String restoreSsid, restorePassword;

enum WifiAttempt { WIFI_IDLE, WIFI_CANDIDATE, WIFI_RESTORE };
WifiAttempt wifiAttempt = WIFI_IDLE;
uint32_t wifiAttemptAt = 0;
bool setupPageOpened = false;

String serialLine;

void publishStatus(const char* state, const char* detail = nullptr) {
  StaticJsonDocument<256> doc;
  doc["type"] = "status";
  doc["state"] = state;
  if (detail) doc["detail"] = detail;
  if (WiFi.status() == WL_CONNECTED) {
    doc["wifi"] = "connected";
    doc["ssid"] = WiFi.SSID();
  } else {
    doc["wifi"] = "disconnected";
  }
  serializeJson(doc, Serial);
  Serial.println();
}

void beginWifiAttempt(const String& ssid, const String& password, WifiAttempt kind) {
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiAttempt = kind;
  wifiAttemptAt = millis();
}

void handleSerialCommand(const String& line) {
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, line)) {
    publishStatus("error", "Invalid JSON command");
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "wifi_config") != 0) {
    publishStatus("error", "Unsupported command");
    return;
  }

  const char* ssid = doc["ssid"] | "";
  const char* password = doc["password"] | "";
  const size_t ssidLen = strlen(ssid);
  const size_t passwordLen = strlen(password);

  if (!ssidLen || ssidLen > 32 || passwordLen > 63) {
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
}

void readSerialCommands() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialLine.length()) handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 512) {
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
      publishStatus("wifi_connected", "Wi-Fi connected; credentials saved on ESP32");
    } else {
      publishStatus("wifi_failed_restored", "Previous Wi-Fi connection restored");
    }
    wifiAttempt = WIFI_IDLE;
    return;
  }

  if (millis() - wifiAttemptAt < 20000) return;

  if (wifiAttempt == WIFI_CANDIDATE && restoreSsid.length()) {
    publishStatus("restoring", "New Wi-Fi failed; restoring previous network");
    beginWifiAttempt(restoreSsid, restorePassword, WIFI_RESTORE);
  } else {
    WiFi.disconnect(false, false);
    wifiAttempt = WIFI_IDLE;
    publishStatus("wifi_failed", "Connection failed; check credentials and retry");
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
  publishStatus("setup_page_opened", "Open the page and connect using its USB serial button");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(700);

  prefs.begin("botrol", false);
  activeSsid = prefs.getString("ssid", "");
  activePassword = prefs.getString("password", "");

  keyboard.begin();

  publishStatus("usb_ready", "BOTROL USB provisioning protocol ready");
  if (activeSsid.length()) {
    candidateSsid = activeSsid;
    candidatePassword = activePassword;
    publishStatus("wifi_reconnecting", "Using saved Wi-Fi configuration");
    beginWifiAttempt(activeSsid, activePassword, WIFI_CANDIDATE);
  } else {
    publishStatus("setup_required", "Pair BOTROL keyboard; then configure Wi-Fi over USB");
  }
}

void loop() {
  readSerialCommands();
  handleWifiState();
  openSetupPage();
  delay(10);
}
