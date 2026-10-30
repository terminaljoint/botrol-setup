# BOTROL Cloud API

FastAPI relay for BOTROL ESP32 devices and desktop agents.

## Routes

- `GET /health`: service health
- `POST /api/v1/devices/register`: register ESP32 (device token)
- `GET /api/v1/devices/{device_id}/commands/next`: ESP32 command polling (device token)
- `POST /api/v1/devices/{device_id}/commands/{command_id}/result`: ESP32 result (device token)
- `POST /api/v1/admin/commands`: enqueue ESP32 `ping` (admin token)
- `POST /api/v1/desktops/register`: register desktop agent (desktop token)
- `GET /api/v1/desktops/{desktop_id}/commands/next`: desktop command polling (desktop token)
- `POST /api/v1/desktops/{desktop_id}/commands/{command_id}/result`: desktop result (desktop token)
- `POST /api/v1/admin/desktop-commands`: enqueue an allowlisted desktop action (admin token)

## Desktop command allowlist

Only `open_notepad` and `open_calculator` are accepted. The cloud API only queues these identifiers. The desktop agent maps them to fixed local application launches. It does not run shell commands or arbitrary scripts.

## Render environment variables

- `BOTROL_DEVICE_TOKEN`: separate secret used by the ESP32 firmware.
- `BOTROL_DESKTOP_TOKEN`: separate secret used by the desktop agent.
- `BOTROL_ADMIN_TOKEN`: secret used by the command dashboard to enqueue commands.
- `BOTROL_ALLOWED_ORIGINS`: `https://botrol-setup.vercel.app`

The Render blueprint asks Render to generate all three secrets. If the service already exists, ensure `BOTROL_DESKTOP_TOKEN` is added under Render → Environment. Never commit real tokens or put them in public source code.

## Deploy

The Render service uses Root Directory `backend`, build command `pip install -r requirements.txt`, and start command `uvicorn main:app --host 0.0.0.0 --port $PORT`.

## Prototype limitations

Device/desktop records and command queues are stored in memory. They disappear when the service restarts or sleeps. This is a connectivity prototype, not a reliable production command system. Before broader use, add persistent storage, per-device credential management, command expiry, replay protection, rate limits, audit logging, and stronger transport/device security.

## Local run

Install dependencies, set the three token environment variables, then run:

```bash
uvicorn main:app --reload
```

Open `/docs` on the local server to inspect the API.
