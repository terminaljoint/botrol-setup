# BOTROL Windows Desktop Voice Agent

## Start
1. Download `BOTROL-Voice-Agent.exe` from the BOTROL dashboard.
2. Run the executable. On first launch, enter your BOTROL admin token in the hidden prompt.
3. The token is stored in Windows Credential Manager. Keep the app running.

## Use
Press **Win + Ctrl + Alt + V** from anywhere.
Speak after the listening prompt. BOTROL automatically detects when you stop speaking and sends the captured audio for transcription. It stops after a short pause or a 15-second maximum.

Default speech language is English (India), `en-IN`. Tamil can be selected by setting `BOTROL_SPEECH_LANGUAGE=ta-IN` before launching the executable.

## Requirements
- Windows 10 or 11
- Working microphone and microphone permission enabled in Windows
- Internet connection
- BOTROL ESP32 online and paired with the computer

Speech audio is sent to Google's speech recognition service for transcription. Task planning is performed by BOTROL Cloud's built-in command parser; no AI API is used. The agent does not run shell commands or arbitrary keystrokes.

The hotkey is **Win + Ctrl + Alt + V**. If it does not trigger, another application may already own that key combination.
