# BOTROL Windows Voice Agent

## Start
1. Download `BOTROL-Voice-Agent.exe` from the BOTROL dashboard.
2. Run it. On first launch, enter your BOTROL admin token.
3. The token is stored in Windows Credential Manager.
4. Keep the console window running.

## Use
1. Press **Win + J** anywhere in Windows.
2. BOTROL opens its local browser voice panel and starts listening automatically.
3. Choose English (India) or Tamil in the panel. Allow microphone access on first use.
4. Your transcript appears live as you speak.
5. Click **Stop**, review the transcript, then click **Execute transcript**.

Audio capture and speech recognition are handled by the browser's Web Speech API, avoiding the previous Python microphone driver and fixed-duration recording issues. Use current Google Chrome or Microsoft Edge. Browser speech recognition may send audio to its speech service.

The desktop agent sends the transcript to BOTROL Cloud. BOTROL's current deterministic command parser supports only implemented commands, including ping, open Notepad, and open Calculator. It does not use an AI API or run arbitrary shell commands.

## Requirements
- Windows 10 or 11
- Current Google Chrome or Microsoft Edge
- Working microphone and browser microphone permission
- Internet connection
- BOTROL ESP32 online and paired with the computer

Close the console window or press Ctrl+C to stop BOTROL.
