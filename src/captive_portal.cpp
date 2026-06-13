#include "captive_portal.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ─────────────────────────── External references ───────────────────────────
extern String configuredSSID;
extern String configuredPass;
extern String apiKey;
extern String apiBaseUrl;
extern String friendlyId;

extern void saveWiFiSettings(const String& ssid, const String& pass);
extern void saveServerSettings(const String& key, const String& url);
extern void clearAllSettings();
extern void showSetupScreen(const String& message);
extern void goToDeepSleep(int seconds);

extern const char* FW_VERSION_STR;
extern int DEFAULT_REFRESH_RATE_VAL;
extern int WIFI_AP_TIMEOUT_VAL;
extern const char* DEFAULT_API_BASE_URL_STR;

// ─────────────────────────── Local state ───────────────────────────
static WebServer webServer(80);
static DNSServer dnsServer;
static bool portalActive = false;

static const int DNS_PORT = 53;

// ─────────────────────────── HTML ───────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>M5Paper TRMNL Setup</title>
<style>
body{font-family:-apple-system,sans-serif;margin:0;padding:20px;background:#f5f5f5}
.container{max-width:400px;margin:0 auto;background:#fff;padding:24px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,0.1)}
h1{font-size:1.4em;margin:0 0 4px;color:#333}
h2{font-size:0.9em;font-weight:normal;color:#666;margin:0 0 20px}
label{display:block;margin:12px 0 4px;font-weight:500;font-size:0.9em;color:#333}
input,select{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:1em;box-sizing:border-box}
input:focus{outline:none;border-color:#4a90d9}
button{width:100%;padding:12px;background:#333;color:#fff;border:none;border-radius:6px;font-size:1em;cursor:pointer;margin-top:16px}
button:hover{background:#555}
.section{border-top:1px solid #eee;margin-top:20px;padding-top:16px}
.info{font-size:0.8em;color:#888;margin-top:4px}
.status{padding:8px;border-radius:4px;margin-top:12px;display:none}
.success{background:#d4edda;color:#155724;display:block}
.error{background:#f8d7da;color:#721c24;display:block}
select{appearance:auto}
.btn-reset{background:#dc3545;margin-top:8px}
.btn-reset:hover{background:#c82333}
</style>
</head>
<body>
<div class="container">
<h1>M5Paper TRMNL</h1>
<h2>Device Configuration</h2>
<form id="configForm">
<label for="ssid">WiFi Network</label>
<input type="text" id="ssid" name="ssid" placeholder="Network name" required>
<label for="pass">WiFi Password</label>
<input type="password" id="pass" name="pass" placeholder="Password">
<div class="section">
<label for="server">TRMNL Server</label>
<select id="server" name="server" onchange="toggleCustomUrl()">
<option value="official">Official (trmnl.app)</option>
<option value="custom">Custom / Local Server</option>
</select>
<div id="customUrlDiv" style="display:none">
<label for="url">Server URL</label>
<input type="url" id="url" name="url" placeholder="http://192.168.1.100:8080">
<p class="info">Full base URL of your local TRMNL-compatible server</p>
</div>
</div>
<div class="section">
<label for="apikey">API Key (Access Token)</label>
<input type="text" id="apikey" name="apikey" placeholder="Optional - or use auto-registration">
<p class="info">Leave empty to use MAC-based auto-registration, or paste your TRMNL API key</p>
</div>
<button type="submit">Save & Connect</button>
</form>
<div id="status" class="status"></div>
<div class="section">
<button class="btn-reset" onclick="resetDevice()">Factory Reset</button>
<p class="info">MAC: <strong>%MAC%</strong><br>FW: %FW%<br>
Hold button >5s after wake to return to this setup.</p>
</div>
</div>
<script>
function toggleCustomUrl(){
  document.getElementById('customUrlDiv').style.display=
    document.getElementById('server').value==='custom'?'block':'none';
}
document.getElementById('configForm').addEventListener('submit',function(e){
  e.preventDefault();
  var st=document.getElementById('status');
  st.className='status';st.style.display='none';
  var data={
    ssid:document.getElementById('ssid').value,
    pass:document.getElementById('pass').value,
    url:document.getElementById('server').value==='custom'?document.getElementById('url').value:'https://trmnl.app',
    apikey:document.getElementById('apikey').value
  };
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
  .then(r=>r.json()).then(d=>{
    if(d.success){st.className='status success';st.textContent='Saved! Device will restart...';}
    else{st.className='status error';st.textContent='Error: '+d.message;}
  }).catch(()=>{st.className='status error';st.textContent='Connection error';});
});
function resetDevice(){
  if(confirm('Reset all settings? Device will restart in setup mode.')){
    fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({reset:true})}).then(()=>{location.reload();});
  }
}
</script>
</body>
</html>
)rawliteral";

// ─────────────────────────── Handlers ───────────────────────────
static void handlePortalRoot() {
  String html = String(PORTAL_HTML);
  html.replace("%MAC%", WiFi.macAddress());
  html.replace("%FW%", FW_VERSION_STR);
  webServer.send(200, "text/html", html);
}

static void handlePortalSave() {
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"No data\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  if (err) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  // Handle factory reset
  bool reset = doc["reset"] | false;
  if (reset) {
    clearAllSettings();
    webServer.send(200, "application/json", "{\"success\":true}");
    delay(500);
    ESP.restart();
    return;
  }

  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";
  String url = doc["url"] | DEFAULT_API_BASE_URL_STR;
  String key = doc["apikey"] | "";

  if (ssid.length() == 0) {
    webServer.send(400, "application/json", "{\"success\":false,\"message\":\"SSID required\"}");
    return;
  }

  // Sanitize URL - remove trailing slash
  if (url.length() > 0 && url.endsWith("/")) {
    url = url.substring(0, url.length() - 1);
  }

  saveWiFiSettings(ssid, pass);
  saveServerSettings(key, url);

  webServer.send(200, "application/json", "{\"success\":true}");

  delay(1000);
  portalActive = false;
  ESP.restart();
}

static void handlePortalStatus() {
  JsonDocument doc;
  doc["configured"] = (configuredSSID.length() > 0);
  doc["mac"] = WiFi.macAddress();
  doc["fw"] = FW_VERSION_STR;
  doc["friendly_id"] = friendlyId;

  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
}

static void handleNotFound() {
  // Redirect all unknown requests to portal (captive portal behavior)
  webServer.sendHeader("Location", "http://192.168.4.1/");
  webServer.send(302, "text/plain", "");
}

// ─────────────────────────── Public API ───────────────────────────
void startCaptivePortal() {
#ifdef DEBUG_LOGS
  Serial.println("Starting captive portal...");
#endif

  showSetupScreen("Connect to WiFi:\nM5Paper-TRMNL\nThen open: 192.168.4.1");

  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP("M5Paper-TRMNL", "");  // Open network for easy setup
  delay(100);

#ifdef DEBUG_LOGS
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
#endif

  // DNS server to redirect all domains to our IP (captive portal)
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  // Web server routes
  webServer.on("/", HTTP_GET, handlePortalRoot);
  webServer.on("/save", HTTP_POST, handlePortalSave);
  webServer.on("/status", HTTP_GET, handlePortalStatus);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  portalActive = true;

  // Run portal for up to WIFI_AP_TIMEOUT seconds
  unsigned long portalStart = millis();
  while (portalActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    delay(10);

    if (millis() - portalStart > (unsigned long)WIFI_AP_TIMEOUT_VAL * 1000) {
#ifdef DEBUG_LOGS
      Serial.println("Portal timeout - going to sleep");
#endif
      webServer.stop();
      WiFi.softAPdisconnect(true);
      goToDeepSleep(DEFAULT_REFRESH_RATE_VAL);
      return;
    }
  }
}
