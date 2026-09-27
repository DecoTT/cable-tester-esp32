#include "web_cmd.h"
#include "raw.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>

// NOTA: wifi_portal.cpp tiene su propio WebServer(80). No chocan porque son
// mutuamente excluyentes (portal AP vs modo STA conectado). Cualquier ruta
// nueva —incluido OTA— debe colgarse de ESTE servidor, no de uno nuevo.
static WebServer _web(80);

// Print que captura a String — para redirigir salida de comandos al web
class StringPrint : public Print {
public:
    String buf;
    size_t write(uint8_t c)                       override { buf += (char)c; return 1; }
    size_t write(const uint8_t* b, size_t n)      override {
        for (size_t i = 0; i < n; i++) buf += (char)b[i];
        return n;
    }
};

// ── HTML de la interfaz de comandos ──────────────────────────
static const char WEB_HTML[] PROGMEM = R"(<!DOCTYPE html>
<html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>CableTester32</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1e1e2e;color:#cdd6f4;
     display:flex;flex-direction:column;height:100vh;padding:12px;gap:8px}
h2{color:#89b4fa;font-size:16px}
#out{flex:1;background:#181825;border:1px solid #313244;border-radius:6px;
     padding:10px;overflow-y:auto;white-space:pre-wrap;font-size:13px;
     line-height:1.5}
.rx{color:#a6e3a1} .tx{color:#89b4fa} .sys{color:#6c7086}
.err{color:#f38ba8}
.bar{display:flex;gap:6px}
#cmd{flex:1;background:#313244;color:#cdd6f4;border:1px solid #45475a;
     border-radius:4px;padding:7px 10px;font-family:monospace;font-size:14px}
#cmd:focus{outline:none;border-color:#89b4fa}
.btn{background:#89b4fa;color:#1e1e2e;border:none;border-radius:4px;
     padding:7px 14px;cursor:pointer;font-family:monospace;font-weight:bold}
.btn:hover{background:#b4d0fa}
.quick{display:flex;flex-wrap:wrap;gap:4px}
.q{background:#313244;color:#cdd6f4;border:1px solid #45475a;
   border-radius:4px;padding:4px 10px;cursor:pointer;font-family:monospace;
   font-size:12px}
.q:hover{border-color:#89b4fa;color:#89b4fa}
.q.go{background:#a6e3a1;color:#1e1e2e;border-color:#a6e3a1;font-weight:bold}
.q.go:hover{background:#c6f3c1;color:#1e1e2e}
.q.pair{background:#f9e2af;color:#1e1e2e;border-color:#f9e2af;font-weight:bold}
.q.pair:hover{background:#fbeccb;color:#1e1e2e}
.info{color:#6c7086;font-size:11px}
</style></head>
<body>
<h2>⚡ Cable Tester 32CH &nbsp;<span class='info' id='ip'></span></h2>
<div id='out'></div>
<div class='quick'>
<button class='q' onclick='run("help")'>help</button>
<button class='q' onclick='run("scan_i2c")'>scan_i2c</button>
<button class='q' onclick='run("led_test")'>led_test</button>
<button class='q' onclick='run("expander_test")'>expander_test</button>
<button class='q' onclick='run("all_input")'>all_input</button>
<button class='q' onclick='run("read_all")'>read_all</button>
<button class='q' onclick='run("sr_dump")'>sr_dump</button>
<button class='q' onclick='run("sysinfo")'>sysinfo</button>
<button class='q' onclick='run("wifi_info")'>wifi_info</button>
<button class='q' onclick='run("wifi_reset")'>wifi_reset</button>
</div>
<div class='quick'>
<button class='q go' onclick='run("selftest")'>▶ SELFTEST (ambos sentidos)</button>
<button class='q go' onclick='run("rtest")'>▶ RTEST (resistencia)</button>
<button class='q go' onclick='run("leaktest")'>▶ LEAKTEST (pines a GND)</button>
</div>
<div class='quick'>
<button class='q go' onclick='run("api_test")'>▶▶ PRUEBA (JSON, como la PC)</button>
<button class='q pair' onclick='run("map")'>🗺 MAPEAR arnés</button>
<button class='q pair' onclick='run("map_check")'>✔ VERIFICAR vs golden</button>
<button class='q' onclick='if(confirm("Guardar el arnés conectado como GOLDEN de referencia?"))run("map_save "+(prompt("Nombre del job:","golden")||"golden"))'>💾 Guardar golden</button>
<button class='q' onclick='run("map_golden")'>map_golden</button>
<button class='q' onclick='run("map_json")'>map_json</button>
<button class='q' onclick='run("loopback16")'>loopback16 (solo ida)</button>
<button class='q' onclick='document.getElementById("ota").click()'>⬆ Firmware OTA</button>
<input type='file' id='ota' accept='.bin' style='display:none' onchange='upload(this)'>
</div>
<div class='quick'>
<button class='q pair' onclick='run("link_pair")'>🔗 EMPAREJAR 2a unidad</button>
<button class='q' onclick='run("link_info")'>link_info</button>
<button class='q' onclick='run("link_ping")'>link_ping</button>
<button class='q' onclick='run("link_push_wifi")'>Reenviar WiFi al peer</button>
<button class='q' onclick='run("link_reset")'>link_reset</button>
</div>
<div class='quick'>
<button class='q' onclick='run("api_last")'>api_last</button>
<button class='q' onclick='run("leds 0 1023")'>LEDs todos</button>
<button class='q' onclick='run("leds 0 31")'>LEDs rojos</button>
<button class='q' onclick='run("leds 0 992")'>LEDs verdes</button>
<button class='q' onclick='run("leds off")'>LEDs off</button>
<button class='q' onclick='run("led_mode")'>led_mode</button>
<button class='q' onclick='run("trigger")'>trigger (ver)</button>
<button class='q' onclick='var p=prompt("Pin del switch (4,5,6,7,10) o off:","10");if(p)run("trigger "+p)'>trigger (fijar)</button>
</div>
<div class='bar'>
<input id='cmd' placeholder='Escribe un comando...'
       onkeydown='if(event.key==="Enter")send()'>
<button class='btn' onclick='send()'>Enviar</button>
</div>
<script>
var out=document.getElementById('out');
var hist=[],hi=-1;
document.getElementById('ip').textContent=location.host;

function append(txt,cls){
  var d=document.createElement('div');
  d.className=cls||'';d.textContent=txt;
  out.appendChild(d);out.scrollTop=out.scrollHeight;
}

function run(c){document.getElementById('cmd').value=c;send();}

function send(){
  var inp=document.getElementById('cmd');
  var c=inp.value.trim();if(!c)return;
  hist.unshift(c);hi=-1;inp.value='';
  append('> '+c,'tx');
  fetch('/cmd',{method:'POST',
    headers:{'Content-Type':'text/plain'},body:c})
    .then(r=>r.text())
    .then(t=>{if(t)append(t,'rx');})
    .catch(e=>append('Error: '+e,'err'));
}

function upload(el){
  var f=el.files[0];if(!f)return;
  append('Subiendo firmware: '+f.name+' ('+f.size+' bytes)...','sys');
  var fd=new FormData();fd.append('firmware',f,f.name);
  var x=new XMLHttpRequest();
  x.open('POST','/update');
  x.upload.onprogress=function(e){
    if(e.lengthComputable)
      append('  '+Math.round(e.loaded*100/e.total)+'%','sys');
  };
  x.onload=function(){
    append(x.responseText,x.status===200?'rx':'err');
    if(x.status===200)append('Reiniciando... recarga la pagina en ~10s.','sys');
  };
  x.onerror=function(){append('Error de red durante OTA','err');};
  x.send(fd);
  el.value='';
}

document.getElementById('cmd').addEventListener('keydown',function(e){
  if(e.key==='ArrowUp'){hi=Math.min(hi+1,hist.length-1);this.value=hist[hi]||'';}
  if(e.key==='ArrowDown'){hi=Math.max(hi-1,-1);this.value=hi<0?'':hist[hi];}
});

append('Conectado a CableTester32. Escribe "help" para ver comandos.','sys');
</script>
</body></html>)";

// ── Handlers HTTP ─────────────────────────────────────────────
static void _handleRoot() {
    _web.send(200, "text/html", FPSTR(WEB_HTML));
}

// POST /api/raw — primitivas crudas, para que la PC orqueste el barrido.
//
// Cuerpo: una operacion por linea, mismo formato que el comando 'raw'.
//   io_dir 0 0xFFFE
//   sample 0 1
//
// Respuesta: JSON versionado. La PC NO debe parsear texto de /cmd; esta ruta
// existe para que no tenga que hacerlo.
//   {"api":1,"n":2,"results":[0,12345]}
//
// Las ops se ejecutan EN ORDEN y sin ceder el control: una secuencia
// dir -> wr -> rd partida a la mitad no mide nada. Por eso van en una sola
// peticion y no como llamadas sueltas.
static void _handleApiRaw() {
    if (!_web.hasArg("plain")) {
        _web.send(400, "application/json", "{\"api\":1,\"error\":\"sin cuerpo\"}");
        return;
    }
    String body = _web.arg("plain");

    RawOpCode ops[RAW_MAX_OPS];
    uint8_t n = 0;
    String err;
    int start = 0;
    while (start < (int)body.length() && n < RAW_MAX_OPS) {
        int nl = body.indexOf('\n', start);
        if (nl < 0) nl = body.length();
        String line = body.substring(start, nl);
        line.trim();
        start = nl + 1;
        if (!line.length() || line[0] == '#') continue;
        if (!rawParse(line, ops[n], err)) {
            String j = String("{\"api\":1,\"error\":\"") + err + "\",\"linea\":" + n + "}";
            _web.send(400, "application/json", j);
            return;
        }
        n++;
    }

    int32_t res[RAW_MAX_OPS];
    bool    ok[RAW_MAX_OPS];
    for (uint8_t i = 0; i < RAW_MAX_OPS; i++) ok[i] = true;
    uint8_t done = rawExec(ops, n, res, ok);

    String j = "{\"api\":1,\"n\":" + String(done) + ",\"results\":[";
    for (uint8_t i = 0; i < done; i++) {
        if (i) j += ",";
        // OJO: String(INT32_MIN) devuelve "-" en Arduino (desborda al negar el
        // valor minimo). Se formatea con snprintf, que si lo maneja.
        if (!ok[i]) { j += "null"; continue; }
        char nb[16];
        snprintf(nb, sizeof(nb), "%ld", (long)res[i]);
        j += nb;
    }
    j += "]}";
    _web.send(200, "application/json", j);
}

static void _handleCmd() {
    if (!_web.hasArg("plain")) {
        _web.send(400, "text/plain", "sin body");
        return;
    }
    String cmd = _web.arg("plain");
    cmd.trim();

    StringPrint sp;
    handleCommand(cmd, sp);

    _web.send(200, "text/plain; charset=utf-8", sp.buf);
}

static void _handleNotFound() {
    _web.sendHeader("Location", "/", true);
    _web.send(302, "text/plain", "");
}

// ── API de maquina ────────────────────────────────────────────
// Para el software de produccion. Estable y versionado: los mensajes de
// texto de /cmd son para humanos y NO deben parsearse desde un programa.
static void _handleApiStatus() {
    StringPrint sp;
    apiStatusJson(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

static void _handleApiGoldenGet() {
    StringPrint sp;
    apiGoldenGetJson(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

// POST /api/test — puede tardar segundos en distribuido (96 canales).
static void _handleApiTest() {
    StringPrint sp;
    apiRunTest(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

static void _handleApiLast() {
    StringPrint sp;
    apiLastTestJson(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

static void _handleApiSelfCheck() {
    StringPrint sp;
    apiSelfCheckJson(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

static void _handleApiMap() {
    StringPrint sp;
    apiMapJson(sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

static void _handleApiGoldenPost() {
    StringPrint sp;
    if (!_web.hasArg("plain")) {
        _web.sendHeader("Access-Control-Allow-Origin", "*");
        _web.send(400, "application/json", "{\"ok\":false,\"error\":\"sin cuerpo\"}");
        return;
    }
    apiGoldenSet(_web.arg("plain"), sp);
    _web.sendHeader("Access-Control-Allow-Origin", "*");
    // 200 aunque el contenido diga ok:false — el error va en el cuerpo,
    // asi el cliente siempre parsea la misma forma.
    _web.send(200, "application/json; charset=utf-8", sp.buf);
}

// ── OTA: subida de firmware por HTTP ──────────────────────────
// Respuesta final (se envía cuando la subida terminó)
static void _handleUpdateDone() {
    bool ok = !Update.hasError();
    _web.sendHeader("Connection", "close");
    _web.send(ok ? 200 : 500, "text/plain",
              ok ? "OTA OK — reiniciando." : "OTA FALLO — firmware sin cambios.");
    if (ok) {
        Serial.println(F("[OTA]  Actualizacion completa. Reiniciando..."));
        delay(800);
        ESP.restart();
    }
}

// Handler de streaming: se llama por cada bloque recibido
static void _handleUpdateUpload() {
    HTTPUpload& up = _web.upload();

    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA]  Recibiendo '%s'\n", up.filename.c_str());
        // UPDATE_SIZE_UNKNOWN: el tamaño se descubre al cerrar el stream
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA]  %u bytes escritos OK\n", up.totalSize);
        } else {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println(F("[OTA]  Abortado."));
    }
}

// ── API pública ───────────────────────────────────────────────
void webCmdBegin() {
    _web.on("/",     HTTP_GET,  _handleRoot);
    _web.on("/cmd",  HTTP_POST, _handleCmd);
    _web.on("/api/status", HTTP_GET,  _handleApiStatus);
    _web.on("/api/golden", HTTP_GET,  _handleApiGoldenGet);
    _web.on("/api/golden", HTTP_POST, _handleApiGoldenPost);
    _web.on("/api/test",   HTTP_POST, _handleApiTest);
    _web.on("/api/test",   HTTP_GET,  _handleApiTest);
    _web.on("/api/test/last", HTTP_GET, _handleApiLast);
    _web.on("/api/selfcheck", HTTP_GET, _handleApiSelfCheck);
    _web.on("/api/map",       HTTP_GET, _handleApiMap);
    _web.on("/api/raw",       HTTP_POST, _handleApiRaw);
    _web.on("/update", HTTP_POST, _handleUpdateDone, _handleUpdateUpload);
    _web.onNotFound(_handleNotFound);
    _web.begin();
    Serial.printf("[Web]  Interfaz disponible en http://%s/\n",
                  WiFi.localIP().toString().c_str());
}

void webCmdLoop() {
    _web.handleClient();
}
