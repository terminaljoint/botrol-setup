# BOTROL Cloud API

FastAPI relay for BOTROL ESP32 devices. The ESP32 polls this service over HTTPS and can execute a small allowlist of BLE HID keyboard actions on a paired computer.

## Routes

- `GET /health`: service health
- `POST /api/v1/devices/register`: register ESP32 (device token)
- `GET /api/v1/devices/{device_id}/commands/next`: fetch next command (device token)
- `POST /api/v1/devices/{device_id}/commands/{command_id}/result`: report result (device token)
- `POST /api/v1/admin/commands`: queue an allowlisted command (admin token)

## Command allowlist

- `ping`: connectivity test
- `ble_open_notepad`: ask the ESP32 BLE keyboard to open Notepad using Windows + R
- `ble_open_calculator`: ask the ESP32 BLE keyboard to open Calculator using Windows + R

The API rejects all other command identifiers. It does not accept arbitrary shell commands, scripts, URLs, or raw keystroke sequences.

## Render environment variables

- `BOTROL_DEVICE_TOKEN`: secret shared with ESP32 firmware.
- `BOTROL_ADMIN_TOKEN`: secret used by the dashboard to enqueue commands.
- `BOTROL_ALLOWED_ORIGINS`: `https://botrol-setup.vercel.app`

Never commit real tokens or place them in public source code.

## Deployment

Render uses Root Directory `backend`, build command `pip install -r requirements.txt`, and start command `uvicorn main:app --host 0.0.0.0 --port $PORT`.

## Prototype limitations

Device records and command queues are stored in memory and disappear when the service restarts or sleeps. This is a connectivity prototype. Before relying on BOTROL, add persistent storage, command expiry, replay protection, rate limits, audit logging, and stronger device authentication.

BLE HID requires one-time pairing with the computer. This milestone's shortcut commands are Windows-specific. The browser sends requests to the cloud; the ESP32 performs the BLE keyboard action.

## Local run

Set `BOTROL_DEVICE_TOKEN` and `BOTROL_ADMIN_TOKEN`, then run:

```bash
uvicorn main:app --reload
```

Open `/docs` on the local server to inspect the API.
