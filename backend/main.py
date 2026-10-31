import os
import secrets
import threading
from datetime import datetime, timezone
from typing import Any
from uuid import uuid4

from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, Field

from ai_planner import plan_task, PlannerError

app = FastAPI(
    title="BOTROL Cloud API",
    version="0.5.0",
    description="In-memory command relay for BOTROL ESP32 devices.",
)

allowed_origins = [
    origin.strip()
    for origin in os.getenv("BOTROL_ALLOWED_ORIGINS", "https://botrol-setup.vercel.app").split(",")
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
_LOCK = threading.RLock()
_DEVICES: dict[str, dict[str, Any]] = {}
_COMMANDS: dict[str, dict[str, Any]] = {}

def now() -> str:
    return datetime.now(timezone.utc).isoformat()

def _check_bearer(supplied: str, env_name: str, label: str) -> None:
    expected = os.getenv(env_name, "")
    if not expected:
        raise HTTPException(status_code=503, detail=f"{label} authentication is not configured")
    if not secrets.compare_digest(supplied, expected):
        raise HTTPException(status_code=401, detail=f"Invalid {label.lower()} token")

def require_device_token(credentials: HTTPAuthorizationCredentials | None = Depends(bearer)) -> str:
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
    command: str = Field(pattern="^(ping|ble_open_notepad|ble_open_calculator)$")
    payload: dict[str, Any] = Field(default_factory=dict)

class TaskRequest(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    transcript: str = Field(min_length=1, max_length=2000)

class CommandResult(BaseModel):
    status: str = Field(pattern="^(completed|failed)$")
    message: str = Field(default="", max_length=500)

@app.get("/health")
def health() -> dict[str, Any]:
    with _LOCK:
        return {
            "service": "BOTROL Cloud API",
            "status": "ok",
            "storage": "in-memory",
            "registered_devices": len(_DEVICES),
            "queued_commands": sum(1 for c in _COMMANDS.values() if c["status"] == "queued"),
            "version": "0.5.0",
        }

@app.post("/api/v1/devices/register")
def register_device(body: DeviceRegistration, _: str = Depends(require_device_token)) -> dict[str, Any]:
    timestamp = now()
    with _LOCK:
        device = _DEVICES.get(body.device_id)
        if device is None:
            device = {"device_id": body.device_id, "registered_at": timestamp}
            _DEVICES[body.device_id] = device
        device.update({"firmware_version": body.firmware_version, "last_seen": timestamp, "status": "online"})
        return {"ok": True, "device": dict(device)}

@app.get("/api/v1/devices/{device_id}/commands/next")
def get_next_command(device_id: str, _: str = Depends(require_device_token)) -> dict[str, Any]:
    with _LOCK:
        device = _DEVICES.get(device_id)
        if not device:
            # Recreate volatile device state after a Render restart.
            device = {"device_id": device_id, "firmware_version": "unknown", "registered_at": now(), "last_seen": now(), "status": "online"}
            _DEVICES[device_id] = device
        device.update({"last_seen": now(), "status": "online"})
        queued = [
            c for c in _COMMANDS.values()
            if c["device_id"] == device_id and c["status"] == "queued"
        ]
        if not queued:
            return {"ok": True, "command": None}
        command = min(queued, key=lambda c: (c["created_at"], c.get("step") or 0))
        command["status"] = "delivered"
        command["delivered_at"] = now()
        return {"ok": True, "command": dict(command)}

@app.post("/api/v1/devices/{device_id}/commands/{command_id}/result")
def report_result(device_id: str, command_id: str, body: CommandResult, _: str = Depends(require_device_token)) -> dict[str, bool]:
    with _LOCK:
        command = _COMMANDS.get(command_id)
        if not command or command["device_id"] != device_id:
            raise HTTPException(status_code=404, detail="Command not found for this device")
        command.update({"status": body.status, "result": body.message, "completed_at": now()})
        if device_id in _DEVICES:
            _DEVICES[device_id].update({"last_seen": now(), "status": "online"})
    return {"ok": True}

@app.get("/api/v1/admin/commands/{command_id}", dependencies=[Depends(require_admin)])
def get_command_status(command_id: str) -> dict[str, Any]:
    with _LOCK:
        command = _COMMANDS.get(command_id)
        if not command:
            raise HTTPException(status_code=404, detail="Command not found")
        return {"ok": True, "command": dict(command)}

def _enqueue(device_id: str, command: str, payload: dict[str, Any], task_id: str | None = None,
             step: int | None = None, summary: str = "") -> dict[str, Any]:
    command_id = str(uuid4())
    row = {
        "command_id": command_id, "task_id": task_id, "step": step, "task_summary": summary,
        "device_id": device_id, "command": command, "payload": payload, "status": "queued",
        "result": "", "created_at": now(), "delivered_at": None, "completed_at": None,
    }
    _COMMANDS[command_id] = row
    return dict(row)

@app.post("/api/v1/admin/commands", dependencies=[Depends(require_admin)])
def enqueue_command(body: CommandRequest) -> dict[str, Any]:
    with _LOCK:
        if body.device_id not in _DEVICES:
            raise HTTPException(status_code=404, detail="Device is not registered. Re-run device provisioning.")
        return {"ok": True, "command": _enqueue(body.device_id, body.command, body.payload)}

@app.post("/api/v1/admin/tasks/execute", dependencies=[Depends(require_admin)])
def execute_task(body: TaskRequest) -> dict[str, Any]:
    try:
        plan = plan_task(body.transcript)
    except PlannerError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    task_id = str(uuid4())
    steps = []
    with _LOCK:
        if body.device_id not in _DEVICES:
            raise HTTPException(status_code=404, detail="Device is not registered. Re-run device provisioning.")
        for index, step in enumerate(plan["steps"], start=1):
            row = _enqueue(body.device_id, step["command"], step["payload"], task_id, index, plan["summary"])
            steps.append({"command_id": row["command_id"], "command": row["command"], "status": "queued"})
    return {"ok": True, "task": {"task_id": task_id, "summary": plan["summary"], "steps": steps}}

@app.get("/api/v1/admin/tasks/{task_id}", dependencies=[Depends(require_admin)])
def get_task_status(task_id: str) -> dict[str, Any]:
    with _LOCK:
        rows = [
            {"command_id": c["command_id"], "command": c["command"], "status": c["status"],
             "result": c["result"], "step": c["step"]}
            for c in _COMMANDS.values() if c.get("task_id") == task_id
        ]
        rows.sort(key=lambda c: c["step"] or 0)
    if not rows:
        raise HTTPException(status_code=404, detail="Task not found")
    return {"ok": True, "task_id": task_id, "steps": rows}
