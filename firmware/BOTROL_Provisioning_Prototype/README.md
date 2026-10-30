# BOTROL USB provisioning prototype

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
9. Watch the page status and Arduino Serial Monitor.

## USB serial protocol

- Baud rate: `115200`
- One UTF-8 JSON object per line.
- Browser-to-device Wi-Fi command:

```json
{"type":"wifi_config","ssid":"network","password":"secret"}
```

- Device-to-browser status messages use JSON lines with `type: "status"`, a `state`, and optional `detail`, `wifi`, and `ssid` fields.
- The serial monitor may be used for diagnostics, but close it before the browser opens the port. Only one application can own the serial port at a time.

Wi-Fi credentials are stored in ESP32 NVS only after the candidate network connects successfully. If a new network fails, the firmware attempts to restore the previous saved network. Credentials are not sent to BOTROL Cloud.

## Prototype limitations

- The HID website opener currently uses Windows Win+R.
- Web Serial requires a supported Chromium browser (Chrome or Edge), HTTPS or localhost, and a user click to select the serial port.
- Opening a serial port can reset some ESP32 USB-to-UART boards depending on adapter and DTR behavior. If that happens, wait for the ESP32 startup status to appear in the page.
- This prototype has not been compiled or hardware-tested in the user's Arduino environment. Validate before replacing the working firmware.
- Cloud registration and OTA are intentionally not included yet.
