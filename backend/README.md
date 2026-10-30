# BOTROL Cloud API

Minimal FastAPI relay for the BOTROL ESP32 prototype.

## Current prototype scope

- Health check: `GET /health`
- Device registration: `POST /api/v1/devices/register`
- Fetch next command: `GET /api/v1/devices/{device_id}/commands/next`
- Report command result: `POST /api/v1/devices/{device_id}/commands/{command_id}/result`
- Queue a test command: `POST /api/v1/admin/commands`

Only the harmless `ping` command is accepted in this milestone. This API does not execute computer actions.

## Deploy on Render

1. Create a new Web Service from this GitHub repository.
2. Set the Root Directory to `backend`.
3. Build command: `pip install -r requirements.txt`
4. Start command: `uvicorn main:app --host 0.0.0.0 --port $PORT`
5. Add environment variables:
   - `BOTROL_DEVICE_TOKEN`: a long random secret shared with the ESP32 firmware.
   - `BOTROL_ADMIN_TOKEN`: a separate long random secret for command submission.
   - `BOTROL_ALLOWED_ORIGINS`: `https://botrol-setup.vercel.app`

The included `render.yaml` supplies these settings and asks Render to generate both secrets. Never commit real tokens to GitHub or put them in the public website.

## Important prototype limitation

Device records and command queues are stored in memory. They disappear when the service restarts or sleeps. This is for connectivity testing only. Before relying on BOTROL, replace this storage with a persistent database/queue and add per-device credentials, expiry, replay protection, and stronger audit logging.

## Local run

Install dependencies, set both secret environment variables, then run:

```bash
uvicorn main:app --reload
```

Open `/docs` on the local server to inspect and test the API.
