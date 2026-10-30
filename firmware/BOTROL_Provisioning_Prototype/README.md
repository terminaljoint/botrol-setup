# BOTROL BLE provisioning prototype

Experimental firmware. It does not replace the working production sketch.

## Target

- ESP32-WROOM-32 / DevKit V1
- Arduino ESP32 core compatible with the installed HijelHID_BLEKeyboard library
- HijelHID_BLEKeyboard
- NimBLE-Arduino 2.x (the same NimBLE stack used by the HID library)
- ArduinoJson
- Preferences (included with ESP32 Arduino core)

## Test flow

1. Open `BOTROL_Provisioning_Prototype.ino` in Arduino IDE.
2. Select the ESP32 Dev Module and compile.
3. Upload to a spare test cycle / only after preserving the current working sketch.
4. Pair the BLE keyboard named `BOTROL` with Windows.
5. The keyboard should open `https://botrol-setup.vercel.app/provision.html`.
6. In Chrome or Edge, click **Connect BOTROL** and select BOTROL.
7. Enter Wi-Fi credentials and click **Save and connect**.
8. Watch the page status and Arduino Serial Monitor.

## BLE protocol

- Service: `7b100001-6d2a-4f8b-9c31-2d5a7e110001`
- Config write: `7b100002-6d2a-4f8b-9c31-2d5a7e110001`
- Status read/notify: `7b100003-6d2a-4f8b-9c31-2d5a7e110001`
- Device info read: `7b100004-6d2a-4f8b-9c31-2d5a7e110001`

Config payload is UTF-8 JSON: `{"ssid":"network","password":"secret"}`.

Wi-Fi credentials are stored in ESP32 NVS only after the candidate network connects successfully. If a new network fails, the firmware attempts to restore the previous saved network.

## Prototype limitations

- The HID website opener currently uses Windows Win+R.
- BLE pairing is encrypted/bonded, but this prototype uses the HID library's default Just Works pairing, which does not provide MITM protection. Do not treat this as production-grade provisioning security.
- GATT service creation after the HID library starts its BLE server must be validated on the actual board/library versions.
- Web Bluetooth requires a supported Chromium browser, HTTPS, and a user click.
- Cloud registration and OTA are intentionally not included yet.
- This prototype has not been hardware-tested or compiled in the user's Arduino environment. Validate before replacing the working firmware.
