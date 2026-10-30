# BOTROL USB provisioning + cloud prototype

Experimental firmware. It does not replace the working production sketch.

## Target

- ESP32-WROOM-32 / DevKit V1
- Arduino ESP32 core compatible with the installed HijelHID_BLEKeyboard library
- HijelHID_BLEKeyboard
- ArduinoJson
- Preferences (included with ESP32 Arduino core)

## Setup flow

1. Open `BOTROL_Provisioning_Prototype.ino` in Arduino IDE.
2. Select the ESP32 Dev Module and compile.
3. Upload to the test ESP32.
4. Pair the BLE keyboard named `BOTROL` with Windows once. The keyboard opens the setup page.
5. Connect the ESP32 to the computer using a USB data cable.
6. Open `https://botrol-setup.vercel.app/provision.html` in Chrome or Edge.
7. Click **Choose ESP32 port** and select the ESP32 serial port (often Silicon Labs CP210x).
8. Enter Wi-Fi credentials and click **Save and connect**.
9. Enter the `BOTROL_DEVICE_TOKEN` configured in the Render service environment and click **Save token and register device**. Do not use the admin token.
10. Wait for the page to show `cloud_registered`.

## USB serial protocol

- Baud rate: `115200`
- One UTF-8 JSON object per line.
- Wi-Fi command: `{"type":"wifi_config","ssid":"network","password":"secret"}`
- Cloud token command: `{"type":"cloud_config","device_token":"..."}`
- Device emits JSON status lines with `type: "status"`, `state`, optional `detail`, `wifi`, `ssid`, and `cloud`.

Wi-Fi credentials and the device token are stored in ESP32 NVS. The Wi-Fi credentials are saved only after a successful Wi-Fi connection. The token is stored locally and sent as a Bearer credential only to BOTROL Cloud. Neither is included in the public source code or sent as configuration data to the cloud.

Close Arduino Serial Monitor before the browser opens the port. Only one application can own the serial port at a time.

## Cloud API behavior

- Registers `BOTROL-ESP32-001` with `POST /api/v1/devices/register`.
- Polls `GET /api/v1/devices/BOTROL-ESP32-001/commands/next` every 5 seconds.
- Reports command outcomes to the matching `/commands/{command_id}/result` endpoint.
- Supported commands: `ping`, `ble_open_notepad`, `ble_open_calculator`.
- Registration is retried every 30 seconds when it fails.

The current backend uses in-memory command queues. Pending commands are lost if the service restarts or sleeps.

## Prototype security and limitations

- HTTPS currently uses `WiFiClientSecure::setInsecure()` for bring-up testing only. This encrypts traffic but does not validate the server certificate. Replace it with trusted CA validation before real-world use.
- The device token is a bearer secret. Keep it private and rotate it if exposed.
- The HID website opener and app shortcuts currently use Windows Win+R.
- Web Serial requires Chrome or Edge, HTTPS or localhost, and a user click to select the serial port.
- This prototype has not been compiled or hardware-tested after the cloud integration changes.
- Cloud registration and basic HTTPS polling are included; OTA, persistent cloud storage, WebSocket transport, and production-grade device identity are not included.
