# BOTROL Desktop Agent

Small Python agent that connects a computer to BOTROL Cloud.

## Current safe actions

- `open_notepad`: opens Notepad on Windows, TextEdit on macOS, or an installed text editor on Linux.
- `open_calculator`: opens Calculator on Windows/macOS or an installed calculator on Linux.

The agent does not accept shell commands, arbitrary paths, file contents, keystrokes, or arbitrary scripts. It uses argument-list process launches without a shell.

## Setup on Windows

1. Download the repository ZIP from GitHub and extract it.
2. Open the `desktop-agent` folder.
3. Run `Start-BOTROL-Desktop.bat`. On first run it creates `config.json` and opens it in Notepad.
4. In Render, open the `botrol-cloud` service → Environment and copy the value of `BOTROL_DESKTOP_TOKEN`.
5. Paste that value into `config.json` as the `desktop_token` value, save, then run the BAT file again.

Python 3 must be installed and available as `py -3`. No third-party Python packages are required.

## Dashboard

Open https://botrol-setup.vercel.app/dashboard.html, enter the BOTROL admin token, then use the desktop action buttons. Keep the desktop agent running.

## Security and prototype limitations

- Keep `config.json` private. It is excluded by `.gitignore`; never commit or share it.
- Keep the admin token private. It is entered into the dashboard for each request and cleared after submission.
- The desktop token is separate from the ESP32 device token and admin token.
- Only the two allowlisted launch actions are supported.
- Commands are delivered through an in-memory cloud queue. Render restarts or sleeps can clear queued commands.
- This is an early prototype, not a hardened remote administration product. Do not expose arbitrary command execution.
