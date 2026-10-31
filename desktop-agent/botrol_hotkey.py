"""BOTROL Windows voice panel using the browser's native Web Speech API."""
import getpass
import json
import os
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import keyring
import keyboard

API = "https://botrol-cloud.onrender.com"
DEVICE_ID = "BOTROL-ESP32-001"
SERVICE_NAME = "BOTROL Desktop Agent"
USERNAME = "admin-token"
LANGUAGE = os.getenv("BOTROL_SPEECH_LANGUAGE", "en-IN")
HOST = "127.0.0.1"
PORT = 8765

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BOTROL Voice Panel</title><style>
*{box-sizing:border-box}body{margin:0;background:#090d18;color:#edf3ff;font:16px system-ui;min-height:100vh;display:grid;place-items:center;padding:20px}
main{width:min(620px,100%);background:#111a2a;border:1px solid #293750;border-radius:22px;padding:28px;box-shadow:0 24px 80px #0007}
h1{margin:0;font-size:38px;letter-spacing:-.05em}h1 span{color:#aab6ff}.muted{color:#a0aec2;font-size:14px}
button,select{font:inherit;border-radius:12px;padding:13px 18px;border:1px solid #7183d8;background:#8797f4;color:#091020;font-weight:750;cursor:pointer}
button:disabled{opacity:.5;cursor:wait}.row{display:flex;gap:10px;flex-wrap:wrap;margin:20px 0}
#transcript{min-height:125px;background:#080f1d;border:1px solid #293750;border-radius:13px;padding:16px;white-space:pre-wrap;overflow-wrap:anywhere;font-size:19px}
#status{margin-top:14px;color:#a0aec2;font-size:14px;white-space:pre-wrap}.good{color:#65e0b2!important}.bad{color:#ff8585!important}
</style></head><body><main>
<p class="muted">TERMINAL JOINT / BOTROL</p><h1>Voice <span>command.</span></h1>
<p class="muted">Browser-native speech recognition. Your words appear here as you speak. No Python audio capture or fixed recording timer.</p>
<div class="row"><label class="muted">Language&nbsp; <select id="language"><option value="en-IN">English (India)</option><option value="ta-IN">Tamil</option></select></label></div>
<div class="row"><button id="listen">Start listening</button><button id="stop" disabled>Stop</button><button id="execute" disabled>Execute transcript</button></div>
<div id="transcript">Your recognized words will appear here...</div><div id="status">Ready. Click Start listening and allow microphone access.</div>
<p class="muted">Recognition is provided by your browser and may send audio to its speech service. Review the transcript before executing.</p>
<script>
const $=id=>document.getElementById(id);let recognition=null,finalText="",listening=false;
const Speech=window.SpeechRecognition||window.webkitSpeechRecognition;
if(!Speech){$("status").textContent="Speech recognition is not supported in this browser. Please use current Google Chrome or Microsoft Edge.";$("listen").disabled=true;}
function start(){if(!Speech)return;finalText="";$("transcript").textContent="Listening...";recognition=new Speech();recognition.lang=$("language").value;recognition.continuous=true;recognition.interimResults=true;recognition.maxAlternatives=1;
recognition.onstart=()=>{listening=true;$("listen").disabled=true;$("stop").disabled=false;$("status").textContent="Listening. Speak naturally, then click Stop."};
recognition.onresult=e=>{let interim="";for(let i=e.resultIndex;i<e.results.length;i++){const part=e.results[i][0].transcript;if(e.results[i].isFinal)finalText+=part+" ";else interim+=part;}
$("transcript").textContent=(finalText+interim).trim()||"Listening...";$("execute").disabled=!finalText.trim();};
recognition.onerror=e=>{$("status").textContent="Speech recognition error: "+e.error;$("status").className="bad";};
recognition.onend=()=>{listening=false;$("listen").disabled=false;$("stop").disabled=true;$("execute").disabled=!finalText.trim();if(finalText.trim()){$("status").textContent="Transcript ready. Review it, then execute.";$("status").className="good";}};
try{recognition.start()}catch(e){$("status").textContent=e.message;$("status").className="bad";}
}
$("listen").onclick=start;$("stop").onclick=()=>{if(recognition&&listening)recognition.stop()};
if(new URLSearchParams(location.search).get("listen")==="1"){setTimeout(start,900);}
$("execute").onclick=async()=>{const transcript=finalText.trim();if(!transcript)return;$("execute").disabled=true;$("status").textContent="Sending task to BOTROL Cloud...";try{const r=await fetch("/task",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({transcript})});const d=await r.json();if(!r.ok)throw new Error(d.detail||"Task request failed");$("status").textContent="Task accepted: "+d.summary+"\n"+d.steps.map((x,i)=>(i+1)+". "+x.command+" ("+x.status+")").join("\n");$("status").className="good";}catch(e){$("status").textContent="Could not execute: "+e.message;$("status").className="bad";$("execute").disabled=false;}};
</script></main></body></html>"""

def get_token():
    token = keyring.get_password(SERVICE_NAME, USERNAME)
    if token:
        return token
    print("\nFirst-time setup: enter your BOTROL_ADMIN_TOKEN.")
    print("It will be stored in Windows Credential Manager.")
    token = getpass.getpass("Admin token: ").strip()
    if not token:
        raise RuntimeError("No admin token entered.")
    keyring.set_password(SERVICE_NAME, USERNAME, token)
    print("Token saved securely in Windows Credential Manager.")
    return token

def submit_task(token, transcript):
    payload = json.dumps({"device_id": DEVICE_ID, "transcript": transcript}).encode()
    request = Request(API + "/api/v1/admin/tasks/execute", data=payload,
                      headers={"Authorization": "Bearer " + token,
                               "Content-Type": "application/json"}, method="POST")
    try:
        with urlopen(request, timeout=35) as response:
            return json.loads(response.read().decode())
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"BOTROL Cloud HTTP {exc.code}: {detail}") from exc
    except URLError as exc:
        raise RuntimeError(f"Cannot reach BOTROL Cloud: {exc.reason}") from exc

def main():
    token = get_token()
    with urlopen(API + "/health", timeout=20) as response:
        if response.status != 200:
            raise RuntimeError("BOTROL Cloud health check failed.")

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/":
                self.send_error(404); return
            body = PAGE.encode("utf-8")
            self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
        def do_POST(self):
            if self.path != "/task":
                self.send_error(404); return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                data = json.loads(self.rfile.read(length))
                transcript = str(data.get("transcript", "")).strip()
                if not transcript or len(transcript) > 2000:
                    raise ValueError("Transcript must contain 1 to 2000 characters.")
                result = submit_task(token, transcript)
                payload = {"ok": True, "summary": result["task"]["summary"],
                           "steps": result["task"]["steps"]}
                status = 200
            except Exception as exc:
                payload = {"detail": str(exc)}; status = 400
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
        def log_message(self, fmt, *args):
            print("[BOTROL panel] " + fmt % args)

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    url = f"http://{HOST}:{PORT}/?listen=1"
    print("\nBOTROL is ready. Press WIN + J anywhere to start voice input.")
    print("Speech recognition: browser Web Speech API")
    print("Language: choose Tamil or English in the voice panel.")
    print("First use: allow microphone access in Chrome/Edge. Keep this window open.")

    # keyboard.add_hotkey can fire repeatedly while a key combination is held
    # or repeated by the OS. Debounce launches so one press opens one panel.
    launch_lock = threading.Lock()
    last_launch = 0.0

    def open_voice_panel():
        nonlocal last_launch
        with launch_lock:
            now = time.monotonic()
            if now - last_launch < 2.0:
                return
            last_launch = now
        print("[BOTROL] Win + J pressed. Opening voice panel...")
        webbrowser.open(url, new=0)

    keyboard.add_hotkey("windows+j", open_voice_panel, suppress=False, trigger_on_release=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nBOTROL stopped.")
    finally:
        keyboard.unhook_all()
        server.shutdown()
        server.server_close()

if __name__ == "__main__":
    main()
