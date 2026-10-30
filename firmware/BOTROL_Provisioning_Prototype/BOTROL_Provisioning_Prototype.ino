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
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* statusChar = nullptr;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;

char pendingSsid[33] = {0};
char pendingPassword[64] = {0};
volatile bool configPending = false;

String activeSsid, activePassword, candidateSsid, candidatePassword;
String restoreSsid, restorePassword;

enum WifiAttempt { WIFI_IDLE, WIFI_CANDIDATE, WIFI_RESTORE };
WifiAttempt wifiAttempt = WIFI_IDLE;
uint32_t wifiAttemptAt = 0;
bool setupPageOpened = false;
bool provisioningMode = false;
bool keyboardResumePending = false;
uint32_t keyboardResumeAt = 0;

void publishStatus(const String& state, const String& detail = "") {
  StaticJsonDocument<192> doc;
  doc["state"] = state;
  if (detail.length()) doc["detail"] = detail;
  String json;
  serializeJson(doc, json);
  if (statusChar) {
    statusChar->setValue(state);
    statusChar->notify();
  }
  Serial.println("[BOTROL] " + json);
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
    size_t ssidLen = strlen(ssid), passwordLen = strlen(password);
    if (!ssidLen || ssidLen > 32 || passwordLen > 63) {
      publishStatus("rejected", "SSID or password length is invalid");
      return;
    }
    portENTER_CRITICAL(&configMux);
    strlcpy(pendingSsid, ssid, sizeof(pendingSsid));
    strlcpy(pendingPassword, password, sizeof(pendingPassword));
    configPending = true;
    portEXIT_CRITICAL(&configMux);
    publishStatus("credentials_received", "Wi-Fi credentials received");
  }
};

void startProvisioningService() {
  bleServer = NimBLEDevice::getServer();
  if (!bleServer) {
    Serial.println("[BLE] ERROR: HID GATT server unavailable");
    return;
  }
  NimBLEService* service = bleServer->createService(SERVICE_UUID);
  NimBLECharacteristic* config = service->createCharacteristic(
    CONFIG_UUID, NIMBLE_PROPERTY::WRITE, 180
  );
  statusChar = service->createCharacteristic(
    STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 192
  );
  NimBLECharacteristic* info = service->createCharacteristic(
    INFO_UUID, NIMBLE_PROPERTY::READ, 96
  );
  config->setCallbacks(new ConfigCallbacks());
  statusChar->setValue("ready");
  info->setValue("{\"name\":\"BOTROL\",\"protocol\":1,\"firmware\":\"provisioning-prototype-0.3\"}");
  if (!service->start()) {
    Serial.println("[BLE] ERROR: provisioning service failed to start");
    return;
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  Serial.println("[BLE] Provisioning GATT service registered");
}

size_t connectedPeerCount() {
  if (!bleServer) return 0;
  return bleServer->getPeerDevices().size();
}

bool disconnectAllBlePeers(const char* reason) {
  if (!bleServer) return false;
  Serial.printf("[BLE] Disconnecting peers (%s), current count: %u\n",
                reason, (unsigned)connectedPeerCount());
  auto peers = bleServer->getPeerDevices();
  for (auto& peer : peers) {
    bleServer->disconnect(peer);
  }

  uint32_t started = millis();
  while (connectedPeerCount() != 0 && millis() - started < 4000) {
    delay(50);
  }
  size_t remaining = connectedPeerCount();
  Serial.printf("[BLE] Peer count after disconnect: %u\n", (unsigned)remaining);
  return remaining == 0;
}

void enterProvisioningMode() {
  if (provisioningMode) return;
  Serial.println("[BLE] Switching HID -> provisioning");

  // Stop HID advertising and ask the HID library to release its active host.
  keyboard.end();

  // Explicitly force any remaining GATT peers off the shared NimBLE server.
  if (!disconnectAllBlePeers("enter provisioning")) {
    Serial.println("[BLE] ERROR: host connection still active; not starting provisioning advertising");
    return;
  }

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  NimBLEAdvertisementData scan;
  scan.setName(DEVICE_NAME);
  adv->setScanResponseData(scan);

  NimBLEDevice::startAdvertising();
  provisioningMode = true;
  Serial.println("[BLE] Provisioning advertising started (BOTROL name + service UUID)");
  publishStatus("ready", "Connect from the BOTROL setup website");
}

void resumeKeyboardMode() {
  if (!provisioningMode) return;
  Serial.println("[BLE] Switching provisioning -> HID");

  if (!disconnectAllBlePeers("resume keyboard")) {
    Serial.println("[BLE] ERROR: browser connection still active; delaying keyboard restart");
    keyboardResumePending = true;
    keyboardResumeAt = millis() + 1500;
    return;
  }

  NimBLEDevice::stopAdvertising();
  statusChar = nullptr;
  provisioningMode = false;
  keyboard.begin();
  Serial.println("[BLE] Keyboard advertising restarted");
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
  char ssid[33], password[64];
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
  publishStatus("connecting", "Connecting to the selected Wi-Fi network");
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
      publishStatus("wifi_connected", "Wi-Fi connected and saved on ESP32");
      keyboardResumePending = true;
      keyboardResumeAt = millis() + 6000;
    } else {
      publishStatus("wifi_failed_restored", "Previous Wi-Fi connection restored");
    }
    wifiAttempt = WIFI_IDLE;
    return;
  }
  if (millis() - wifiAttemptAt < 20000) return;
  if (wifiAttempt == WIFI_CANDIDATE && restoreSsid.length()) {
    publishStatus("restoring", "New Wi-Fi failed; restoring previous Wi-Fi");
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
  Serial.println("[BLE] Setup page launch requested");
  delay(1500);
  enterProvisioningMode();
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
    publishStatus("setup_required", "Pair BOTROL keyboard to open Wi-Fi setup");
  }
}

void loop() {
  openSetupPage();
  handleNewCredentials();
  handleWifiState();

  if (keyboardResumePending && (int32_t)(millis() - keyboardResumeAt) >= 0) {
    keyboardResumePending = false;
    resumeKeyboardMode();
  }
  delay(20);
}
