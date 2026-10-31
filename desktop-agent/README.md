# BOTROL Windows Hotkey Agent

## Start
1. Download this folder from the BOTROL website.
2. Extract it.
3. Double-click `Start-BOTROL.bat`.
4. On first launch, paste your Render **BOTROL_ADMIN_TOKEN** into the hidden prompt. It is saved in Windows Credential Manager.
5. Keep the agent running in the background.

## Use
Press **Win + Alt + Space**, speak for up to 7 seconds, and BOTROL sends the recognized task to BOTROL Cloud. It reports each command's execution status.

Default speech language is English (India), `en-IN`. To use Tamil, set the environment variable `BOTROL_SPEECH_LANGUAGE=ta-IN` before starting the agent.

## Requirements
- Windows 10 or 11
- Python 3.10+
- Microphone and internet connection
- BOTROL ESP32 online and paired with the computer

Speech audio is sent to Google's speech recognition service for transcription. Task parsing is performed by BOTROL Cloud's built-in parser; no AI API is used. The agent does not run shell commands or arbitrary keystrokes.

The admin token is stored using the Windows Credential Manager through Python keyring. To remove it, open Windows Credential Manager → Windows Credentials and remove the entry named for Python keyring / BOTROL Desktop Agent.

The hotkey is registered by Windows and may fail if another application already owns Win + Alt + Space.
