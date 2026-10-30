import os
import secrets
import platform
import subprocess
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
    version="0.2.0",
    description="Cloud relay for BOTROL ESP32 devices and desktop agents.",
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
device_queues: dict[str, deque] = defaultdict(deque)
device_commands: dict[str, dict[str, Any]] = {}
desktops: dict[str, dict[str, Any]] = {}
desktop_queues: dict[str, deque] = defaultdict(deque)
desktop_commands: dict[str, dict[str, Any]] = {}


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


def require_desktop_token(
    credentials: HTTPAuthorizationCredentials | None = Depends(bearer),
) -> str:
    _check_bearer(credentials.credentials if credentials else "", "BOTROL_DESKTOP_TOKEN", "Desktop")
    return "desktop"


def require_admin(authorization: str | None = Header(default=None)) -> None:
    supplied = authorization.removeprefix("Bearer ").strip() if authorization else ""
    _check_bearer(supplied, "BOTROL_ADMIN_TOKEN", "Admin")


class DeviceRegistration(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    firmware_version: str = Field(default="unknown", max_length=40)


class DeviceCommandRequest(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    command: str = Field(default="ping", pattern="^ping$")
    payload: dict[str, Any] = Field(default_factory=dict)


class DesktopRegistration(BaseModel):
    desktop_id: str = Field(min_length=1, max_length=80)
    agent_version: str = Field(default="0.1.0", max_length=40)
    operating_system: str = Field(default="unknown", max_length=80)


class DesktopCommandRequest(BaseModel):
    desktop_id: str = Field(default="BOTROL-DESKTOP-001", min_length=1, max_length=80)
    command: str = Field(pattern="^(open_notepad|open_calculator)$")


class CommandResult(BaseModel):
    status: str = Field(pattern="^(completed|failed)$")
    message: str = Field(default="", max_length=500)


def _new_command(target_id: str, command_name: str, target_kind: str) -> dict[str, Any]:
    return {
        "command_id": str(uuid4()),
        "target_id": target_id,
        "target_kind": target_kind,
        "command": command_name,
        "status": "queued",
        "created_at": datetime.now(timezone.utc).isoformat(),
    }


@app.get("/health")
def health() -> dict[str, str]:
    return {"service": "BOTROL Cloud API", "status": "ok", "version": "0.2.0"}


# ESP32 routes: the existing ping flow remains unchanged.
@app.post("/api/v1/devices/register")
def register_device(body: DeviceRegistration, _: str = Depends(require_device_token)) -> dict[str, Any]:
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
def get_next_device_command(device_id: str, _: str = Depends(require_device_token)) -> dict[str, Any]:
    if device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    devices[device_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    if not device_queues[device_id]:
        return {"ok": True, "command": None}
    command = device_queues[device_id].popleft()
    command["status"] = "delivered"
    command["delivered_at"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True, "command": command}


@app.post("/api/v1/devices/{device_id}/commands/{command_id}/result")
def report_device_result(
    device_id: str, command_id: str, body: CommandResult,
    _: str = Depends(require_device_token),
) -> dict[str, bool]:
    if device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    command = device_commands.get(command_id)
    if not command or command["target_id"] != device_id:
        raise HTTPException(status_code=404, detail="Command not found")
    command.update(status=body.status, result=body.message, completed_at=datetime.now(timezone.utc).isoformat())
    devices[device_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True}


@app.post("/api/v1/admin/commands", dependencies=[Depends(require_admin)])
def enqueue_device_command(body: DeviceCommandRequest) -> dict[str, Any]:
    if body.device_id not in devices:
        raise HTTPException(status_code=404, detail="Device is not registered")
    command = _new_command(body.device_id, body.command, "device")
    command["payload"] = body.payload
    device_commands[command["command_id"]] = command
    device_queues[body.device_id].append(command)
    return {"ok": True, "command": command}


# Desktop agent routes. It has a separate token and queue from the ESP32.
@app.post("/api/v1/desktops/register")
def register_desktop(body: DesktopRegistration, _: str = Depends(require_desktop_token)) -> dict[str, Any]:
    now = datetime.now(timezone.utc).isoformat()
    desktops[body.desktop_id] = {
        "desktop_id": body.desktop_id,
        "agent_version": body.agent_version,
        "operating_system": body.operating_system,
        "registered_at": desktops.get(body.desktop_id, {}).get("registered_at", now),
        "last_seen": now,
        "status": "online",
    }
    return {"ok": True, "desktop": desktops[body.desktop_id]}


@app.get("/api/v1/desktops/{desktop_id}/commands/next")
def get_next_desktop_command(desktop_id: str, _: str = Depends(require_desktop_token)) -> dict[str, Any]:
    if desktop_id not in desktops:
        raise HTTPException(status_code=404, detail="Desktop is not registered")
    desktops[desktop_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    if not desktop_queues[desktop_id]:
        return {"ok": True, "command": None}
    command = desktop_queues[desktop_id].popleft()
    command["status"] = "delivered"
    command["delivered_at"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True, "command": command}


@app.post("/api/v1/desktops/{desktop_id}/commands/{command_id}/result")
def report_desktop_result(
    desktop_id: str, command_id: str, body: CommandResult,
    _: str = Depends(require_desktop_token),
) -> dict[str, bool]:
    if desktop_id not in desktops:
        raise HTTPException(status_code=404, detail="Desktop is not registered")
    command = desktop_commands.get(command_id)
    if not command or command["target_id"] != desktop_id:
        raise HTTPException(status_code=404, detail="Command not found")
    command.update(status=body.status, result=body.message, completed_at=datetime.now(timezone.utc).isoformat())
    desktops[desktop_id]["last_seen"] = datetime.now(timezone.utc).isoformat()
    return {"ok": True}


@app.post("/api/v1/admin/desktop-commands", dependencies=[Depends(require_admin)])
def enqueue_desktop_command(body: DesktopCommandRequest) -> dict[str, Any]:
    if body.desktop_id not in desktops:
        raise HTTPException(status_code=404, detail="Desktop is not registered")
    command = _new_command(body.desktop_id, body.command, "desktop")
    desktop_commands[command["command_id"]] = command
    desktop_queues[body.desktop_id].append(command)
    return {"ok": True, "command": command}
