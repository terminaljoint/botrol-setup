import json
import os
import platform
import shutil
import subprocess
import time
import urllib.error
import urllib.request

API = "https://botrol-cloud.onrender.com"
DESKTOP_ID = "BOTROL-DESKTOP-001"
VERSION = "0.1.0"
POLL_SECONDS = 3
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")


def load_config():
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as file:
            config = json.load(file)
    except FileNotFoundError:
        raise SystemExit("Missing config.json. Copy config.example.json to config.json and add your desktop token.")
    token = str(config.get("desktop_token", "")).strip()
    if not token or token == "PASTE_RENDER_BOTROL_DESKTOP_TOKEN_HERE":
        raise SystemExit("Add your Render BOTROL_DESKTOP_TOKEN to config.json.")
    return token


def request(method, path, token, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        API + path,
        data=data,
        method=method,
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=20) as response:
        raw = response.read().decode("utf-8")
        return json.loads(raw) if raw else {}


def launch_first_available(candidates):
    for app in candidates:
        executable = shutil.which(app[0])
        if executable:
            subprocess.Popen([executable] + app[1:], close_fds=True)
            return app[0]
    raise RuntimeError("No supported application was found. Install a desktop text editor or calculator.")


def execute(command_name):
    system = platform.system()
    if command_name == "open_notepad":
        if system == "Windows":
            subprocess.Popen(["notepad.exe"], close_fds=True)
            return "Opened Notepad"
        if system == "Darwin":
            subprocess.Popen(["open", "-a", "TextEdit"], close_fds=True)
            return "Opened TextEdit"
        app = launch_first_available([
            ("gedit", []), ("mousepad", []), ("kate", []), ("xed", []), ("leafpad", [])
        ])
        return "Opened text editor: " + app

    if command_name == "open_calculator":
        if system == "Windows":
            subprocess.Popen(["calc.exe"], close_fds=True)
            return "Opened Calculator"
        if system == "Darwin":
            subprocess.Popen(["open", "-a", "Calculator"], close_fds=True)
            return "Opened Calculator"
        app = launch_first_available([
            ("gnome-calculator", []), ("kcalc", []), ("galculator", []), ("mate-calc", [])
        ])
        return "Opened calculator: " + app

    raise ValueError("Command is not in the desktop agent allowlist: " + command_name)


def report(token, command_id, status, message):
    path = f"/api/v1/desktops/{DESKTOP_ID}/commands/{command_id}/result"
    return request("POST", path, token, {"status": status, "message": message[:500]})


def main():
    token = load_config()
    os_name = platform.system() + " " + platform.release()
    print("======================================")
    print("       BOTROL DESKTOP AGENT v" + VERSION)
    print("======================================")
    print("Desktop ID:", DESKTOP_ID)
    print("Operating system:", os_name)
    print("Connecting to BOTROL Cloud...")

    registration = request("POST", "/api/v1/desktops/register", token, {
        "desktop_id": DESKTOP_ID,
        "agent_version": VERSION,
        "operating_system": os_name,
    })
    print("Registered:", registration.get("ok"))
    print("Waiting for approved desktop commands. Press Ctrl+C to stop.")

    while True:
        try:
            result = request("GET", f"/api/v1/desktops/{DESKTOP_ID}/commands/next", token)
            command = result.get("command")
            if command:
                command_id = command.get("command_id", "")
                name = command.get("command", "")
                print("\n[ BOTROL ] Command received:", name)
                try:
                    message = execute(name)
                    report(token, command_id, "completed", message)
                    print("[ BOTROL ]", message)
                    print("[ BOTROL ] Result reported successfully")
                except Exception as exc:
                    report(token, command_id, "failed", str(exc))
                    print("[ BOTROL ] Command failed:", exc)
            time.sleep(POLL_SECONDS)
        except KeyboardInterrupt:
            print("\nBOTROL Desktop Agent stopped.")
            break
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            print(f"[ CLOUD ] HTTP {exc.code}: {detail}")
            time.sleep(5)
        except Exception as exc:
            print("[ CLOUD ] Connection error:", exc)
            time.sleep(5)


if __name__ == "__main__":
    main()
