import os
import secrets
from contextlib import contextmanager
from datetime import datetime, timezone
from typing import Any
from uuid import uuid4

import psycopg
from psycopg.rows import dict_row
from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, Field

from ai_planner import plan_task, PlannerError

app = FastAPI(
    title="BOTROL Cloud API",
    version="0.4.0",
    description="Persistent cloud relay for BOTROL ESP32 devices with BLE HID desktop control.",
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
DATABASE_URL = os.getenv("DATABASE_URL", "").strip()
SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS botrol_devices (
    device_id TEXT PRIMARY KEY,
    firmware_version TEXT NOT NULL DEFAULT 'unknown',
    registered_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_seen TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    status TEXT NOT NULL DEFAULT 'online'
);
CREATE TABLE IF NOT EXISTS botrol_commands (
    command_id UUID PRIMARY KEY,
    task_id UUID,
    step INTEGER,
    task_summary TEXT,
    device_id TEXT NOT NULL REFERENCES botrol_devices(device_id),
    command TEXT NOT NULL CHECK (command IN ('ping','ble_open_notepad','ble_open_calculator')),
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    status TEXT NOT NULL DEFAULT 'queued' CHECK (status IN ('queued','delivered','completed','failed')),
    result TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    delivered_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS botrol_commands_queue_idx
    ON botrol_commands(device_id, status, created_at);
CREATE INDEX IF NOT EXISTS botrol_commands_task_idx
    ON botrol_commands(task_id, step);
"""

def db_connect():
    if not DATABASE_URL:
        raise HTTPException(status_code=503, detail="Persistent database is not configured. Set DATABASE_URL in Render.")
    try:
        conn = psycopg.connect(DATABASE_URL, connect_timeout=10, sslmode="require", row_factory=dict_row)
        conn.execute(SCHEMA_SQL)
        return conn
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"BOTROL database unavailable: {type(exc).__name__}") from exc

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
    try:
        with db_connect() as conn:
            conn.execute("SELECT 1")
        return {"service": "BOTROL Cloud API", "status": "ok", "database": "connected", "version": "0.4.0"}
    except HTTPException as exc:
        return {"service": "BOTROL Cloud API", "status": "degraded", "database": "unavailable", "detail": exc.detail, "version": "0.4.0"}

@app.post("/api/v1/devices/register")
def register_device(body: DeviceRegistration, _: str = Depends(require_device_token)) -> dict[str, Any]:
    now = datetime.now(timezone.utc)
    with db_connect() as conn:
        row = conn.execute("""
            INSERT INTO botrol_devices(device_id, firmware_version, registered_at, last_seen, status)
            VALUES (%s, %s, %s, %s, 'online')
            ON CONFLICT(device_id) DO UPDATE SET
                firmware_version=EXCLUDED.firmware_version, last_seen=EXCLUDED.last_seen, status='online'
            RETURNING device_id, firmware_version, registered_at, last_seen, status
        """, (body.device_id, body.firmware_version, now, now)).fetchone()
    return {"ok": True, "device": row}

@app.get("/api/v1/devices/{device_id}/commands/next")
def get_next_command(device_id: str, _: str = Depends(require_device_token)) -> dict[str, Any]:
    with db_connect() as conn:
        device = conn.execute(
            "UPDATE botrol_devices SET last_seen=NOW(), status='online' WHERE device_id=%s RETURNING device_id",
            (device_id,),
        ).fetchone()
        if not device:
            raise HTTPException(status_code=404, detail="Device is not registered. Re-run device provisioning.")
        command = conn.execute("""
            SELECT command_id, task_id, step, device_id, command, payload, status, created_at
            FROM botrol_commands
            WHERE device_id=%s AND status='queued'
            ORDER BY created_at, step
            FOR UPDATE SKIP LOCKED LIMIT 1
        """, (device_id,)).fetchone()
        if not command:
            return {"ok": True, "command": None}
        updated = conn.execute("""
            UPDATE botrol_commands SET status='delivered', delivered_at=NOW()
            WHERE command_id=%s
            RETURNING command_id, task_id, step, device_id, command, payload, status, created_at, delivered_at
        """, (command["command_id"],)).fetchone()
    updated["command_id"] = str(updated["command_id"])
    if updated.get("task_id"):
        updated["task_id"] = str(updated["task_id"])
    return {"ok": True, "command": updated}

@app.post("/api/v1/devices/{device_id}/commands/{command_id}/result")
def report_result(device_id: str, command_id: str, body: CommandResult, _: str = Depends(require_device_token)) -> dict[str, bool]:
    with db_connect() as conn:
        row = conn.execute("""
            UPDATE botrol_commands SET status=%s, result=%s, completed_at=NOW()
            WHERE command_id=%s AND device_id=%s RETURNING command_id
        """, (body.status, body.message, command_id, device_id)).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Command not found for this device")
        conn.execute("UPDATE botrol_devices SET last_seen=NOW(), status='online' WHERE device_id=%s", (device_id,))
    return {"ok": True}

@app.get("/api/v1/admin/commands/{command_id}", dependencies=[Depends(require_admin)])
def get_command_status(command_id: str) -> dict[str, Any]:
    with db_connect() as conn:
        row = conn.execute("""
            SELECT command_id, device_id, command, status, result, created_at, delivered_at, completed_at
            FROM botrol_commands WHERE command_id=%s
        """, (command_id,)).fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Command not found")
    row["command_id"] = str(row["command_id"])
    return {"ok": True, "command": row}

@app.post("/api/v1/admin/commands", dependencies=[Depends(require_admin)])
def enqueue_command(body: CommandRequest) -> dict[str, Any]:
    command_id = uuid4()
    with db_connect() as conn:
        if not conn.execute("SELECT device_id FROM botrol_devices WHERE device_id=%s", (body.device_id,)).fetchone():
            raise HTTPException(status_code=404, detail="Device is not registered. Re-run device provisioning.")
        row = conn.execute("""
            INSERT INTO botrol_commands(command_id, device_id, command, payload)
            VALUES (%s, %s, %s, %s) RETURNING command_id, device_id, command, payload, status, created_at
        """, (command_id, body.device_id, body.command, psycopg.types.json.Jsonb(body.payload))).fetchone()
    row["command_id"] = str(row["command_id"])
    return {"ok": True, "command": row}

@app.post("/api/v1/admin/tasks/execute", dependencies=[Depends(require_admin)])
def execute_task(body: TaskRequest) -> dict[str, Any]:
    try:
        plan = plan_task(body.transcript)
    except PlannerError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    task_id = uuid4()
    task_steps = []
    with db_connect() as conn:
        if not conn.execute("SELECT device_id FROM botrol_devices WHERE device_id=%s", (body.device_id,)).fetchone():
            raise HTTPException(status_code=404, detail="Device is not registered. Re-run device provisioning.")
        for index, step in enumerate(plan["steps"], start=1):
            command_id = uuid4()
            conn.execute("""
                INSERT INTO botrol_commands(command_id, task_id, step, task_summary, device_id, command, payload)
                VALUES (%s, %s, %s, %s, %s, %s, %s)
            """, (command_id, task_id, index, plan["summary"], body.device_id, step["command"], psycopg.types.json.Jsonb(step["payload"])))
            task_steps.append({"command_id": str(command_id), "command": step["command"], "status": "queued"})
    return {"ok": True, "task": {"task_id": str(task_id), "summary": plan["summary"], "steps": task_steps}}

@app.get("/api/v1/admin/tasks/{task_id}", dependencies=[Depends(require_admin)])
def get_task_status(task_id: str) -> dict[str, Any]:
    with db_connect() as conn:
        rows = conn.execute("""
            SELECT command_id, command, status, result, step
            FROM botrol_commands WHERE task_id=%s ORDER BY step
        """, (task_id,)).fetchall()
    if not rows:
        raise HTTPException(status_code=404, detail="Task not found")
    for row in rows:
        row["command_id"] = str(row["command_id"])
    return {"ok": True, "task_id": task_id, "steps": rows}
