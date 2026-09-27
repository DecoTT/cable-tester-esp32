#include "wifi_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_mac.h>
#include "config.h"

#define AP_BASE   "CableTester32"
#define PREF_NS   "wifi"
#define PREF_SSID "ssid"
#define PREF_PASS "pass"

// SSID del portal con sufijo de MAC: "CableTester32-A1B2".
// Dos unidades NUNCA deben levantar APs con el mismo nombre — es imposible
// saber a cuál te conectaste, y con varias unidades eso pasaría seguido.
static char _ap_ssid[32] = AP_BASE;

// MAC base (interfaz STA) leída de efuse. Se usa esta y no softAPmacAddress()
// porque la del AP va +1 en el último byte: el SSID mostraría un sufijo distinto
// al que reporta link_info, y con varias unidades eso confunde al identificarlas.
// esp_read_mac() funciona en cualquier modo WiFi, incluso antes de WiFi.begin().
static void _baseMac(uint8_t* mac) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

static void _buildApSsid() {
    uint8_t mac[6];
    _baseMac(mac);
    snprintf(_ap_ssid, sizeof(_ap_ssid), "%s-%02X%02X", AP_BASE, mac[4], mac[5]);
}

static WebServer  _srv(80);
static DNSServer  _dns;
static bool       _portal_active = false;

// ── Helpers NVS ──────────────────────────────────────────────
static String _loadStr(const char* key) {
    Preferences p; p.begin(PREF_NS, true);
    String v = p.getString(key, ""); p.end(); return v;
}
static void _saveStr(const char* key, const String& val) {
    Preferences p; p.begin(PREF_NS, false);
    p.putString(key, val); p.end();
}

// ── HTML del portal captivo ───────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"(<!DOCTYPE html>
<html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>CableTester32 WiFi</title>
<style>
*{box-sizing:border-box}
body{font-family:monospace;background:#1e1e2e;color:#cdd6f4;
     display:flex;justify-content:center;padding:30px 10px}
.card{background:#181825;border:1px solid #313244;border-radius:8px;
      padding:24px;max-width:420px;width:100%}
h2{color:#89b4fa;margin:0 0 16px}
label{display:block;margin:12px 0 4px;color:#a6adc8;font-size:13px}
select,input{width:100%;background:#313244;color:#cdd6f4;
             border:1px solid #45475a;border-radius:4px;
             padding:8px;font-family:monospace;font-size:14px}
button{width:100%;margin-top:16px;padding:10px;background:#89b4fa;
       color:#1e1e2e;border:none;border-radius:4px;font-size:15px;
       font-family:monospace;cursor:pointer;font-weight:bold}
button:hover{background:#b4d0fa}
.note{color:#6c7086;font-size:12px;margin-top:10px}
</style></head>
<body><div class='card'>
<h2>⚡ CableTester32</h2>
<p style='color:#a6adc8;font-size:13px'>Conecta el tester a tu red local.</p>
<form method='POST' action='/save'>
<label>Red WiFi</label>
<select name='ssid'>{{NETWORKS}}</select>
<label>Contraseña</label>
<div style='position:relative'>
<input type='password' name='pass' id='pw' placeholder='dejar vacío si es abierta' style='padding-right:38px'>
<button type='button' onclick='var i=document.getElementById("pw");i.type=i.type==="password"?"text":"password"' style='position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:none;color:#89b4fa;cursor:pointer;font-size:16px;padding:0;line-height:1'>👁</button>
</div>
<button type='submit'>Guardar y reiniciar</button>
</form>
<p class='note'>Tras guardar el dispositivo se reinicia y se conecta a tu router.</p>
</div></body></html>)";

static const char SAVED_HTML[] PROGMEM = R"(<!DOCTYPE html>
<html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>body{font-family:monospace;background:#1e1e2e;color:#a6e3a1;
display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
.msg{text-align:center}</style></head>
<body><div class='msg'>
<h2>✅ Guardado</h2>
<p>Reiniciando y conectando a la red...</p>
<p style='color:#6c7086;font-size:13px'>Busca el dispositivo en tu router.</p>
</div></body></html>)";

// ── Handlers del portal ───────────────────────────────────────
static void _handleRoot() {
    int n = WiFi.scanNetworks();
    String opts = "";
    for (int i = 0; i < n; i++) {
        opts += "<option value='" + WiFi.SSID(i) + "'>" +
                WiFi.SSID(i) + " (" + WiFi.RSSI(i) + " dBm)</option>";
    }
    String page = FPSTR(PORTAL_HTML);
    page.replace("{{NETWORKS}}", opts);
    _srv.send(200, "text/html", page);
}

static void _handleSave() {
    String ssid = _srv.hasArg("ssid") ? _srv.arg("ssid") : "";
    String pass = _srv.hasArg("pass") ? _srv.arg("pass") : "";
    if (ssid.length()) {
        _saveStr(PREF_SSID, ssid);
        _saveStr(PREF_PASS, pass);
        _srv.send(200, "text/html", FPSTR(SAVED_HTML));
        delay(1500);
        ESP.restart();
    } else {
        _srv.send(400, "text/plain", "SSID vacio");
    }
}

static void _handleNotFound() {
    // Redirige todo al portal (captive portal)
    _srv.sendHeader("Location", "http://192.168.4.1/", true);
    _srv.send(302, "text/plain", "");
}

// ── API pública ───────────────────────────────────────────────
bool wifiBegin(uint32_t timeout_ms) {
    String ssid = _loadStr(PREF_SSID);
    String pass = _loadStr(PREF_PASS);
    if (!ssid.length()) return false;

    Serial.printf("[WiFi] Conectando a '%s'", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > timeout_ms) {
            Serial.println(" timeout.");
            return false;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());

    // mDNS con identidad por unidad. Un nombre fijo choca con varios testers
    // en la misma red. El servicio propio "_cabletester._tcp" permite que la
    // app de PC descubra SOLO testers (no cualquier dispositivo HTTP) y lea
    // sus metadatos sin conocer la IP de antemano.
    String host = "cabletester-" + wifiUnitId();
    host.toLowerCase();
    if (MDNS.begin(host.c_str())) {
        MDNS.addService("http",        "tcp", 80);
        MDNS.addService("cabletester", "tcp", 80);
        MDNS.addServiceTxt("cabletester", "tcp", "unit", wifiUnitId());
        MDNS.addServiceTxt("cabletester", "tcp", "fw",   FW_VERSION);
        MDNS.addServiceTxt("cabletester", "tcp", "api",  String(API_VERSION));
        Serial.printf("[WiFi] mDNS: http://%s.local/  (servicio _cabletester._tcp)\n",
                      host.c_str());
    }
    return true;
}

void wifiPortalBegin() {
    Serial.println("[WiFi] Sin red guardada. Levantando AP portal...");
    WiFi.mode(WIFI_AP);
    _buildApSsid();
    WiFi.softAP(_ap_ssid);
    Serial.printf("[WiFi] AP '%s' activo. IP: %s\n",
                  _ap_ssid, WiFi.softAPIP().toString().c_str());

    _dns.start(53, "*", WiFi.softAPIP());

    _srv.on("/",       HTTP_GET,  _handleRoot);
    _srv.on("/save",   HTTP_POST, _handleSave);
    _srv.onNotFound(_handleNotFound);
    _srv.begin();

    _portal_active = true;
}

void wifiPortalLoop() {
    if (!_portal_active) return;
    _dns.processNextRequest();
    _srv.handleClient();
}

bool wifiIsPortalActive() { return _portal_active; }
bool wifiIsConnected()     { return WiFi.status() == WL_CONNECTED; }
String wifiGetIP()         { return WiFi.localIP().toString(); }

String wifiGetApSsid() {
    if (!_portal_active) _buildApSsid();   // válido aunque el portal no esté activo
    return String(_ap_ssid);
}

// Identificador corto de la unidad (últimos 2 bytes de la MAC base).
// Coincide con el sufijo del SSID del portal y con la MAC que usa ESP-NOW.
String wifiUnitId() {
    uint8_t mac[6];
    _baseMac(mac);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

// --- Provisión por lote ---
// Escribe credenciales sin pasar por el portal captivo. Lo usa el flasher para
// dejar varias unidades configuradas de una pasada.
bool wifiSetCreds(const String& ssid, const String& pass) {
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) return false;
    _saveStr(PREF_SSID, ssid);
    _saveStr(PREF_PASS, pass);
    return true;
}

// Rol y SKU se conocen DESPUES de wifiBegin(), asi que se anuncian aparte.
// Con esto la app de PC filtra "el master del SKU4589" sin abrir conexion.
void wifiMdnsSetInfo(const char* role, const String& sku) {
    MDNS.addServiceTxt("cabletester", "tcp", "role", role);
    MDNS.addServiceTxt("cabletester", "tcp", "sku",  sku.length() ? sku : String("-"));
}

String wifiGetSsid() { return _loadStr(PREF_SSID); }
String wifiGetPass() { return _loadStr(PREF_PASS); }

void wifiClearCreds() {
    Preferences p; p.begin(PREF_NS, false);
    p.clear(); p.end();
    Serial.println("[WiFi] Credenciales borradas.");
}
