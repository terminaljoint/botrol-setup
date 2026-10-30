"""Offline, deterministic task planner for BOTROL. No AI provider or API key required.

This is an MVP command parser, not a language model. It recognizes only the
explicit actions implemented by the current ESP32 firmware.
"""
import re

COMMANDS = {
    "ping": ("ping", ("ping", "check connection", "check device", "test connection")),
    "notepad": ("ble_open_notepad", ("open notepad", "launch notepad", "start notepad", "notepad")),
    "calculator": ("ble_open_calculator", ("open calculator", "launch calculator", "start calculator", "calculator", "calc")),
}


class PlannerError(Exception):
    pass


def plan_task(transcript: str) -> dict:
    text = " ".join(transcript.lower().strip().split())
    if not text:
        raise PlannerError("Please say or type a task.")
    matches = []
    for name, (command, phrases) in COMMANDS.items():
        positions = []
        for phrase in phrases:
            for match in re.finditer(r"(?<![a-z0-9])" + re.escape(phrase) + r"(?![a-z0-9])", text):
                positions.append(match.start())
        if positions:
            matches.append((min(positions), command, name))
    if not matches:
        raise PlannerError(
            "I couldn't recognize a supported action. Try: 'open Notepad, then open Calculator'."
        )
    matches.sort(key=lambda item: item[0])
    steps = [{"command": command, "payload": {}} for _, command, _ in matches]
    if len(steps) > 8:
        raise PlannerError("A task can contain at most 8 actions.")
    summary = " then ".join(name.title() for _, _, name in matches)
    return {"summary": summary, "steps": steps}
