using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Speech.Recognition;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

internal static class BotrolAgent
{
    const string Api = "https://botrol-cloud.onrender.com";
    const string DeviceId = "BOTROL-ESP32-001";
    const int HotkeyId = 0xB071;
    const uint MOD_ALT = 0x0001, MOD_CONTROL = 0x0002, MOD_WIN = 0x0008, VK_B = 0x42;
    static readonly string ConfigDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BOTROL");
    static readonly string TokenPath = Path.Combine(ConfigDir, "admin-token.dpapi");
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer();

    [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public UIntPtr wParam; public IntPtr lParam; public uint time; public POINT pt; }
    [DllImport("user32.dll", SetLastError=true)] static extern bool RegisterHotKey(IntPtr hwnd, int id, uint modifiers, uint key);
    [DllImport("user32.dll", SetLastError=true)] static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    [DllImport("user32.dll", SetLastError=true)] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);

    static void Main(string[] args)
    {
        Console.Title = "BOTROL Voice Agent";
        try
        {
            if (Array.IndexOf(args, "--forget-token") >= 0) {
                if (File.Exists(TokenPath)) File.Delete(TokenPath);
                Console.WriteLine("Saved token removed."); return;
            }
            string token = LoadOrAskToken();
            Console.WriteLine("\nBOTROL VOICE AGENT");
            Console.WriteLine("Hotkey: WIN + CTRL + ALT + B");
            Console.WriteLine("Press the hotkey anywhere to speak. Ctrl+C exits.");
            if (!RegisterHotKey(IntPtr.Zero, HotkeyId, MOD_WIN | MOD_CONTROL | MOD_ALT, VK_B))
                throw new Exception("Could not register hotkey. Windows error " + Marshal.GetLastWin32Error());
            Console.WriteLine("Ready. Waiting for your command...");
            try {
                MSG msg;
                while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) {
                    if (msg.message == 0x0312 && msg.wParam.ToUInt32() == HotkeyId)
                        RunVoiceTask(token);
                }
            } finally { UnregisterHotKey(IntPtr.Zero, HotkeyId); }
        }
        catch (Exception ex) { Console.ForegroundColor=ConsoleColor.Red; Console.WriteLine("BOTROL error: "+ex.Message); Console.ResetColor(); }
        Console.WriteLine("\nPress Enter to close."); Console.ReadLine();
    }

    static string LoadOrAskToken()
    {
        Directory.CreateDirectory(ConfigDir);
        if (!File.Exists(TokenPath)) {
            Console.Write("Enter BOTROL admin token (hidden): ");
            string value = ReadHidden();
            if (value.Length < 12) throw new Exception("Token appears too short.");
            byte[] encrypted = ProtectedData.Protect(Encoding.UTF8.GetBytes(value), null, DataProtectionScope.CurrentUser);
            File.WriteAllBytes(TokenPath, encrypted);
            Console.WriteLine("Token saved encrypted for this Windows account.");
        }
        byte[] bytes = ProtectedData.Unprotect(File.ReadAllBytes(TokenPath), null, DataProtectionScope.CurrentUser);
        return Encoding.UTF8.GetString(bytes);
    }

    static string ReadHidden()
    {
        var b = new StringBuilder();
        ConsoleKeyInfo k;
        while ((k = Console.ReadKey(true)).Key != ConsoleKey.Enter) {
            if (k.Key == ConsoleKey.Backspace && b.Length > 0) b.Length--;
            else if (!char.IsControl(k.KeyChar)) b.Append(k.KeyChar);
        }
        Console.WriteLine(); return b.ToString();
    }

    static void RunVoiceTask(string token)
    {
        Console.WriteLine("\nListening... speak now.");
        try {
            using (var recognizer = new SpeechRecognitionEngine()) {
                recognizer.SetInputToDefaultAudioDevice();
                recognizer.LoadGrammar(new DictationGrammar());
                recognizer.BabbleTimeout = TimeSpan.FromSeconds(4);
                recognizer.InitialSilenceTimeout = TimeSpan.FromSeconds(7);
                recognizer.EndSilenceTimeout = TimeSpan.FromSeconds(1.2);
                RecognitionResult result = recognizer.Recognize(TimeSpan.FromSeconds(15));
                if (result == null || String.IsNullOrWhiteSpace(result.Text)) { Console.WriteLine("No speech recognized."); return; }
                Console.WriteLine("Heard: " + result.Text);
                ExecuteTask(token, result.Text.Trim());
            }
        }
        catch (Exception ex) { Console.WriteLine("Voice/task error: " + ex.Message); }
        Console.WriteLine("\nReady. Press Win + Ctrl + Alt + B again.");
    }

    static Dictionary<string, object> Request(string method, string path, string token, string body = null)
    {
        var req = (HttpWebRequest)WebRequest.Create(Api + path);
        req.Method = method; req.Timeout = 45000;
        req.Headers[HttpRequestHeader.Authorization] = "Bearer " + token;
        if (body != null) {
            byte[] data = Encoding.UTF8.GetBytes(body);
            req.ContentType = "application/json"; req.ContentLength = data.Length;
            using (Stream s = req.GetRequestStream()) s.Write(data, 0, data.Length);
        }
        using (var response = (HttpWebResponse)req.GetResponse())
        using (var reader = new StreamReader(response.GetResponseStream()))
            return Json.Deserialize<Dictionary<string, object>>(reader.ReadToEnd());
    }

    static void ExecuteTask(string token, string transcript)
    {
        var payload = Json.Serialize(new Dictionary<string, object> { { "device_id", DeviceId }, { "transcript", transcript } });
        var created = Request("POST", "/api/v1/admin/tasks/execute", token, payload);
        var task = (Dictionary<string, object>)created["task"];
        string id = Convert.ToString(task["task_id"]);
        Console.WriteLine("Task: " + Convert.ToString(task["summary"]));
        DateTime deadline = DateTime.UtcNow.AddSeconds(90);
        while (DateTime.UtcNow < deadline) {
            Thread.Sleep(1500);
            var status = Request("GET", "/api/v1/admin/tasks/" + Uri.EscapeDataString(id), token);
            var steps = (object[])status["steps"];
            bool finished = true;
            foreach (Dictionary<string, object> step in steps) {
                string state = Convert.ToString(step["status"]);
                if (state != "completed" && state != "failed") finished = false;
            }
            if (finished) {
                foreach (Dictionary<string, object> step in steps)
                    Console.WriteLine("{0,-10} {1} {2}", Convert.ToString(step["status"]).ToUpperInvariant(), step["command"], step.ContainsKey("result") ? step["result"] : "");
                return;
            }
        }
        Console.WriteLine("Task is still pending. Check ESP32 power, Wi-Fi and Bluetooth.");
    }
}