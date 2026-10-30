#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <HijelHID_BLEKeyboard.h>

static const char* SETUP_URL = "https://botrol-setup.vercel.app/provision.html";
static const char* DEVICE_NAME = "BOTROL";
static const char* SERVICE_UUID = "7b100001-6d2a-4f8b-9c31-2d5a7e110001";
static const char* CONFIG_UUID  = "7b100002-6d2a-4f8b-9c31-2d5a7e110001";
static const char* STATUS_UUID  = "7b100003-6d2a-4f8b-9c31-2d5a7e110001";
static const char* INFO_UUID    = "7b100004-6d2a-4f8b-9c31-2d5a7e110001";

HijelHID_BLEKeyboard keyboard(DEVICE_NAME);
Preferences prefs;
NimBLECharacteristic* statusChar = nullptr;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;

char pendingSsid[33] = {0};
char pendingPassword[64] = {0};
volatile bool configPending = false;

String activeSsid;
String activePassword;
String candidateSsid;
String candidatePassword;
String restoreSsid;
String restorePassword;

enum WifiAttempt { WIFI_IDLE, WIFI_CANDIDATE, WIFI_RESTORE };
WifiAttempt wifiAttempt = WIFI_IDLE;
uint32_t wifiAttemptAt = 0;
bool setupPageOpened = false;

void publishStatus(const String& state, const String& detail = "") {
  StaticJsonDocument<192> doc;
  doc["state"] = state;
  if (detail.length()) doc["detail"] = detail;
  String value;
  serializeJson(doc, value);
  if (statusChar) {
    statusChar->setValue(state);
    statusChar->notify();
  }
  Serial.println("[BOTROL] " + value);
}

class ConfigCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      publishStatus("rejected", "Encrypted BLE connection required");
      return;
    }

    std::string raw = chr->getValue();
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) {
      publishStatus("rejected", "Invalid configuration format");
      return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";
    size_t ssidLen = strlen(ssid);
    size_t passwordLen = strlen(password);

    if (!ssidLen || ssidLen > 32 || passwordLen > 63) {
      publishStatus("rejected", "SSID or password length is invalid");
      return;
    }

    portENTER_CRITICAL(&configMux);
    strlcpy(pendingSsid, ssid, sizeof(pendingSsid));
    strlcpy(pendingPassword, password, sizeof(pendingPassword));
    configPending = true;
    portEXIT_CRITICAL(&configMux);
    publishStatus("credentials_received", "Connecting to the selected Wi-Fi network");
  }
};

void startProvisioningService() {
  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server) {
    Serial.println("[BLE] HID server unavailable");
    return;
  }

  NimBLEService* service = server->createService(SERVICE_UUID);
  NimBLECharacteristic* configChar = service->createCharacteristic(
    CONFIG_UUID, NIMBLE_PROPERTY::WRITE, 180
  );
  statusChar = service->createCharacteristic(
    STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 192
  );
  NimBLECharacteristic* infoChar = service->createCharacteristic(
    INFO_UUID, NIMBLE_PROPERTY::READ, 96
  );

  configChar->setCallbacks(new ConfigCallbacks());
  statusChar->setValue("{\"state\":\"ready\"}");
  infoChar->setValue("{\"name\":\"BOTROL\",\"protocol\":1,\"firmware\":\"provisioning-prototype-0.1\"}");

  if (!service->start()) {
    Serial.println("[BLE] Provisioning service start failed");
    return;
  }

  Serial.println("[BLE] Provisioning GATT service ready");
  publishStatus("ready", "Connect from the BOTROL setup website");
}

void beginWifiAttempt(const String& ssid, const String& password, WifiAttempt kind) {
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiAttempt = kind;
  wifiAttemptAt = millis();
}

void handleNewCredentials() {
  char ssid[33];
  char password[64];
  bool received = false;

  portENTER_CRITICAL(&configMux);
  if (configPending) {
    strlcpy(ssid, pendingSsid, sizeof(ssid));
    strlcpy(password, pendingPassword, sizeof(password));
    configPending = false;
    received = true;
  }
  portEXIT_CRITICAL(&configMux);

  if (!received) return;

  candidateSsid = ssid;
  candidatePassword = password;
  restoreSsid = activeSsid;
  restorePassword = activePassword;
  publishStatus("connecting", "Trying the new Wi-Fi network");
  beginWifiAttempt(candidateSsid, candidatePassword, WIFI_CANDIDATE);
}

void handleWifiState() {
  if (wifiAttempt == WIFI_IDLE) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiAttempt == WIFI_CANDIDATE) {
      prefs.putString("ssid", candidateSsid);
      prefs.putString("password", candidatePassword);
      activeSsid = candidateSsid;
      activePassword = candidatePassword;
      publishStatus("wifi_connected", "Wi-Fi configured and saved on this ESP32");
    } else {
      publishStatus("wifi_failed_restored", "New network failed; previous Wi-Fi restored");
    }
    wifiAttempt = WIFI_IDLE;
    return;
  }

  if (millis() - wifiAttemptAt < 20000) return;

  if (wifiAttempt == WIFI_CANDIDATE && restoreSsid.length()) {
    publishStatus("restoring", "New network failed; restoring previous Wi-Fi");
    beginWifiAttempt(restoreSsid, restorePassword, WIFI_RESTORE);
  } else {
    WiFi.disconnect(false, false);
    wifiAttempt = WIFI_IDLE;
    publishStatus("wifi_failed", "Could not connect; check credentials and try again");
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
  Serial.println("[BLE] Setup page launch requested");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  prefs.begin("botrol", false);
  activeSsid = prefs.getString("ssid", "");
  activePassword = prefs.getString("password", "");

  keyboard.begin();
  startProvisioningService();

  if (activeSsid.length()) {
    candidateSsid = activeSsid;
    candidatePassword = activePassword;
    publishStatus("wifi_reconnecting", "Using saved Wi-Fi configuration");
    beginWifiAttempt(activeSsid, activePassword, WIFI_CANDIDATE);
  } else {
    publishStatus("setup_required", "Connect with Web Bluetooth to configure Wi-Fi");
  }
}

void loop() {
  openSetupPage();
  handleNewCredentials();
  handleWifiState();
  delay(20);
}
