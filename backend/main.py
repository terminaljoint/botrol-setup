import os
import secrets
from collections import defaultdict, deque
from datetime import datetime, timezone
from typing import Any
from uuid import uuid4

from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, Field

app = FastAPI(
    title="BOTROL Cloud API",
    version="0.3.0",
    description="Cloud relay for BOTROL ESP32 devices with BLE HID desktop control.",
)

allowed_origins = [
    origin.strip()
    for origin in os.getenv(
        "BOTROL_ALLOWED_ORIGINS",
        "https://botrol-setup.vercel.app",
    ).split(",")
    if origin.strip()
]

app.add_middleware(
    CORSMiddleware,
    allow_origins=allowed_origins,
    allow_credentials=False,
    allow_methods=["GET", "POST"],
    allow_headers=["Authorization", "Content-Type"],
)

bearer = HTTPBearer(auto_error=False)

# Prototype in-memory storage. Queues reset when the service restarts or sleeps.
devices: dict[str, dict[str, Any]] = {}
queues: dict[str, deque] = defaultdict(deque)
commands: dict[str, dict[str, Any]] = {}


def _check_bearer(supplied: str, env_name: str, label: str) -> None:
    expected = os.getenv(env_name, "")
    if not expected:
        raise HTTPException(status_code=503, detail=f"{label} authentication is not configured")
    if not secrets.compare_digest(supplied, expected):
        raise HTTPException(status_code=401, detail=f"Invalid {label.lower()} token")


def require_device_token(
    credentials: HTTPAuthorizationCredentials | None = Depends(bearer),
) -> str:
    _check_bearer(credentials.credentials if credentials else "", "BOTROL_DEVICE_TOKEN", "Device")
    return "device"


def require_admin(authorization: str | None = Header(default=None)) -> None:
    supplied = authorization.removeprefix("Bearer ").strip() if authorization else ""
    _check_bearer(supplied, "BOTROL_ADMIN_TOKEN", "Admin")


class DeviceRegistration(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    firmware_version: str = Field(default="unknown", max_length=40)


class CommandRequest(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    # Commands are fixed identifiers. No shell commands, scripts, or arbitrary keystrokes.
    command: str = Field(pattern="^(ping|ble_open_notepad|ble_open_calculator)$")
    payload: dict[str, Any] = Field(default_factory=dict)


class CommandResult(BaseModel):
    status: str = Field(pattern="^(completed|failed)$")
    message: str = Field(default="", max_length=500)


@app.get("/health")
def health() -> dict[str, str]:
    return {"service": "BOTROL Cloud API", "status": "ok", "version": "0.3.0"}


@app.post("/api/v1/devices/register")
def register_device(
    body: DeviceRegistration,
    _: str = Depends(require_device_token),
) -> dict[str, Any]:
    now = datetime.now(timezone.utc).isoformat()
    devices[body.device_id] = {
        "device_id": body.device_id,
        "firmware_version": body.firmware_version,
        "registered_at": devices.get(body.device_id, {}).get("registered_at", now),
        "last_seen": now,
        "status": "online",
    }
    return {"ok": True, "device": devices[body.device_id]}


@app.get("/api/v1/devices/{device_id}/commands/next")
def get_next_command(
    device_id: str,
    _: str = Depends(require_device_token),
) -> dict[str, Any]:
    if device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    devices[device_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    if not queues[device_id]:
        return {"ok": True, "command": None}
    command = queues[device_id].popleft()
    command["status"] = "delivered"
    command["delivered_at"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True, "command": command}


@app.post("/api/v1/devices/{device_id}/commands/{command_id}/result")
def report_result(
    device_id: str,
    command_id: str,
    body: CommandResult,
    _: str = Depends(require_device_token),
) -> dict[str, bool]:
    if device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    command = commands.get(command_id)
    if not command or command["device_id"] != device_id:
        raise HTTPException(status_code=404, detail="Command not found")
    command["status"] = body.status
    command["result"] = body.message
    command["completed_at"] = datetime.now(timezone.utc).isoformat()
    devices[device_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True}


@app.get("/api/v1/admin/commands/{command_id}", dependencies=[Depends(require_admin)])
def get_command_status(command_id: str) -> dict[str, Any]:
    command = commands.get(command_id)
    if not command:
        raise HTTPException(status_code=404, detail="Command not found")
    return {
        "ok": True,
        "command": {
            "command_id": command["command_id"],
            "device_id": command["device_id"],
            "command": command["command"],
            "status": command["status"],
            "result": command.get("result", ""),
            "created_at": command["created_at"],
            "delivered_at": command.get("delivered_at"),
            "completed_at": command.get("completed_at"),
        },
    }


@app.post("/api/v1/admin/commands", dependencies=[Depends(require_admin)])
def enqueue_command(body: CommandRequest) -> dict[str, Any]:
    if body.device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    command = {
        "command_id": str(uuid4()),
        "device_id": body.device_id,
        "command": body.command,
        "payload": body.payload,
        "status": "queued",
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    commands[command["command_id"]] = command
    queues[body.device_id].append(command)
    return {"ok": True, "command": command}
