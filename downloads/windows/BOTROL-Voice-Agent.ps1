# BOTROL Windows Voice Hotkey Agent
# Hotkey: Win + Ctrl + Alt + B
# Requires Windows PowerShell 5.1, microphone, and installed Windows speech recognition.
$ErrorActionPreference = "Stop"
$ApiBase = "https://botrol-cloud.onrender.com"
$DeviceId = "BOTROL-ESP32-001"
$ConfigDir = Join-Path $env:LOCALAPPDATA "BOTROL"
$TokenFile = Join-Path $ConfigDir "admin-token.dpapi"
Add-Type -AssemblyName System.Speech
if (-not ("BotrolHotkey.Native" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
namespace BotrolHotkey {
 public static class Native {
  [DllImport("user32.dll", SetLastError=true)] public static extern bool RegisterHotKey(IntPtr hWnd,int id,uint modifiers,uint vk);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool UnregisterHotKey(IntPtr hWnd,int id);
  [DllImport("user32.dll")] public static extern int GetMessage(out MSG msg,IntPtr hWnd,uint min,uint max);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x; public int y; }
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public UIntPtr wParam; public IntPtr lParam; public uint time; public POINT pt; }
 }
}
"@
}
function Save-BotrolToken {
 New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
 $secure=Read-Host "Paste BOTROL_ADMIN_TOKEN (input is hidden)" -AsSecureString
 if($secure.Length -lt 12){throw "Token looks too short."}
 $secure | ConvertFrom-SecureString | Set-Content -Encoding ASCII $TokenFile
 Write-Host "Token saved encrypted for this Windows user." -ForegroundColor Green
}
function Get-BotrolToken {
 if(-not(Test-Path $TokenFile)){Write-Host "First-time setup: configure your admin token." -ForegroundColor Cyan;Save-BotrolToken}
 $secure=Get-Content -Raw $TokenFile | ConvertTo-SecureString
 $ptr=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
 try{[Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)}
 finally{[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)}
}
function Invoke-BotrolVoiceTask {
 Write-Host ""
 Write-Host "Listening... speak your command." -ForegroundColor Cyan
 $recognizer=New-Object System.Speech.Recognition.SpeechRecognitionEngine
 try{
  $recognizer.SetInputToDefaultAudioDevice()
  $recognizer.LoadGrammar((New-Object System.Speech.Recognition.DictationGrammar))
  $recognizer.BabbleTimeout=[TimeSpan]::FromSeconds(4)
  $recognizer.InitialSilenceTimeout=[TimeSpan]::FromSeconds(7)
  $recognizer.EndSilenceTimeout=[TimeSpan]::FromSeconds(1.2)
  $result=$recognizer.Recognize([TimeSpan]::FromSeconds(15))
  if($null -eq $result -or [string]::IsNullOrWhiteSpace($result.Text)){Write-Host "No speech recognized." -ForegroundColor Yellow;return}
  $transcript=$result.Text.Trim()
  Write-Host "Heard: $transcript" -ForegroundColor White
  $token=Get-BotrolToken
  $headers=@{Authorization="Bearer $token"}
  $body=@{device_id=$DeviceId;transcript=$transcript}|ConvertTo-Json -Depth 5
  $created=Invoke-RestMethod -Method Post -Uri "$ApiBase/api/v1/admin/tasks/execute" -Headers $headers -ContentType "application/json" -Body $body -TimeoutSec 45
  $task=$created.task
  if(-not $task -or -not $task.task_id){throw "Cloud did not return a task ID."}
  Write-Host "Task: $($task.summary)" -ForegroundColor Cyan
  foreach($step in $task.steps){Write-Host " QUEUED $($step.command)"}
  $deadline=(Get-Date).AddSeconds(90)
  do{
   Start-Sleep -Milliseconds 1500
   $status=Invoke-RestMethod -Method Get -Uri "$ApiBase/api/v1/admin/tasks/$($task.task_id)" -Headers $headers -TimeoutSec 20
   $finished=@($status.steps|Where-Object{$_.status -notin @("completed","failed")}).Count -eq 0
  }while(-not $finished -and (Get-Date) -lt $deadline)
  foreach($step in $status.steps){
   $color=if($step.status -eq "completed"){"Green"}elseif($step.status -eq "failed"){"Red"}else{"Yellow"}
   Write-Host (" {0,-10} {1} {2}" -f $step.status.ToUpper(),$step.command,$step.result) -ForegroundColor $color
  }
  if(-not $finished){Write-Host "Still pending. Check ESP32 power, Wi-Fi and Bluetooth." -ForegroundColor Yellow}
 }catch{Write-Host "BOTROL error: $($_.Exception.Message)" -ForegroundColor Red}
 finally{if($recognizer){$recognizer.Dispose()}}
}
if($args -contains "--configure"){Save-BotrolToken;exit}
if($args -contains "--forget-token"){if(Test-Path $TokenFile){Remove-Item $TokenFile -Force};Write-Host "Saved token removed.";exit}
$MOD_ALT=0x0001;$MOD_CONTROL=0x0002;$MOD_WIN=0x0008;$VK_B=0x42;$HOTKEY_ID=0xB071
Write-Host ""
Write-Host "BOTROL VOICE AGENT" -ForegroundColor Cyan
Write-Host "Hotkey: WIN + CTRL + ALT + B" -ForegroundColor White
Write-Host "Press it anywhere to speak. Ctrl + C exits. Keep this window running." -ForegroundColor Gray
$null=Get-BotrolToken
if(-not [BotrolHotkey.Native]::RegisterHotKey([IntPtr]::Zero,$HOTKEY_ID,$MOD_WIN -bor $MOD_ALT -bor $MOD_CONTROL,$VK_B)){
 $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
 throw "Could not register Win + Ctrl + Alt + B (Windows error $code). Another app may use it."
}
Write-Host "Ready. Waiting for Win + Alt + B..." -ForegroundColor Green
try{
 while($true){
  $message=New-Object BotrolHotkey.Native+MSG
  $result=[BotrolHotkey.Native]::GetMessage([ref]$message,[IntPtr]::Zero,0,0)
  if($result -le 0){break}
  if($message.message -eq 0x0312 -and $message.wParam.ToUInt32() -eq $HOTKEY_ID){
   Invoke-BotrolVoiceTask
   Write-Host ""
   Write-Host "Ready again. Press Win + Alt + B to speak." -ForegroundColor Green
  }
 }
}finally{[BotrolHotkey.Native]::UnregisterHotKey([IntPtr]::Zero,$HOTKEY_ID)|Out-Null}
