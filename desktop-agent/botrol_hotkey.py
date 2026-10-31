"""BOTROL Windows voice hotkey agent.

Hotkey: Win + Ctrl + Alt + B
Speech transcription: Google Web Speech (no API key; internet required)
Task planning/execution: BOTROL Cloud's built-in command parser.
Admin token: Windows Credential Manager via keyring.
"""
import ctypes
import getpass
import json
import os
import queue
import sys
import threading
import time
import urllib.error
import urllib.request
import wave
from ctypes import wintypes

import keyring
import numpy as np
import sounddevice as sd
import speech_recognition as sr

API = "https://botrol-cloud.onrender.com"
DEVICE_ID = "BOTROL-ESP32-001"
SERVICE_NAME = "BOTROL Desktop Agent"
USERNAME = "admin-token"
SAMPLE_RATE = 16000
RECORD_SECONDS = 7
LANGUAGE = os.getenv("BOTROL_SPEECH_LANGUAGE", "en-IN")

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000
VK_B = 0x42
WM_HOTKEY = 0x0312
HOTKEY_ID = 1


def get_token():
    token = keyring.get_password(SERVICE_NAME, USERNAME)
    if token:
        return token
    print("\nFirst-time setup: enter your BOTROL_ADMIN_TOKEN.")
    print("It will be stored in Windows Credential Manager, not in this script.")
    token = getpass.getpass("Admin token: ").strip()
    if not token:
        raise RuntimeError("No admin token entered.")
    keyring.set_password(SERVICE_NAME, USERNAME, token)
    print("Token saved securely in Windows Credential Manager.\n")
    return token


def http_json(path, token, body=None):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Authorization": "Bearer " + token}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(API + path, data=data, headers=headers,
                                     method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(request, timeout=35) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"BOTROL Cloud returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Cannot reach BOTROL Cloud: {exc.reason}") from exc


def capture_and_transcribe():
    print(f"\n🎙 Listening for up to {RECORD_SECONDS} seconds...")
    audio = sd.rec(int(RECORD_SECONDS * SAMPLE_RATE), samplerate=SAMPLE_RATE,
                   channels=1, dtype="int16")
    sd.wait()
    raw = audio.reshape(-1).tobytes()
    recognizer = sr.Recognizer()
    audio_data = sr.AudioData(raw, SAMPLE_RATE, 2)
    try:
        text = recognizer.recognize_google(audio_data, language=LANGUAGE)
    except sr.UnknownValueError as exc:
        raise RuntimeError("I couldn't understand that. Try speaking clearly and closer to the microphone.") from exc
    except sr.RequestError as exc:
        raise RuntimeError(f"Speech recognition service unavailable: {exc}") from exc
    print("You said:", text)
    return text


def run_task(token, transcript):
    result = http_json("/api/v1/admin/tasks/execute", token,
                       {"device_id": DEVICE_ID, "transcript": transcript})
    task = result["task"]
    print("\nTask:", task["summary"])
    for index, step in enumerate(task["steps"], 1):
        print(f"  {index}. {step['command']} [{step['status']}]")
    print("\nWaiting for ESP32...")
    deadline = time.time() + 90
    while time.time() < deadline:
        time.sleep(1.5)
        status = http_json("/api/v1/admin/tasks/" + task["task_id"], token)
        steps = status["steps"]
        for index, step in enumerate(steps, 1):
            print(f"  {index}. {step['command']}: {step['status']} {step.get('result', '')}")
        if all(step["status"] in ("completed", "failed") for step in steps):
            if any(step["status"] == "failed" for step in steps):
                print("⚠ Task finished with an error.")
            else:
                print("✓ Task completed.")
            return
    print("Task is still pending. Check ESP32 power, Wi-Fi and Bluetooth pairing.")


def main():
    if sys.platform != "win32":
        raise SystemExit("This first desktop hotkey agent is for Windows 10/11.")
    token = get_token()
    print("Checking BOTROL Cloud...")
    health = urllib.request.urlopen(API + "/health", timeout=20)
    if health.status != 200:
        raise RuntimeError("BOTROL Cloud health check failed.")
    print("BOTROL Cloud connected.")
    print("\nBOTROL is ready.")
    print("Hotkey: Win + Ctrl + Alt + B")
    print(f"Speech language: {LANGUAGE} (change BOTROL_SPEECH_LANGUAGE to ta-IN for Tamil)")
    print("Press the hotkey, speak within 7 seconds. Press Ctrl+C to exit.")
    print("Note: speech audio is sent to Google's speech recognition service for transcription.")

    busy = threading.Lock()

    def activate():
        if not busy.acquire(blocking=False):
            print("\nBOTROL is busy with the previous task.")
            return
        def worker():
            try:
                transcript = capture_and_transcribe()
                run_task(token, transcript)
            except Exception as exc:
                print("\n✗", exc)
            finally:
                busy.release()
                print("\nReady. Press Win + Alt + Space to speak.")
        threading.Thread(target=worker, daemon=True).start()

    user32 = ctypes.windll.user32
    if not user32.RegisterHotKey(None, HOTKEY_ID, MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_B):
        raise RuntimeError("Could not register Win + Alt + Space. Another application may already use it.")
    try:
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) != 0:
            if msg.message == WM_HOTKEY and msg.wParam == HOTKEY_ID:
                activate()
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
    finally:
        user32.UnregisterHotKey(None, HOTKEY_ID)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBOTROL stopped.")
    except Exception as error:
        print("\nBOTROL could not start:", error)
        input("Press Enter to close...")
