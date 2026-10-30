"""Provider-adaptable BOTROL task planner. API keys stay on the cloud server."""
import json
import os
import urllib.error
import urllib.request

ALLOWED = {"ping", "ble_open_notepad", "ble_open_calculator"}
PROMPT = """You plan tasks for BOTROL. Convert the user's request into JSON only:
{"summary":"short summary","steps":[{"command":"ping","payload":{}}]}
Allowed commands ONLY: ping, ble_open_notepad, ble_open_calculator.
These respectively check device connectivity, open Windows Notepad, and open Windows Calculator.
Never output shell commands, scripts, arbitrary keystrokes, or unsupported actions.
If the request needs an unsupported action, return {"summary":"Unsupported action","steps":[]}.
Maximum 8 steps. Do not claim execution. Keep commands in the requested order."""


class PlannerError(Exception):
    pass


def _request(url, headers, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
        headers={**headers, "Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=40) as response:
            return json.loads(response.read().decode())
    except (urllib.error.URLError, TimeoutError, ValueError) as exc:
        raise PlannerError("AI provider request failed") from exc


def _generate(transcript):
    provider = os.getenv("BOTROL_AI_PROVIDER", "").lower().strip()
    key = os.getenv("BOTROL_AI_API_KEY", "")
    model = os.getenv("BOTROL_AI_MODEL", "")
    if not key:
        raise PlannerError("AI provider API key is not configured on BOTROL Cloud")
    if provider in ("openai", "openai-compatible"):
        base = os.getenv("BOTROL_AI_BASE_URL", "https://api.openai.com/v1").rstrip("/")
        data = _request(base + "/chat/completions", {"Authorization": "Bearer " + key},
            {"model": model or "gpt-4o-mini", "temperature": 0,
             "response_format": {"type": "json_object"},
             "messages": [{"role": "system", "content": PROMPT},
                          {"role": "user", "content": transcript}]})
        return data["choices"][0]["message"]["content"]
    if provider == "gemini":
        model = model or "gemini-2.5-flash"
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent?key={key}"
        data = _request(url, {}, {
            "systemInstruction": {"parts": [{"text": PROMPT}]},
            "contents": [{"role": "user", "parts": [{"text": transcript}]}],
            "generationConfig": {"temperature": 0, "responseMimeType": "application/json"}})
        return data["candidates"][0]["content"]["parts"][0]["text"]
    raise PlannerError("Set BOTROL_AI_PROVIDER to openai or gemini in cloud environment")


def plan_task(transcript):
    try:
        raw = json.loads(_generate(transcript))
        steps = raw.get("steps")
        if not isinstance(steps, list) or not 1 <= len(steps) <= 8:
            raise ValueError("Plan must contain 1 to 8 supported steps")
        safe = []
        for step in steps:
            if not isinstance(step, dict) or step.get("command") not in ALLOWED:
                raise ValueError("AI proposed an unsupported command")
            payload = step.get("payload", {})
            if payload != {}:
                raise ValueError("Unexpected command payload")
            safe.append({"command": step["command"], "payload": {}})
        return {"summary": str(raw.get("summary", "Task plan"))[:240], "steps": safe}
    except (ValueError, KeyError, TypeError, IndexError) as exc:
        raise PlannerError("AI returned an invalid or unsafe plan: " + str(exc)) from exc
