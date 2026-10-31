# BOTROL Windows Voice Panel

## Start
1. Download `BOTROL-Voice-Agent.exe` from the BOTROL dashboard.
2. Run it. On first launch, enter your BOTROL admin token.
3. The token is stored in Windows Credential Manager.
4. BOTROL opens a local browser voice panel at `http://127.0.0.1:8765`.

## Use
1. Choose English (India) or Tamil.
2. Click **Start listening** and allow microphone access.
3. Speak naturally. The transcript appears live as you speak.
4. Click **Stop**, review the recognized transcript, then click **Execute transcript**.

Audio capture and speech recognition are handled by the browser's Web Speech API, avoiding the previous Python microphone driver and fixed-duration recording issues. Use current Google Chrome or Microsoft Edge. Browser speech recognition may send audio to its speech service.

The desktop agent sends the transcript to BOTROL Cloud. BOTROL's current deterministic command parser supports only its implemented commands, including ping, open Notepad, and open Calculator. It does not use an AI API or run arbitrary shell commands.

## Requirements
- Windows 10 or 11
- Current Google Chrome or Microsoft Edge
- Working microphone and browser microphone permission
- Internet connection
- BOTROL ESP32 online and paired with the computer

Keep the executable's console window open while using the voice panel. Close it or press Ctrl+C in the console to stop the local service.
