// =============================================================
// bring_up.ino — Cable Tester 32CH
// Firmware: v0.7.2 — Mapeo distribuido con reintentos y numero de secuencia
// Target:   ESP32-C3-MINI-1-N4 en módulo SBC Rev1.1
// Board:    "ESP32C3 Dev Module"  (CDCOnBoot: default)
//
// GPIO CONFIRMADOS desde esquemático EasyEDA Rev 1.0 (2026-03-02)
// Bits del shift register y direcciones I2C VERIFICADOS en PCB (2026-08-03)
// =============================================================

#include <Wire.h>
#include <WiFi.h>
#include "config.h"
#include "tca9535.h"
#include "shift595.h"
#include "wifi_portal.h"
#include "web_cmd.h"
#include "selftest.h"
#include "link.h"
#include "mapping.h"
#include "mapdist.h"
#include "raw.h"
#include <Preferences.h>

// --- Instancias de hardware ---
TCA9535  u1(TCA9535_U1_ADDR);
TCA9535  u2(TCA9535_U2_ADDR);
Shift595 sr(PIN_595_SER, PIN_595_SRCLK, PIN_595_RCLK);

bool u1_ok = false;
bool u2_ok = false;

// ── Blink IP en LEDs (último octeto) ─────────────────────────
// Cada dígito: N parpadeos rápidos, pausa entre dígitos, pausa larga entre grupos
static void blinkDigit(uint8_t n) {
    if (n == 0) {
        // cero = pulso largo
        sr.setAllLeds(true); delay(600); sr.setAllLeds(false); delay(300);
        return;
    }
    for (uint8_t i = 0; i < n; i++) {
        sr.setAllLeds(true); delay(180);
        sr.setAllLeds(false); delay(180);
    }
}

static void showIPonLeds() {
    if (!wifiIsConnected()) {
        // No conectado: blink rápido 5x
        for (int i = 0; i < 5; i++) {
            sr.setAllLeds(true); delay(80); sr.setAllLeds(false); delay(80);
        }
        return;
    }
    IPAddress ip = WiFi.localIP();
    Serial.printf("[BTN]  IP: %s\n", ip.toString().c_str());

    // Parpadear los 4 octetos separados por pausa larga
    for (int oct = 0; oct < 4; oct++) {
        uint8_t val = ip[oct];
        uint8_t d1 = val / 100;
        uint8_t d2 = (val % 100) / 10;
        uint8_t d3 = val % 10;
        if (d1) { blinkDigit(d1); delay(400); }
        if (d1 || d2) { blinkDigit(d2); delay(400); }
        blinkDigit(d3);
        delay(900);  // pausa entre octetos
    }
    // Flash final: "fin"
    for (int i = 0; i < 2; i++) {
        sr.setAllLeds(true); delay(300); sr.setAllLeds(false); delay(150);
    }
}

// --- Prototipos internos ---
void cmd_help(Print& out);
void cmd_scan_i2c(Print& out);
void cmd_led_test(Print& out);
void cmd_expander_test(Print& out);
void cmd_all_input(Print& out);
void cmd_drive(const String& pin_str, const String& level_str, Print& out);
void cmd_read_all(Print& out);
void cmd_loopback(Print& out);
void cmd_scope_read(const String& pin_str, Print& out);
void cmd_sr_dump(Print& out);
void cmd_wifi_info(Print& out);
void cmd_sysinfo(Print& out);
bool parseJpin(const String& s, int& out_pin);
void printLine(Print& out, int n = 48);
String jname(int n);

// ── Causa del último reinicio ─────────────────────────────────
// Es el diagnóstico clave para módulos ensamblados a mano: un BROWNOUT
// delata alimentación marginal (soldadura fría, desacoplo insuficiente),
// que de otro modo solo se manifiesta como reinicios aleatorios bajo WiFi.
static const char* resetReasonName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "POWERON (normal)";
        case ESP_RST_SW:       return "SW (ESP.restart)";
        case ESP_RST_PANIC:    return "PANIC  <-- crash de firmware";
        case ESP_RST_INT_WDT:  return "INT_WDT  <-- watchdog de interrupcion";
        case ESP_RST_TASK_WDT: return "TASK_WDT <-- watchdog de tarea";
        case ESP_RST_WDT:      return "WDT      <-- watchdog";
        case ESP_RST_BROWNOUT: return "BROWNOUT <-- ALIMENTACION MARGINAL";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_EXT:      return "EXT (reset externo)";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "DESCONOCIDA";
    }
}

// ── Handlers de prueba distribuida (lado hijo) ───────────────
// link.cpp no conoce el hardware; el .ino le registra qué hacer. Así el
// mismo firmware sirve de master o de hijo sin compilar dos versiones.
static uint32_t th_sample(int ch, bool level) { return mapSampleLocal(u1, u2, ch, level); }
static void     th_drive(int ch, bool level)  { mapDriveHold(u1, u2, ch, level); }
static void     th_release()                  { mapRelease(u1, u2); }
static void     th_leds(uint16_t mask)        { sr.setAllLeds(false); sr.setLeds(mask); }

// Modo dev en el hijo. Se copia a un array tipado en vez de castear el buffer
// de bytes: el RISC-V del C3 no perdona un uint16_t leido sin alinear.
static uint8_t th_rawexec(const uint8_t* ops, uint8_t n, int32_t* results) {
    if (n > RAW_MAX_OPS) n = RAW_MAX_OPS;
    RawOpCode buf[RAW_MAX_OPS];
    memcpy(buf, ops, (size_t)n * sizeof(RawOpCode));
    return rawExec(buf, n, results);
}

// =============================================================
// Control de LEDs y disparador físico
// =============================================================
#define PREF_NS_CFG "cfg"

// Modo de LEDs:
//   auto   = el firmware los pone tras cada prueba (verde/rojo por rango)
//   manual = solo la app decide (POST /api/leds). Util si la PC quiere
//            señalizar otra cosa, o apagarlos entre piezas.
static bool _led_auto = true;

// Disparador físico: un switch en un GPIO equivale al botón de la app.
// En producción el operador conecta el arnés y pulsa sin volver a la PC —
// eso es lo que sube el ritmo. Queda registrado igual que un clic.
static int8_t   _trig_pin  = -1;     // -1 = desactivado
static bool     _trig_low  = true;   // true = activo en BAJO (con pull-up interno)
static bool     _trig_prev = true;
static uint32_t _trig_next = 0;      // anti-rebote / anti-repetición

// QUE HACE EL BOTON AL PULSARLO
//
//   0 = AVISAR (por defecto): solo cuenta la pulsacion. La prueba la ejecuta la
//       PC, que es donde vive el golden. Vale igual para uno o para tres equipos.
//   1 = LOCAL: el equipo corre SU prueba con el golden de su NVS.
//
// Por que 0 es el defecto: con el golden en la PC, la prueba interna aborta con
// "sin golden cargado" y —esto es lo importante— NO incrementa _last_seq, asi
// que la app no se entera de nada. Un boton que no hace nada visible es peor
// que no tener boton.
static uint8_t  _trig_mode    = 0;
static uint16_t _trig_presses = 0;   // sube con CADA pulsacion, la ejecute quien la ejecute

// Resultado de la última prueba. La app lo consulta para enterarse de las
// disparadas por el switch, que no pasan por ella.
// 0 = sin reinicio pendiente. Se difiere para que la respuesta HTTP llegue
// antes de que el equipo se caiga; si se reinicia dentro del handler, quien
// mando el comando ve un error de red en vez de la confirmacion.
static uint32_t    _reboot_at = 0;
static MapResult   _last_dut;
static uint16_t    _last_seq    = 0;
static uint32_t    _last_ms     = 0;
static int         _last_faults = -1;
static const char* _last_result = "";
static bool        _test_busy   = false;

// Print que descarta: el mapeo emite traza humana que no debe ensuciar el JSON.
class NullPrint : public Print {
public:
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
};

static void cfgLoad() {
    Preferences p; p.begin(PREF_NS_CFG, true);
    _led_auto = p.getBool("led_auto", true);
    _trig_pin  = (int8_t)p.getChar("trig_pin", -1);
    _trig_low  = p.getBool("trig_low", true);
    _trig_mode = (uint8_t)p.getUChar("trig_mode", 0);
    p.end();
}

static void cfgSave() {
    Preferences p; p.begin(PREF_NS_CFG, false);
    p.putBool("led_auto", _led_auto);
    p.putChar("trig_pin", (int8_t)_trig_pin);
    p.putBool("trig_low", _trig_low);
    p.putUChar("trig_mode", _trig_mode);
    p.end();
}

// Aplica una máscara de LEDs a una unidad. 0 = master, 1..N = hijos.
static bool ledsApply(uint8_t unit, uint16_t mask) {
    if (unit == 0) { sr.setAllLeds(false); sr.setLeds(mask); return true; }
    return linkSendLeds((uint8_t)(unit - 1), mask);
}

// Núcleo de la prueba. Lo usan POST /api/test y el disparador físico.
// Devuelve nº de fallos (0 = PASS), o -1 con motivo en 'err'.
static int runTestCore(String& err) {
    if (_test_busy) { err = "prueba en curso"; return -1; }
    _test_busy = true;
    const uint32_t t0 = millis();

    MapResult g; String gname;
    if (!mapLoadGolden(g, gname)) {
        err = mapHasGolden() ? "golden corrupto (CRC no cuadra)" : "sin golden cargado";
        _test_busy = false; return -1;
    }

    NullPrint quiet;
    if (!mapDistRun(u1, u2, _last_dut, quiet)) {
        err = "fallo el mapeo (I2C o alguna unidad sin responder)";
        _test_busy = false; return -1;
    }
    if (g.channels != _last_dut.channels) {
        char b[120];
        snprintf(b, sizeof(b), "el golden es de %u canales y esta configuracion tiene %u "
                 "— revisa las unidades enlazadas", g.channels, _last_dut.channels);
        err = b;
        _test_busy = false; return -1;
    }

    if (_led_auto) mapDistLeds(g, _last_dut);

    NullPrint sink;
    int n = mapFaultsJson(g, _last_dut, sink);   // solo cuenta
    _last_faults = n;
    _last_result = (n == 0) ? "PASS" : "FAIL";
    _last_ms     = millis() - t0;
    _last_seq++;
    _test_busy   = false;
    return n;
}

static void trigLoop() {
    if (_trig_pin < 0 || _test_busy) return;
    bool raw    = digitalRead(_trig_pin);
    bool active = _trig_low ? !raw : raw;

    if (active && !_trig_prev && (int32_t)(millis() - _trig_next) >= 0) {
        delay(30);                                  // anti-rebote
        bool still = _trig_low ? !digitalRead(_trig_pin) : digitalRead(_trig_pin);
        if (still) {
            // El contador sube SIEMPRE y lo primero: es lo unico que mira la
            // PC, y tiene que subir aunque la prueba local falle o ni se
            // intente. Si subiera solo al terminar bien, un boton pulsado sin
            // golden seria indistinguible de un boton no pulsado.
            _trig_presses++;
            if (_trig_mode == 0) {
                Serial.printf("[TRIG] Pulsacion %u -> avisando a la PC.\n", _trig_presses);
                _trig_next = millis() + 1000;
                _trig_prev = active;
                return;
            }
            Serial.println(F("[TRIG] Switch fisico -> ejecutando prueba local..."));
            String err;
            int n = runTestCore(err);
            if (n < 0) Serial.printf("[TRIG] ERROR: %s\n", err.c_str());
            else       Serial.printf("[TRIG] %s (%d fallo(s), %lu ms) seq=%u\n",
                                     _last_result, n, (unsigned long)_last_ms, _last_seq);
            _trig_next = millis() + 1000;           // no repetir con una pulsación larga
        }
    }
    _trig_prev = active;
}

// ── RGB (WS2812B en IO8, usar ANTES de Wire.begin) ───────────
static void rgb(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(PIN_RGB_LED, r, g, b);
}
static void rgb_off() { rgb(0, 0, 0); }

// =============================================================
void setup() {
    // 1. Apagar shift register inmediatamente (retiene estado tras reset)
    sr.begin();

    // 1b. Serial ANTES de las animaciones: el banner sale de inmediato y las
    //     herramientas de provision pueden detectar el arranque sin esperar
    //     los ~2 s de animacion de LEDs.
    Serial.begin(SERIAL_BAUD);

    // 2. Animación RGB — IO8 libre ANTES de Wire.begin()
    //    Después de Wire.begin(SDA=9, SCL=8), IO8 es SCL y RGB no funciona
    const uint8_t BR = CT_RGB_BRIGHTNESS;
    rgb(BR, 0,  0);  delay(200);  // Rojo
    rgb(0,  BR, 0);  delay(200);  // Verde
    rgb(0,  0,  BR); delay(200);  // Azul
    rgb_off();

    // 3. Animación LEDs tester — confirma shift register U3/U4
    const uint16_t leds[10] = {
        SR_LED1_RED, SR_LED2_RED, SR_LED3_RED, SR_LED4_RED, SR_LED5_RED,
        SR_LED1_GREEN, SR_LED2_GREEN, SR_LED3_GREEN, SR_LED4_GREEN, SR_LED5_GREEN
    };
    for (int rep = 0; rep < 2; rep++) {
        for (int i = 0; i < 10; i++) {
            sr.setAllLeds(false);
            sr.setLeds(leds[i]);
            delay(55);
        }
    }
    sr.setAllLeds(false);
    for (int i = 0; i < 3; i++) {
        sr.setAllLeds(true);  delay(100);
        sr.setAllLeds(false); delay(100);
    }

    // 4. Banner (Serial ya arrancado en el paso 1b)
    delay(400);
    Serial.println(F("\n================================================"));
    Serial.println(F("  Cable Tester 32CH — Firmware v" FW_VERSION ""));
    Serial.println(F("  ESP32-C3-MINI-1-N4 | SBC Rev1.1"));
    Serial.println(F("================================================\n"));
    Serial.printf("[SYS]  Unidad %s | Reinicio: %s\n",
                  wifiUnitId().c_str(), resetReasonName());

    // 5. Botón
    pinMode(PIN_BTN_START, INPUT_PULLUP);
    // Escape manual: arrancar con BTN_START presionado fuerza el portal aunque
    // la unidad sea slave. Sirve para recuperar un equipo cuyo master se perdió.
    delay(20);
    bool force_portal = (digitalRead(PIN_BTN_START) == LOW);
    if (force_portal) Serial.println(F("[BTN]  BTN_START al arranque → portal forzado."));

    // 6. WiFi + enlace — ANTES de Wire.begin() para poder usar RGB como indicador
    //
    // El rol se decide en runtime, no en compilación:
    //   con WiFi           → MASTER
    //   sin WiFi + peer    → SLAVE (escucha; NO abre AP, ver LINK_DESIGN §4.1)
    //   sin WiFi sin peer  → portal captivo (unidad suelta)
    LinkRole role       = LINK_ROLE_NONE;
    LinkRole configured = linkGetConfiguredRole();   // NONE = auto
    if (configured != LINK_ROLE_NONE)
        Serial.printf("[PROV] Rol fijado en NVS: %s\n", linkRoleName(configured));

    if (!force_portal && wifiBegin(12000)) {
        // Con provisión por lote TODAS las unidades llevan las mismas
        // credenciales, así que "tiene WiFi = master" ya no sirve para decidir.
        // Si hay rol explícito, manda ese.
        role = (configured == LINK_ROLE_SLAVE) ? LINK_ROLE_SLAVE : LINK_ROLE_MASTER;
        webCmdBegin();                              // web + OTA en ambos roles
        rgb(0, BR, 0); delay(500); rgb_off();       // Pulso verde = conectado
        linkBegin(role);
        wifiMdnsSetInfo(linkRoleName(role), mapGetSku());   // descubrible por la PC
    } else {
        // CRITICO: cortar la reconexion automatica.
        //
        // Tras un WiFi.begin() fallido, la libreria sigue reintentando por su
        // cuenta para siempre, y CADA reintento barre canales — se lleva la
        // radio fuera del canal del master durante segundos. El hijo deja de
        // oir, a los 15 s se declara desenlazado, se pone a barrer buscandolo,
        // y vuelve... hasta el siguiente reintento.
        //
        // MEDIDO: durante un bloqueo el hijo solo recibia ~17% de los paquetes
        // (84 de ~480), justo lo que oye una unidad que esta barriendo canales
        // (600 ms de cada 7.8 s ≈ 8%). Y se recuperaba sola a los minutos.
        //
        // Un hijo no necesita red: solo habla ESP-NOW. El unico que sirve HTTP
        // es el master.
        // Un HIJO no puede tener la STA intentando conectarse.
        //
        // MEDIDO: con la reconexion automatica activa, el barrido falla cada
        // ~60 s con una firma inequivoca — los paquetes del master salen y el
        // hijo NO los confirma a nivel MAC (TX sin ACK +120), mientras los
        // contadores del hijo no se mueven: ni se entera de que le hablan.
        // Cada reintento de conexion escanea canales y lo saca de la radio.
        //
        // Esto hace incompatibles los dos transportes en una misma unidad:
        //   rol HIJO  -> ESP-NOW, sin WiFi
        //   sin rol   -> WiFi, y entonces la PC orquesta y no hace falta ESP-NOW
        // No es una limitacion que se pueda esquivar: el ESP32-C3 tiene UNA
        // radio y un canal a la vez.
        WiFi.disconnect(true, true);
        WiFi.setAutoReconnect(false);
        WiFi.mode(WIFI_STA);
        linkBegin(LINK_ROLE_SLAVE);                // carga el peer guardado, si hay

        // Unidad virgen: ESCUCHA PRIMERO. Sin esta ventana abriría el portal de
        // inmediato y jamás emitiría descubrimiento → sería imposible emparejarla.
        if (!linkPeerCount() && !force_portal) {
            Serial.printf("[LINK] Buscando master en modo emparejamiento (%d s)...\n",
                          LINK_DISCOVERY_MS / 1000);
            uint32_t t0 = millis();
            while (millis() - t0 < LINK_DISCOVERY_MS && !linkPeerCount()) {
                linkLoop();
                // Abortar si llega algo por serial: durante esta ventana loop()
                // aun no corre, asi que sin esto la unidad queda MUDA ~25 s y
                // cualquier herramienta de provision se rinde antes de tiempo.
                if (Serial.available()) {
                    Serial.println(F("[LINK] Descubrimiento cancelado (comando por serial)."));
                    break;
                }
                bool on = ((millis() / 250) % 2) == 0;   // parpadeo magenta
                rgb(on ? BR : 0, 0, on ? BR : 0);
                delay(20);
            }
            rgb_off();
            if (!linkPeerCount() && !Serial.available())
                Serial.println(F("[LINK] Ningun master emparejando. Abriendo portal."));
        }

        if (linkPeerCount() && !force_portal) {
            // Es un slave enlazado: espera órdenes/credenciales del master.
            // Levantar un AP aquí crearía dos portales homónimos — justo lo que
            // el diseño evita. Se recupera con BTN_START al arranque.
            role = LINK_ROLE_SLAVE;
            Serial.println(F("[WiFi] Sin red, pero hay peer → modo SLAVE."));
            Serial.println(F("[WiFi] Esperando credenciales del master (no se abre AP)."));
            rgb(BR, 0, BR);                        // Magenta fijo = slave esperando
        } else {
            wifiPortalBegin();
            Serial.printf("[WiFi] Portal activo: '%s' → http://192.168.4.1/\n",
                          wifiGetApSsid().c_str());
            rgb(0, 0, BR);                         // Azul fijo = portal activo
        }
    }
    // A partir de aquí NO llamar rgb() — Wire toma IO8 como SCL

    // 7. I2C + TCA9535
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    Serial.printf("[I2C]  SDA=IO%d SCL=IO%d @ 100kHz\n", PIN_SDA, PIN_SCL);

    u1_ok = u1.begin();
    if (u1_ok) {
        u1.setDirection(0x0000);
        u1.writePort(0x0000);
        Serial.printf("[TCA]  U1 0x%02X OK  → OUTPUT (J00-J15)\n", TCA9535_U1_ADDR);
    } else {
        Serial.printf("[TCA]  U1 0x%02X NO RESPONDE\n", TCA9535_U1_ADDR);
    }

    u2_ok = u2.begin();
    if (u2_ok) {
        u2.setDirection(0xFFFF);
        Serial.printf("[TCA]  U2 0x%02X OK  → INPUT  (J16-J30+J_SPARE)\n", TCA9535_U2_ADDR);
    } else {
        Serial.printf("[TCA]  U2 0x%02X NO RESPONDE\n", TCA9535_U2_ADDR);
    }

    // 8. Configuracion de LEDs y disparador fisico
    cfgLoad();
    if (_trig_pin >= 0) {
        pinMode(_trig_pin, _trig_low ? INPUT_PULLUP : INPUT_PULLDOWN);
        _trig_prev = _trig_low ? !digitalRead(_trig_pin) : digitalRead(_trig_pin);
        Serial.printf("[TRIG] Switch en IO%d activo %s -> ejecuta la prueba.\n",
                      _trig_pin, _trig_low ? "BAJO" : "ALTO");
    }
    Serial.printf("[LED]  Modo %s\n", _led_auto ? "AUTO" : "MANUAL");

    // 9. Handlers de prueba distribuida — valen para master y para hijo
    {
        LinkTestHandlers th = { th_sample, th_drive, th_release, th_leds, th_rawexec };
        linkSetTestHandlers(th);
    }

    // 10. Modo dev: primitivas crudas sobre el mismo hardware.
    rawBegin(&u1, &u2, &sr);

    Serial.println(F("\n[OK]   Listo. Serial o web para comandos. Escribe 'help'."));
}

// =============================================================
void loop() {
    if (_reboot_at && (int32_t)(millis() - _reboot_at) >= 0) ESP.restart();
    // Portal WiFi (si no hay red guardada)
    if (wifiIsPortalActive()) {
        wifiPortalLoop();
        // linkLoop() TAMBIEN con el portal abierto.
        //
        // Sin esto, una unidad en modo AP recibe las credenciales por ESP-NOW
        // —el callback las deja en _creds_pending— y NADIE las aplica, porque
        // quien lo hace es linkLoop(). Se quedaba en el portal para siempre y
        // el aprovisionamiento por lote no podia funcionar: justo el caso de
        // mover el equipo a otra planta, que es cuando hace falta.
        linkLoop();
        return;
    }

    // Web server
    webCmdLoop();

    // Enlace ESP-NOW (emparejamiento, credenciales, barrido de canales)
    linkLoop();

    // Disparador fisico → ejecuta la prueba
    trigLoop();

    // BTN_START → parpadear la IP, SOLO si no esta asignado como disparador
    if (_trig_pin != PIN_BTN_START) {
        static bool btn_prev = HIGH;
        bool btn = digitalRead(PIN_BTN_START);
        if (btn_prev == HIGH && btn == LOW) {
            delay(30);  // debounce
            if (digitalRead(PIN_BTN_START) == LOW) showIPonLeds();
        }
        btn_prev = btn;
    }

    // Serial — SOLO si hay algo pendiente.
    //
    // readStringUntil() tiene 1 s de timeout por defecto: llamarlo a secas
    // bloquea loop() un segundo entero cuando no hay entrada. Con eso el
    // protocolo de prueba distribuido (que responde desde linkLoop) tardaria
    // ~1 s por mensaje en vez de milisegundos.
    String line;
    if (Serial.available()) {
        Serial.setTimeout(50);
        line = Serial.readStringUntil('\n');
    }
    line.trim();
    if (line.length() > 0) {
        Serial.print(F(">> "));
        Serial.println(line);
        handleCommand(line, Serial);
        Serial.println();
    }
}

// =============================================================
// handleCommand — punto de entrada para serial Y web
// =============================================================
void handleCommand(const String& raw, Print& out) {
    String cmd = raw;
    cmd.trim();
    cmd.toLowerCase();

    if (cmd == "help") {
        cmd_help(out);
    } else if (cmd == "scan_i2c") {
        cmd_scan_i2c(out);
    } else if (cmd == "led_test") {
        cmd_led_test(out);
    } else if (cmd == "expander_test") {
        cmd_expander_test(out);
    } else if (cmd == "all_input") {
        cmd_all_input(out);
    } else if (cmd == "read_all") {
        cmd_read_all(out);
    } else if (cmd == "loopback16") {
        cmd_loopback(out);
    } else if (cmd == "selftest") {
        stFullLoopback(u1, u2, sr, out);
    } else if (cmd == "rtest") {
        stContactResistance(u1, u2, sr, out);
    } else if (cmd == "leaktest") {
        stLeakTest(u1, u2, sr, out);
    } else if (cmd == "sr_dump") {
        cmd_sr_dump(out);
    } else if (cmd == "wifi_info") {
        cmd_wifi_info(out);
    } else if (cmd == "sysinfo") {
        cmd_sysinfo(out);

    // ── API de maquina por SERIAL ─────────────────────────────
    // Mismo JSON que /api/* por HTTP. Asi la app de PC usa el mismo contrato
    // por USB (PC sin WiFi compartido) o por red, sin cambiar de protocolo.
    } else if (cmd == "api_status") {
        apiStatusJson(out);

    } else if (cmd == "api_golden") {
        apiGoldenGetJson(out);

    } else if (cmd == "api_test") {
        apiRunTest(out);

    } else if (cmd == "api_last") {
        apiLastTestJson(out);

    } else if (cmd == "api_selfcheck") {
        apiSelfCheckJson(out);

    } else if (cmd == "api_map") {
        apiMapJson(out);

    } else if (cmd == "selfcheck") {
        static uint8_t st[MAP_CH_MAX];
        int bad = mapDistSelfCheck(u1, u2, st, out);
        const uint8_t N = mapDistChannels();
        if (bad == 0) out.println(F("[CHK]  >>> MODULOS OK — ningun pin pegado <<<"));
        else {
            printLine(out);
            for (uint8_t g = 0; g < N; g++) {
                if (st[g] == CHK_OK) continue;
                out.printf("  %-8s %s\n", mapChName(g).c_str(),
                    st[g] == CHK_STUCK_LOW  ? "PEGADO A GND (no sube)"
                  : st[g] == CHK_STUCK_HIGH ? "PEGADO A 3V3 (no baja)" : "ERROR de comunicacion");
            }
            printLine(out);
            out.println(F("  >>> NO capturar un golden asi: congelaria el defecto <<<"));
        }

    // ── LEDs y disparador físico ──────────────────────────────
    } else if (cmd.startsWith("leds ")) {
        String a = cmd.substring(5); a.trim();
        if (a == "off") {
            for (uint8_t u = 0; u <= linkPeerCount(); u++) ledsApply(u, 0);
            out.println(F("[LED]  Todas las unidades apagadas."));
        } else {
            int sp = a.indexOf(' ');
            uint8_t unit = 0; long mask;
            if (sp > 0) { unit = (uint8_t)a.substring(0, sp).toInt(); mask = a.substring(sp + 1).toInt(); }
            else        { mask = a.toInt(); }
            if (unit > linkPeerCount())      out.printf("[ERR]  No hay unidad %u.\n", unit);
            else if (mask < 0 || mask > 1023) out.println(F("[ERR]  Mascara 0-1023 (bits 0-4 rojo 1-5, 5-9 verde 1-5)."));
            else out.println(ledsApply(unit, (uint16_t)mask)
                    ? F("[LED]  Aplicado.") : F("[ERR]  No se pudo enviar al hijo."));
        }

    } else if (cmd.startsWith("led_mode")) {
        String a = cmd.substring(8); a.trim();
        if (a == "auto" || a == "manual") {
            _led_auto = (a == "auto"); cfgSave();
            out.printf("[LED]  Modo: %s\n", _led_auto
                ? "AUTO (el firmware los pone tras cada prueba)"
                : "MANUAL (solo la app, con 'leds')");
        } else {
            out.printf("[LED]  Modo actual: %s\n", _led_auto ? "auto" : "manual");
            out.println(F("[LED]  Uso: led_mode auto|manual"));
        }

    } else if (cmd.startsWith("trigger")) {
        String a = cmd.substring(7); a.trim();
        if (!a.length()) {
            out.printf("[TRIG] Pin: %d %s modo=%s pulsaciones=%u\n", _trig_pin,
                       _trig_pin < 0 ? "(desactivado)" : (_trig_low ? "(activo BAJO)" : "(activo ALTO)"),
                       _trig_mode == 0 ? "avisar" : "local", _trig_presses);
            out.println(F("[TRIG] Uso: trigger <pin> [low|high] [avisar|local] | trigger off"));
            out.println(F("[TRIG] avisar = solo cuenta la pulsacion; la prueba la corre la PC (defecto)."));
            out.println(F("[TRIG] local  = el equipo corre su prueba con el golden de su NVS."));
            out.println(F("[TRIG] Libres en el header: 4, 5, 6, 7. El 10 ya trae boton (BTN_START)."));
        } else if (a == "off") {
            _trig_pin = -1; cfgSave();
            out.println(F("[TRIG] Disparador desactivado."));
        } else {
            int sp = a.indexOf(' ');
            int pin = (sp > 0 ? a.substring(0, sp) : a).toInt();
            String resto = (sp > 0) ? a.substring(sp + 1) : String("");
            resto.trim();
            bool low = !resto.startsWith("high");
            // El modo puede ir en cualquier posicion del resto: "4 low local",
            // "4 local" y "4 high local" valen igual.
            uint8_t modo = resto.indexOf("local") >= 0 ? 1 : 0;
            // Pines ocupados: 0,1,2 (595) · 3 (ADC) · 8,9 (I2C) · 18,19 (USB)
            const int BUSY[] = {0, 1, 2, 3, 8, 9, 18, 19};
            bool busy = false;
            for (int b : BUSY) if (b == pin) busy = true;
            if (pin < 0 || pin > 21 || busy) {
                out.printf("[ERR]  Pin %d no disponible. Libres: 4, 5, 6, 7, 10.\n", pin);
            } else {
                _trig_pin = (int8_t)pin; _trig_low = low; _trig_mode = modo; cfgSave();
                pinMode(_trig_pin, _trig_low ? INPUT_PULLUP : INPUT_PULLDOWN);
                _trig_prev = _trig_low ? !digitalRead(_trig_pin) : digitalRead(_trig_pin);
                out.printf("[TRIG] Switch en IO%d, activo %s, modo %s.\n",
                           pin, low ? "BAJO" : "ALTO", modo == 0 ? "avisar" : "local");
                if (pin == PIN_BTN_START)
                    out.println(F("[TRIG] Nota: IO10 deja de parpadear la IP; ahora dispara la prueba."));
            }
        }

    } else if (cmd.startsWith("api_golden_set ")) {
        // Una sola linea, campos separados por ';' (por serial no hay cuerpo
        // multilinea). Se leen de 'raw' para conservar mayusculas del SKU.
        String body = raw; body.trim();
        body = body.substring(15);
        body.replace(';', '\n');
        apiGoldenSet(body, out);

    // ── Mapeo por golden ──────────────────────────────────────
    // mapDistRun cubre 1, 2 o 3 unidades: sin hijos cae en el mapeo local.
    } else if (cmd == "map") {
        MapResult m;
        if (mapDistRun(u1, u2, m, out)) mapPrint(m, out);

    } else if (cmd.startsWith("map_save")) {
        String nombre = raw; nombre.trim();
        nombre = (nombre.length() > 8) ? nombre.substring(9) : String("");
        nombre.trim();
        MapResult m;
        if (!mapDistRun(u1, u2, m, out)) { out.println(F("[ERR]  Fallo el mapeo.")); }
        else {
            mapPrint(m, out);
            out.println(mapSaveGolden(m, nombre)
                ? F("[MAP]  Guardado como GOLDEN de referencia.")
                : F("[ERR]  No se pudo guardar."));
        }

    } else if (cmd == "map_check") {
        MapResult g, d; String gname;
        if (!mapLoadGolden(g, gname)) {
            out.println(F("[ERR]  No hay golden guardado. Conecta uno bueno y usa 'map_save'."));
        } else if (!mapDistRun(u1, u2, d, out)) {
            out.println(F("[ERR]  Fallo el mapeo."));
        } else {
            out.printf("[MAP]  Golden: '%s'\n", gname.c_str());
            int fallos = mapCompare(g, d, out);
            if (fallos >= 0) {
                mapDistLeds(g, d);
                out.printf("[MAP]  LEDs actualizados en %u unidad(es) (%d fallo(s)).\n",
                           1 + linkPeerCount(), fallos);
            }
        }

    } else if (cmd == "map_golden") {
        MapResult g; String gname;
        if (!mapLoadGolden(g, gname)) out.println(F("[MAP]  Sin golden guardado."));
        else { out.printf("[MAP]  Golden: '%s'\n", gname.c_str()); mapPrint(g, out); }

    } else if (cmd == "map_json") {
        MapResult g; String gname;
        if (!mapLoadGolden(g, gname)) out.println(F("[MAP]  Sin golden guardado."));
        else mapPrintJson(g, gname.c_str(), out);

    } else if (cmd == "map_clear") {
        mapClearGolden();
        out.println(F("[MAP]  Golden borrado (el SKU se conserva)."));

    } else if (cmd.startsWith("sku_set")) {
        // OJO: desde 'raw' — toLowerCase() destruiria mayusculas del SKU
        String s = raw; s.trim();
        s = (s.length() > 7) ? s.substring(8) : String("");
        s.trim();
        out.println(mapSetSku(s)
            ? (s.length() ? String("[SKU]  Tester vinculado a: ") + s
                          : String("[SKU]  Vinculacion borrada."))
            : String("[ERR]  SKU demasiado largo (max 40)."));

    } else if (cmd == "sku_get") {
        String s = mapGetSku();
        out.printf("[SKU]  Tester: %s\n", s.length() ? s.c_str() : "(sin vincular)");
        out.printf("[SKU]  Meta  : %s\n", mapGetMeta().c_str());
        out.printf("[SKU]  Golden CRC: 0x%04X\n", mapGoldenCrc());

    } else if (cmd.startsWith("meta_set")) {
        String s = raw; s.trim();
        s = (s.length() > 8) ? s.substring(9) : String("");
        s.trim();
        out.println(mapSetMeta(s) ? F("[SKU]  Metadatos guardados.")
                                  : F("[ERR]  Metadatos demasiado largos (max 120)."));

    } else if (cmd == "tester_clear") {
        mapClearAll();
        out.println(F("[SKU]  Tester LIBERADO: golden, SKU y metadatos borrados."));
        out.println(F("[SKU]  Listo para reasignarse a otro producto."));

    // ── Modo dev: primitivas crudas ───────────────────────────
    // Ver raw.h. No interpretan nada: la PC compone el experimento y el
    // firmware solo ejecuta, así que deja de quedarse obsoleto cada vez que
    // se nos ocurre una prueba nueva. Los comandos de arriba no cambian.
    } else if (cmd.startsWith("ch_count")) {
        int sp3 = cmd.indexOf(' ');
        if (sp3 > 0) {
            int v = cmd.substring(sp3 + 1).toInt();
            out.println(mapSetChCount((uint8_t)v)
                ? F("[CFG]  Canales poblados guardados. Reinicia para aplicar.")
                : F("[ERR]  Solo 16 o 32."));
        }
        out.printf("[CFG]  Canales poblados: %u\n", mapGetChCount());

    } else if (cmd == "peer_swap") {
        if (linkSwapPeers()) {
            out.println(F("[LINK] Orden invertido. Ahora:"));
            for (uint8_t i = 0; i < linkPeerCount(); i++)
                out.printf("  L%u = %s\n", i + 1, linkPeerMacStr(i).c_str());
            out.println(F("[LINK] Si el fallo sigue en L1, es la POSICION."));
            out.println(F("[LINK] Si se va con la placa, es esa UNIDAD."));
        } else {
            out.println(F("[LINK] Hacen falta 2 hijos."));
        }

    } else if (cmd.startsWith("burst")) {
        // Machaca el enlace con ida-y-vueltas SIN tocar I2C, al mismo ritmo que
        // un barrido. Si aqui no se pierde nada pero 'map' si, el problema esta
        // en la ruta de muestreo, no en la radio.
        int n = 100;
        int sp2 = cmd.indexOf(' ');
        if (sp2 > 0) { int v = cmd.substring(sp2 + 1).toInt(); if (v > 0) n = v; }
        out.printf("[BURST] %d ida-y-vueltas por hijo, sin I2C...\n", n);
        // Segundo argumento: 0 = ping vacio, 1 = lectura suelta, 2 = ciclo de
        // muestreo completo. Comparar 0 contra 2 separa la radio del bus I2C.
        uint8_t modo = 0;
        {
            int s2 = cmd.indexOf(' ', sp2 + 1);
            if (s2 > 0) modo = (uint8_t)cmd.substring(s2 + 1).toInt();
        }
        static const char* MODO[5] = { "ping vacio", "lectura I2C",
                                       "muestreo completo", "barrido real", "barrido 96ch" };
        out.printf("[BURST] modo %u (%s)\n", modo, MODO[modo > 4 ? 0 : modo]);

        if (modo == 3 || modo == 4) {
            // modo 4: igual pero recorriendo los 96 canales, asi que 2 de cada
            // 3 rondas pasan por linkRemoteDrive(). Es LA diferencia entre el
            // modo 3 (600 rondas sin un fallo) y un 'map' real (se cae).
            const bool remoto = (modo == 4);
            const uint8_t NCH = mapDistChannels();
            // Imita un barrido de verdad: excitar y preguntar a TODOS los hijos
            // en orden, igual que _sweep(). Reproduce el fallo en segundos en
            // vez de esperar un 'map' completo.
            uint32_t rondas = 0, fallos = 0, t0 = millis();
            int8_t primer_mudo = -1;
            uint32_t masks[LINK_MAX_PEERS];
            uint32_t fallos_drive = 0;
            const uint32_t t_fin = millis() + MAPDIST_DEADLINE_MS;
            for (int i = 0; i < n; i++) {
                if ((int32_t)(millis() - t_fin) >= 0) {
                    out.println(F("[BURST] ABORTADO por plazo."));
                    break;
                }
                bool level = (i & 1) != 0;
                uint8_t src   = remoto ? (uint8_t)(i % NCH) : (uint8_t)(i % 32);
                uint8_t unit  = (uint8_t)(src / 32);
                uint8_t local = (uint8_t)(src % 32);
                rondas++;
                if (unit == 0) {
                    mapDriveHold(u1, u2, local, level);
                } else if (!linkRemoteDrive((uint8_t)(unit - 1), local, level)) {
                    fallos_drive++; fallos++;
                    if (primer_mudo < 0) primer_mudo = (int8_t)(unit - 1);
                    continue;
                }
                mapSampleLocal(u1, u2, unit == 0 ? local : -1, level);
                if (!linkSampleAll(level, masks, (int8_t)unit, (int8_t)local)) {
                    fallos++;
                    if (primer_mudo < 0) primer_mudo = linkFirstSilentPeer();
                }
            }
            linkReleaseAll();
            mapRelease(u1, u2);
            uint32_t ms = millis() - t0;
            out.printf("[BURST] %lu rondas, %lu fallos (%lu ms, %.1f/s)\n",
                       (unsigned long)rondas, (unsigned long)fallos,
                       (unsigned long)ms, ms ? (rondas * 1000.0f / ms) : 0.0f);
            out.printf("[BURST] primer mudo: %s\n",
                       primer_mudo < 0 ? "ninguno" : (primer_mudo == 0 ? "L1" : "L2"));
            return;
        }

    } else if (cmd == "raw" || cmd == "raw_help") {
        rawPrintHelp(out);

    } else if (cmd.startsWith("raw ")) {
        RawOpCode op; String err;
        if (!rawParse(cmd.substring(4), op, err)) {
            out.printf("[RAW]  ERROR: %s\n", err.c_str());
        } else {
            int32_t r = RAW_ERR;
            rawExec(&op, 1, &r);
            if (r == RAW_ERR) out.printf("[RAW]  %s -> ERROR\n", rawOpName(op.op));
            else              out.printf("[RAW]  %s -> %ld (0x%04lX)\n",
                                         rawOpName(op.op), (long)r, (long)(r & 0xFFFF));
        }

    } else if (cmd.startsWith("rraw ")) {
        // rraw <unidad> <op> [a] [b]   unidad: 1 = L1, 2 = L2
        String rest = cmd.substring(5); rest.trim();
        int sp = rest.indexOf(' ');
        int unit = (sp > 0) ? rest.substring(0, sp).toInt() : 0;
        if (unit < 1 || sp < 0) {
            out.println(F("[RAW]  Uso: rraw <unidad 1-2> <op> [a] [b]"));
        } else {
            RawOpCode op; String err;
            if (!rawParse(rest.substring(sp + 1), op, err)) {
                out.printf("[RAW]  ERROR: %s\n", err.c_str());
            } else {
                int32_t r = RAW_ERR;
                if (!linkRemoteRaw((uint8_t)(unit - 1), &op, 1, &r))
                    out.printf("[RAW]  L%d no contesto\n", unit);
                else if (r == RAW_ERR)
                    out.printf("[RAW]  L%d %s -> ERROR\n", unit, rawOpName(op.op));
                else
                    out.printf("[RAW]  L%d %s -> %ld (0x%04lX)\n", unit,
                               rawOpName(op.op), (long)r, (long)(r & 0xFFFF));
            }
        }

    // ── Provisión por lote ────────────────────────────────────
    // OJO: estos comandos leen de 'raw', NO de 'cmd'. handleCommand() aplica
    // toLowerCase() al principio, lo que destruiria mayusculas de SSID y clave.
    } else if (cmd.startsWith("wifi_set ")) {
        String rest = raw; rest.trim();
        rest = rest.substring(9);          // longitud de "wifi_set "
        int sep = rest.indexOf('|');       // separador SSID|clave
        if (sep < 0) {
            out.println(F("[ERR]  Uso: wifi_set <ssid>|<clave>   (clave vacia = red abierta)"));
        } else {
            String ssid = rest.substring(0, sep);
            String pass = rest.substring(sep + 1);   // puede contener '|'
            ssid.trim();
            if (wifiSetCreds(ssid, pass)) {
                out.printf("[PROV] WiFi guardado: '%s' (clave %d caracteres)\n",
                           ssid.c_str(), pass.length());
                out.println(F("[PROV] Reinicia para conectar."));
            } else {
                out.println(F("[ERR]  SSID vacio o demasiado largo (max 32 / clave 64)."));
            }
        }

    } else if (cmd == "wifi_get") {
        String s = wifiGetSsid();
        if (!s.length()) {
            out.println(F("[PROV] Sin WiFi guardado."));
        } else {
            out.printf("[PROV] SSID: %s\n", s.c_str());
            // La clave solo por Serial. Por web quedaria expuesta a cualquiera
            // en la LAN, y este comando existe justo para copiarla a otras unidades.
            if (&out == &Serial) out.printf("[PROV] PASS: %s\n", wifiGetPass().c_str());
            else                 out.println(F("[PROV] PASS: (solo visible por USB serial)"));
        }

    } else if (cmd.startsWith("role_set ")) {
        String r = cmd.substring(9); r.trim();
        LinkRole nr = (r == "master") ? LINK_ROLE_MASTER
                    : (r == "link" || r == "slave") ? LINK_ROLE_SLAVE
                    : (r == "auto") ? LINK_ROLE_NONE
                    : (LinkRole)0xFF;
        if (nr == (LinkRole)0xFF) {
            out.println(F("[ERR]  Uso: role_set master|link|auto"));
        } else if (linkSetConfiguredRole(nr)) {
            out.printf("[PROV] Rol configurado: %s\n",
                       nr == LINK_ROLE_NONE ? "AUTO (se deduce al arrancar)" : linkRoleName(nr));
            out.println(F("[PROV] Reinicia para aplicarlo."));
        } else {
            out.println(F("[ERR]  No se pudo guardar el rol."));
        }

    } else if (cmd == "role_get") {
        LinkRole c = linkGetConfiguredRole();
        out.printf("[PROV] Rol configurado: %s\n",
                   c == LINK_ROLE_NONE ? "AUTO" : linkRoleName(c));
        out.printf("[PROV] Rol en ejecucion: %s\n", linkRoleName(linkGetRole()));

    } else if (cmd == "link_info") {
        linkPrintInfo(out);

    } else if (cmd == "link_pair") {
        if (linkGetRole() != LINK_ROLE_MASTER) {
            out.println(F("[ERR]  Solo el MASTER (el que tiene WiFi) abre emparejamiento."));
        } else {
            linkStartPairing(LINK_PAIR_WINDOW_MS);
            out.printf("[LINK] Emparejamiento abierto %d s.\n", LINK_PAIR_WINDOW_MS / 1000);
            out.println(F("[LINK] ENCIENDE AHORA la segunda unidad (sin WiFi configurado)."));
            out.println(F("[LINK] Consulta 'link_info' para ver si ya enlazo."));
        }

    } else if (cmd == "link_mac") {
        // Lo lee el flasher para enlazar las unidades sin radio
        out.printf("[LINK] MAC: %s\n", linkOwnMac().c_str());

    } else if (cmd.startsWith("link_master ")) {
        // En el HIJO: apuntar al master. Tras reiniciar se anuncia y el master
        // lo aprende solo — por eso el master no hay que conectarlo dos veces.
        String m = cmd.substring(12); m.trim();
        if (!linkAddPeerStr(m)) {
            out.println(F("[ERR]  MAC invalida, propia, o lista llena. "
                          "Uso: link_master AA:BB:CC:DD:EE:FF"));
        } else {
            linkSetConfiguredRole(LINK_ROLE_SLAVE);
            out.printf("[PROV] Master fijado: %s  (rol LINK)\n", m.c_str());
            out.println(F("[PROV] Reinicia: se anunciara y el master lo dara de alta."));
        }

    } else if (cmd.startsWith("link_child ")) {
        // En el MASTER: alta explicita de un hijo (alternativa al anuncio)
        String m = cmd.substring(11); m.trim();
        if (!linkAddPeerStr(m)) {
            out.println(F("[ERR]  MAC invalida, propia, o lista llena."));
        } else {
            out.printf("[PROV] Hijo añadido: %s  (%u en total)\n",
                       m.c_str(), linkPeerCount());
        }

    } else if (cmd == "link_reset") {
        linkClearPeers();
        out.println(F("[LINK] Enlaces borrados. El rol configurado se conserva."));

    } else if (cmd == "link_ping") {
        out.println(linkPing() ? F("[LINK] PING enviado a todos. Revisa 'link_info'.")
                               : F("[ERR]  Sin peers o ESP-NOW no iniciado."));

    } else if (cmd == "link_push_wifi") {
        if (!linkPeerCount()) {
            out.println(F("[ERR]  No hay peers enlazados."));
        } else {
            Preferences p; p.begin("wifi", true);
            String s = p.getString("ssid", ""), w = p.getString("pass", "");
            p.end();
            if (!s.length()) out.println(F("[ERR]  Esta unidad no tiene WiFi guardado."));
            else out.println(linkPushCreds(s, w)
                    ? F("[LINK] Credenciales enviadas a todos los peers.")
                    : F("[ERR]  Fallo el envio."));
        }

    } else if (cmd == "reboot") {
        // Reinicio limpio, sin borrar nada. Hace falta porque varios ajustes
        // (role_set, ch_count) solo se aplican al arrancar, y con todo por OTA
        // no siempre hay alguien delante para desenchufar la unidad.
        out.println(F("[SYS]  Reiniciando en 1 s..."));
        _reboot_at = millis() + 1000;

    } else if (cmd == "wifi_reset") {
        out.println(F("[WiFi] Borrando credenciales y reiniciando..."));
        wifiClearCreds();
        delay(500);
        ESP.restart();

    } else if (cmd.startsWith("drive ")) {
        String rest = cmd.substring(6); rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 0) { out.println(F("[ERR]  Uso: drive J<nn> HIGH|LOW")); return; }
        cmd_drive(rest.substring(0, sp), rest.substring(sp + 1), out);

    } else if (cmd.startsWith("scope_read ")) {
        cmd_scope_read(cmd.substring(11), out);

    } else {
        out.println(F("[ERR]  Comando desconocido. Escribe 'help'."));
    }
}

// =============================================================
void cmd_help(Print& out) {
    printLine(out);
    out.println(F("  Cable Tester 32CH v" FW_VERSION " — Comandos"));
    printLine(out);
    out.println(F("  help                 Lista de comandos"));
    out.println(F("  raw                  MODO DEV: primitivas crudas (raw_help)"));
    out.println(F("  rraw <1|2> <op> ...  Igual, pero en un hijo por ESP-NOW"));
    out.println(F("  scan_i2c             Escanear bus I2C"));
    out.println(F("  led_test             Walk 10 LEDs rojo/verde"));
    out.println(F("  expander_test        Walk J00-J31 vía TCA9535"));
    out.println(F("  all_input            Leer U1+U2 como input"));
    out.println(F("  drive J<nn> HIGH|LOW Drive Jnn (solo J00-J15)"));
    out.println(F("  read_all             Leer 32 canales"));
    out.println(F("  selftest             *** Continuidad AMBOS sentidos + reporte"));
    out.println(F("  rtest                *** Resistencia de contacto por linea"));
    out.println(F("  leaktest             *** Fugas / pines pegados a GND"));
    out.println(F("  loopback16           Continuidad solo CN1->CN2 (legacy)"));
    out.println(F("  scope_read J<nn>     ADC en canal Jnn"));
    out.println(F("  sr_dump              Mostrar shadow shift register"));
    printLine(out);
    out.println(F("  MAPEO POR GOLDEN (descubre el cableado, no hay que declararlo)"));
    out.println(F("  map                  Mapear el arnes conectado y mostrar nets"));
    out.println(F("  map_save <nombre>    Mapear y guardar como GOLDEN de referencia"));
    out.println(F("  map_check            Mapear y comparar contra el golden + LEDs"));
    out.println(F("  map_golden           Ver el golden guardado"));
    out.println(F("  map_json             Exportar el golden en JSON"));
    out.println(F("  map_clear            Borrar el golden"));
    out.println(F("  sku_set <SKU>        Vincular ESTE tester a un producto"));
    out.println(F("  sku_get              Ver SKU, metadatos y CRC del golden"));
    out.println(F("  meta_set <texto>     Fecha/operador/revision (los pone la PC)"));
    out.println(F("  tester_clear         LIBERAR el tester: borra golden + SKU"));
    printLine(out);
    printLine(out);
    out.println(F("  API JSON POR SERIAL (mismo contrato que /api/* por HTTP)"));
    out.println(F("  api_status           Estado completo en JSON"));
    out.println(F("  api_golden           Golden cargado en JSON"));
    out.println(F("  api_golden_set sku=X;job=Y;meta=Z;nets=0,1,1,...   (una linea)"));
    out.println(F("  api_test             Corrida completa -> JSON con fallas"));
    out.println(F("  api_last             Resultado de la ultima prueba"));
    out.println(F("  api_selfcheck        Validar modulos (arnes DESCONECTADO) -> JSON"));
    out.println(F("  api_map              Capturar mapa sin comparar ni guardar -> JSON"));
    out.println(F("  selfcheck            Igual que api_selfcheck, legible"));
    printLine(out);
    out.println(F("  LEDS Y DISPARADOR FISICO"));
    out.println(F("  leds [unidad] <mask> Encender LEDs (bits 0-4 rojo, 5-9 verde)"));
    out.println(F("  leds off             Apagar en todas las unidades"));
    out.println(F("  led_mode auto|manual Quien controla los LEDs tras una prueba"));
    out.println(F("  trigger <pin> [low|high] [avisar|local]  Switch fisico"));
    out.println(F("  trigger off          Desactivarlo"));
    printLine(out);
    out.println(F("  sysinfo              Salud del modulo: causa de reset, heap, uptime"));
    out.println(F("  wifi_info            IP y estado WiFi"));
    out.println(F("  wifi_reset           Borrar WiFi y volver al portal"));
    printLine(out);
    out.println(F("  ENLACE 2 UNIDADES (ESP-NOW)"));
    out.println(F("  link_info            Rol, MAC, peer y estado del enlace"));
    out.println(F("  link_pair            MASTER: abre 60 s para enlazar la 2a unidad"));
    out.println(F("  link_push_wifi       Reenvia el WiFi actual al peer"));
    out.println(F("  link_ping            Prueba de vida del peer"));
    out.println(F("  link_reset           Deshace el enlace"));
    printLine(out);
    out.println(F("  PROVISION POR LOTE"));
    out.println(F("  wifi_set <ssid>|<clave>   Guardar red sin usar el portal"));
    out.println(F("  wifi_get                  Ver red guardada (clave solo por USB)"));
    out.println(F("  role_set master|link|auto Fijar rol de forma persistente"));
    out.println(F("  role_get                  Ver rol configurado y en ejecucion"));
    printLine(out);
    out.println(F("  OTA: sube el .bin desde la web (boton 'Firmware OTA')"));
    printLine(out);
}

// =============================================================
void cmd_scan_i2c(Print& out) {
    out.println(F("[I2C]  Escaneando 0x01-0x7E..."));
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            out.printf("[I2C]  0x%02X encontrado", a);
            if (a == TCA9535_U1_ADDR) out.print(F("  ← U1 (J00-J15)"));
            if (a == TCA9535_U2_ADDR) out.print(F("  ← U2 (J16-J30+J_SPARE)"));
            if (a != TCA9535_U1_ADDR && a != TCA9535_U2_ADDR) out.print(F("  ← DESCONOCIDO"));
            out.println();
            found++;
        }
    }
    out.printf("[I2C]  Total: %d dispositivo(s)\n", found);
    if (!found)
        out.println(F("[I2C]  Ninguno. Verificar SDA/SCL, 3V3, pull-ups."));
}

// =============================================================
void cmd_led_test(Print& out) {
    out.println(F("[LED]  Walk 10 LEDs (rojo C1-C5, verde C1-C5)"));
    sr.setAllLeds(false); delay(150);

    const uint16_t reds[]   = {SR_LED1_RED,   SR_LED2_RED,   SR_LED3_RED,   SR_LED4_RED,   SR_LED5_RED};
    const uint16_t greens[] = {SR_LED1_GREEN, SR_LED2_GREEN, SR_LED3_GREEN, SR_LED4_GREEN, SR_LED5_GREEN};

    out.println(F("[LED]  --- Rojo ---"));
    for (int i = 0; i < 5; i++) {
        sr.setAllLeds(false);
        sr.setLeds(reds[i]);
        out.printf("[LED]  C%d_RED\n", i + 1);
        delay(120);
    }
    out.println(F("[LED]  --- Verde ---"));
    for (int i = 0; i < 5; i++) {
        sr.setAllLeds(false);
        sr.setLeds(greens[i]);
        out.printf("[LED]  C%d_GREEN\n", i + 1);
        delay(120);
    }
    sr.setAllLeds(true);
    out.println(F("[LED]  Todos ON")); delay(500);
    sr.setAllLeds(false);
    out.println(F("[LED]  Todos OFF"));
}

// =============================================================
void cmd_expander_test(Print& out) {
    if (!u1_ok || !u2_ok) {
        out.printf("[EXP]  ERROR: U1=%s U2=%s — verificar I2C y H2.\n",
                   u1_ok ? "OK" : "NO RESP", u2_ok ? "OK" : "NO RESP");
        return;
    }
    out.println(F("[EXP]  Walk U1 output → U2 input, luego U2 output → U1 input"));
    u1.setDirection(0x0000); u2.setDirection(0xFFFF);
    u1.writePort(0x0000);
    for (int i = 0; i < 16; i++) {
        u1.writePort(1u << i); delayMicroseconds(500);
        int32_t rd = u2.readPort();
        if (rd < 0) { out.printf("[EXP]  %s → ERROR I2C\n", jname(i).c_str()); }
        else {
            out.printf("[EXP]  %s driven HIGH", jname(i).c_str());
            if (rd > 0) out.printf("  [AVISO] U2=0x%04X (loopback?)", (uint16_t)rd);
            out.println();
        }
        u1.writePort(0x0000); delay(25);
    }
    u1.setDirection(0xFFFF); u2.setDirection(0x0000);
    u2.writePort(0x0000);
    for (int i = 0; i < 16; i++) {
        u2.writePort(1u << i); delayMicroseconds(500);
        int32_t rd = u1.readPort();
        if (rd < 0) { out.printf("[EXP]  %s → ERROR I2C\n", jname(i + 16).c_str()); }
        else {
            out.printf("[EXP]  %s driven HIGH", jname(i + 16).c_str());
            if (rd > 0) out.printf("  [AVISO] U1=0x%04X", (uint16_t)rd);
            out.println();
        }
        u2.writePort(0x0000); delay(25);
    }
    u1.setDirection(0x0000); u2.setDirection(0xFFFF);
    u1.writePort(0x0000);
    out.println(F("[EXP]  Restaurado: U1=OUT U2=IN."));
}

// =============================================================
void cmd_all_input(Print& out) {
    u1.setDirection(0xFFFF); delay(10);
    int32_t v1 = u1.readPort();
    int32_t v2 = u2.readPort();
    out.printf("[INP]  U1 (J00-J15): 0x%04X\n", (uint16_t)(v1 < 0 ? 0 : v1));
    out.printf("[INP]  U2 (J16-J31): 0x%04X\n", (uint16_t)(v2 < 0 ? 0 : v2));
    u1.setDirection(0x0000);
    out.println(F("[INP]  U1 restaurado como OUTPUT."));
}

// =============================================================
void cmd_drive(const String& pin_str, const String& level_str, Print& out) {
    int pin;
    if (!parseJpin(pin_str, pin)) { out.println(F("[ERR]  Uso: drive J<nn> HIGH|LOW")); return; }
    if (pin > 15)                 { out.println(F("[ERR]  Solo J00-J15 via U1.")); return; }
    bool high = (level_str == "high");
    u1.setPin(pin, high);
    out.printf("[DRV]  %s = %s\n", jname(pin).c_str(), high ? "HIGH" : "LOW");
}

// =============================================================
void cmd_read_all(Print& out) {
    int32_t v1 = u1.readPort(), v2 = u2.readPort();
    printLine(out);
    out.println(F("  Read All — 32 canales"));
    printLine(out);
    out.printf("  U1 J00-J15: 0x%04X\n", (uint16_t)(v1 < 0 ? 0 : v1));
    for (int i = 0; i < 16; i++)
        out.printf("    %s = %d\n", jname(i).c_str(), v1 < 0 ? -1 : (int)((v1 >> i) & 1));
    out.printf("  U2 J16-J31: 0x%04X\n", (uint16_t)(v2 < 0 ? 0 : v2));
    for (int i = 0; i < 16; i++)
        out.printf("    %s = %d\n", jname(i + 16).c_str(), v2 < 0 ? -1 : (int)((v2 >> i) & 1));
    printLine(out);
}

// =============================================================
void cmd_loopback(Print& out) {
    out.println(F("[LB]   Test continuidad — arnes J00<->J16 ... J15<->J_SPARE"));
    out.println(F("[LB]   Solo sentido CN1->CN2. Usa 'selftest' para ambos sentidos."));

    u1.setDirection(0x0000); u2.setDirection(0xFFFF);
    u1.writePort(0x0000); delay(10);

    int pass = 0, fail = 0, shorts = 0;
    for (int i = 0; i < 16; i++) {
        u1.writePort(1u << i); delayMicroseconds(500);
        int32_t rd = u2.readPort();
        if (rd < 0) {
            out.printf("[LB]   %s->%s : ERROR\n", jname(i).c_str(), jname(i + 16).c_str());
            fail++;
        } else {
            bool cont   = ((rd >> i) & 1) == 1;
            uint16_t ex = (uint16_t)rd & ~(1u << i);
            if (cont && !ex) {
                out.printf("[LB]   %s->%s : PASS\n", jname(i).c_str(), jname(i+16).c_str());
                pass++;
            } else if (!cont) {
                out.printf("[LB]   %s->%s : FALLA (abierto)\n", jname(i).c_str(), jname(i+16).c_str());
                fail++;
            } else {
                out.printf("[LB]   %s->%s : CORTO 0x%04X\n", jname(i).c_str(), jname(i+16).c_str(), ex);
                fail++; shorts++;
            }
        }
        u1.writePort(0x0000); delay(10);
    }
    printLine(out);
    out.printf("  PASS=%d  FAIL=%d  CORTOS=%d\n", pass, fail, shorts);
    out.println(fail == 0 ? F("  >>> LOOPBACK OK <<<") : F("  >>> REVISAR LÍNEAS <<<"));
    printLine(out);
    u1.writePort(0x0000);
}

// =============================================================
void cmd_scope_read(const String& pin_str, Print& out) {
    int ch;
    if (!parseJpin(pin_str, ch)) { out.println(F("[ERR]  Uso: scope_read J<nn>")); return; }
    if (ch > 30)                  { out.println(F("[ERR]  Canal max J30.")); return; }
    int mux_ch = (ch <= 15) ? ch : ch - 16;
    int mux_u  = (ch <= 15) ? 0 : 1;
    sr.selectMux(mux_ch, mux_u); delayMicroseconds(50);
    int mn = 4095, mx = 0, sum = 0;
    for (int i = 0; i < 64; i++) {
        int v = analogRead(PIN_ADC_SCOPE);
        sum += v; if (v < mn) mn = v; if (v > mx) mx = v;
        delayMicroseconds(100);
    }
    int avg = sum / 64;
    out.printf("[SCOPE] %s U%d-ch%d  avg=%.3fV  min=%.3fV  max=%.3fV\n",
               jname(ch).c_str(), mux_u ? 6 : 5, mux_ch,
               avg * 3.3f / 4095.0f, mn * 3.3f / 4095.0f, mx * 3.3f / 4095.0f);
    out.println((mx - mn) > 200 ? F("[SCOPE] Señal AC detectada") : F("[SCOPE] DC estable"));
    sr.selectMux(0, -1);
}

// =============================================================
void cmd_sr_dump(Print& out) {
    uint16_t v = sr.shadow();
    out.printf("[SR]  0x%04X\n", v);
    out.printf("[SR]  LEDs R: %d%d%d%d%d  G: %d%d%d%d%d\n",
               (v>>0)&1,(v>>1)&1,(v>>2)&1,(v>>3)&1,(v>>4)&1,
               (v>>5)&1,(v>>6)&1,(v>>7)&1,(v>>8)&1,(v>>9)&1);
    out.printf("[SR]  MUX S=%d%d%d%d  A_EN#=%d B_EN#=%d\n",
               (v>>10)&1,(v>>11)&1,(v>>12)&1,(v>>13)&1,(v>>14)&1,(v>>15)&1);
}

// =============================================================
// =============================================================
// sysinfo — salud del módulo. Pensado para validar placas recién
// ensambladas: si 'Reinicio' dice BROWNOUT o el heap minimo cae
// mucho, hay un problema de alimentacion o de soldadura.
// =============================================================
// =============================================================
// apiStatusJson — contrato de maquina para el software de PC.
//
// NO parsear los mensajes de texto de /cmd: son para humanos y cambian.
// Este endpoint es estable y va versionado en "api".
// =============================================================
void apiStatusJson(Print& out) {
    MapResult g; String gname;
    bool has_g = mapLoadGolden(g, gname);

    out.print(F("{"));
    out.printf("\"api\":%d,",           API_VERSION);
    out.printf("\"fw\":\"%s\",",        FW_VERSION);
    out.printf("\"unit\":\"%s\",",      wifiUnitId().c_str());
    out.printf("\"mac\":\"%s\",",       linkOwnMac().c_str());
    out.printf("\"role\":\"%s\",",      linkRoleName(linkGetRole()));
    out.printf("\"channels\":%u,",     mapGetChCount());
    out.printf("\"channels_local\":%d,", MAP_CH_LOCAL);

    // Peers: es lo que permite comprobar "master + 2 hijos en linea"
    out.print(F("\"peers\":["));
    for (uint8_t i = 0; i < linkPeerCount(); i++) {
        if (i) out.print(',');
        String m = linkPeerMacStr(i);
        String uid = m.substring(12); uid.replace(":", "");   // ultimos 2 bytes
        out.printf("{\"mac\":\"%s\",\"unit\":\"%s\",\"online\":%s,\"rssi\":%d}",
                   m.c_str(), uid.c_str(),
                   linkPeerOnline(i) ? "true" : "false",
                   (int)linkPeerRssi(i));
    }
    out.print(F("],"));
    out.printf("\"peer_count\":%u,",    linkPeerCount());
    out.printf("\"channels_total\":%d,", MAP_CH_LOCAL * (1 + linkPeerCount()));

    // sku identifica a QUE producto pertenece este tester (uno por SKU).
    // El PC debe rechazar la corrida si no coincide con lo que eligio el operador.
    out.printf("\"sku\":\"%s\",",  mapGetSku().c_str());
    out.printf("\"meta\":\"%s\",", mapGetMeta().c_str());

    // golden_ok=false con crc!=0 significa CORRUPTO, no ausente.
    out.printf("\"golden\":{\"loaded\":%s,\"name\":\"%s\",\"nets\":%u,\"crc\":%u},",
               has_g ? "true" : "false",
               has_g ? gname.c_str() : "",
               has_g ? g.net_count : 0,
               mapGoldenCrc());

    out.printf("\"expanders\":{\"u1\":%s,\"u2\":%s},",
               u1_ok ? "true" : "false", u2_ok ? "true" : "false");

    out.printf("\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},",
               wifiIsConnected() ? "true" : "false",
               wifiGetSsid().c_str(),
               wifiIsConnected() ? wifiGetIP().c_str() : "",
               wifiIsConnected() ? (int)WiFi.RSSI() : 0);

    // Control de LEDs y disparador fisico: la app necesita saber si los LEDs
    // los pone el firmware o ella, y si hay un switch conectado.
    out.printf("\"led_auto\":%s,", _led_auto ? "true" : "false");
    out.printf("\"trigger\":{\"pin\":%d,\"active_low\":%s,\"mode\":\"%s\",\"presses\":%u},",
               _trig_pin, _trig_low ? "true" : "false",
               _trig_mode == 0 ? "avisar" : "local", _trig_presses);
    // seq sube con CADA prueba, venga de la app o del switch fisico. La app
    // lo sondea aqui y, si cambio, recoge el detalle en /api/test/last.
    out.printf("\"last_test\":{\"seq\":%u,\"result\":\"%s\",\"faults\":%d,\"duration_ms\":%lu},",
               _last_seq, _last_seq ? _last_result : "NONE", _last_faults,
               (unsigned long)_last_ms);
    out.printf("\"uptime_s\":%lu,",     (unsigned long)(millis() / 1000));
    out.printf("\"reset_reason\":\"%s\",", resetReasonName());
    out.printf("\"heap_free\":%u",      (unsigned)ESP.getFreeHeap());
    out.println(F("}"));
}

// =============================================================
// apiGoldenGetJson — devuelve el job cargado (respaldo / verificacion)
// =============================================================
void apiGoldenGetJson(Print& out) {
    MapResult g; String gname;
    bool ok = mapLoadGolden(g, gname);

    out.print(F("{"));
    out.printf("\"api\":%d,", API_VERSION);
    out.printf("\"loaded\":%s,", ok ? "true" : "false");
    out.printf("\"sku\":\"%s\",",  mapGetSku().c_str());
    out.printf("\"job\":\"%s\",",  ok ? gname.c_str() : "");
    out.printf("\"meta\":\"%s\",", mapGetMeta().c_str());
    out.printf("\"channels\":%d,", MAP_CH_LOCAL);
    out.printf("\"crc\":%u,",      mapGoldenCrc());
    out.printf("\"nets_count\":%u,", ok ? g.net_count : 0);

    // Representacion cruda: es la que se vuelve a empujar tal cual
    out.print(F("\"nets_raw\":["));
    for (uint8_t i = 0; i < MAP_CH_LOCAL; i++) {
        if (i) out.print(',');
        out.print(ok ? g.net_of[i] : 0);
    }
    out.print(F("],"));

    // Representacion legible por nombre de canal
    out.print(F("\"nets\":["));
    bool first_net = true;
    for (uint8_t n = 1; ok && n <= g.net_count; n++) {
        if (!first_net) out.print(',');
        first_net = false;
        out.print('[');
        bool first = true;
        for (uint8_t i = 0; i < MAP_CH_LOCAL; i++) {
            if (g.net_of[i] != n) continue;
            if (!first) out.print(',');
            first = false;
            out.printf("\"M.%s\"", mapChName(i).c_str());
        }
        out.print(']');
    }
    out.println(F("]}"));
}

// =============================================================
// apiGoldenSet — la PC empuja el job antes de una corrida.
//
// Formato del cuerpo: lineas clave=valor. Deliberadamente NO es JSON:
// emitir JSON es trivial, parsearlo en el ESP32 exigiria una libreria de
// ~15 KB estando ya al 86% de la particion. La PC convierte entre su
// biblioteca JSON y este formato en tres lineas de Python.
//
//   sku=SKU4589
//   job=arnes-43p-rev2
//   meta=2026-08-21;operador:JGarcia
//   nets=0,1,1,2,2,0,3,3,...        (exactamente 32 valores, 0 = aislado)
// =============================================================
void apiGoldenSet(const String& body, Print& out) {
    String sku, job, meta, nets;

    int pos = 0;
    while (pos < (int)body.length()) {
        int nl = body.indexOf('\n', pos);
        if (nl < 0) nl = body.length();
        String line = body.substring(pos, nl);
        line.trim();
        pos = nl + 1;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if      (k == "sku")  sku  = v;
        else if (k == "job")  job  = v;
        else if (k == "meta") meta = v;
        else if (k == "nets") nets = v;
    }

    if (!nets.length()) {
        out.printf("{\"api\":%d,\"ok\":false,\"error\":\"falta nets=\"}\n", API_VERSION);
        return;
    }

    MapResult m;
    memset(&m, 0, sizeof(m));
    uint8_t count = 0, mx = 0;
    int p = 0;
    while (p < (int)nets.length() && count < MAP_CH_LOCAL) {
        int c = nets.indexOf(',', p);
        if (c < 0) c = nets.length();
        String tok = nets.substring(p, c); tok.trim();
        p = c + 1;
        if (!tok.length()) continue;
        long v = tok.toInt();
        if (v < 0 || v > 255) {
            out.printf("{\"api\":%d,\"ok\":false,\"error\":\"valor fuera de rango en nets\"}\n",
                       API_VERSION);
            return;
        }
        m.net_of[count++] = (uint8_t)v;
        if (v > mx) mx = (uint8_t)v;
    }

    if (count != MAP_CH_LOCAL) {
        out.printf("{\"api\":%d,\"ok\":false,\"error\":\"se esperaban %d canales, llegaron %u\"}\n",
                   API_VERSION, MAP_CH_LOCAL, count);
        return;
    }

    m.net_count = mx;
    // channels NUNCA se asignaba, y mapSaveGolden sale por "if (!m.valid ||
    // !m.channels) return false" sin grabar nada: POST /api/golden respondia
    // ok:false con crc:0 y el equipo se quedaba sin golden.
    //
    // No se noto porque la PC orquesta y nadie usaba esta ruta. Aparecio al
    // montar un job EN el equipo para que pueda trabajar sin PC.
    m.channels  = count;
    m.valid     = true;

    bool ok = mapSaveGolden(m, job.length() ? job : String("job"));
    if (sku.length())  mapSetSku(sku);
    if (meta.length()) mapSetMeta(meta);

    out.printf("{\"api\":%d,\"ok\":%s,\"sku\":\"%s\",\"job\":\"%s\","
               "\"channels\":%d,\"nets\":%u,\"crc\":%u}\n",
               API_VERSION, ok ? "true" : "false",
               mapGetSku().c_str(), job.c_str(),
               MAP_CH_LOCAL, m.net_count, mapGoldenCrc());
}

// =============================================================
// apiRunTest — una corrida completa, resultado estructurado.
//
// Es lo que consume el software de PC: mapea (local o distribuido segun
// los hijos enlazados), compara contra el golden y devuelve las fallas ya
// en nombres de canal. La PC traduce a posicion de conector con su tabla
// de fixture; el firmware no sabe de conectores.
// =============================================================
void apiRunTest(Print& out) {
    String err;
    if (runTestCore(err) < 0) {
        out.printf("{\"api\":%d,\"result\":\"ERROR\",\"error\":\"%s\"}\n",
                   API_VERSION, err.c_str());
        return;
    }
    apiLastTestJson(out);
}

// GET /api/test/last — el resultado de la ultima prueba, venga de donde venga.
//
// Es lo que permite que el switch fisico funcione sin perder trazabilidad:
// la app sondea 'seq' en /api/status y, cuando cambia, recoge aqui el detalle.
void apiLastTestJson(Print& out) {
    if (!_last_seq) {
        out.printf("{\"api\":%d,\"seq\":0,\"result\":\"NONE\"}\n", API_VERSION);
        return;
    }
    MapResult g; String gname;
    bool hasg = mapLoadGolden(g, gname);

    out.printf("{\"api\":%d,", API_VERSION);
    out.printf("\"seq\":%u,",      _last_seq);
    out.printf("\"sku\":\"%s\",",  mapGetSku().c_str());
    out.printf("\"job\":\"%s\",",  hasg ? gname.c_str() : "");
    out.printf("\"crc\":%u,",      mapGoldenCrc());
    out.printf("\"channels\":%u,", _last_dut.channels);
    out.printf("\"units\":%u,",    1 + linkPeerCount());
    out.print(F("\"faults\":"));
    if (hasg && g.channels == _last_dut.channels) mapFaultsJson(g, _last_dut, out);
    else out.print(F("[]"));
    out.printf(",\"result\":\"%s\"", _last_result);
    out.printf(",\"duration_ms\":%lu}\n", (unsigned long)_last_ms);
}

// GET /api/selfcheck — validar los modulos con el arnes DESCONECTADO.
// Es el paso previo a capturar un golden: si un pin esta pegado a GND, el
// golden lo congelaria como "asi es este producto" y todos los arneses
// buenos serian rechazados despues.
void apiSelfCheckJson(Print& out) {
    static uint8_t st[MAP_CH_MAX];
    NullPrint quiet;
    int bad = mapDistSelfCheck(u1, u2, st, quiet);
    const uint8_t N = mapDistChannels();

    out.printf("{\"api\":%d,\"channels\":%u,\"units\":%u,\"bad\":%d,\"result\":\"%s\",",
               API_VERSION, N, 1 + linkPeerCount(), bad, bad == 0 ? "PASS" : "FAIL");
    out.print(F("\"faults\":["));
    bool first = true;
    for (uint8_t g = 0; g < N; g++) {
        if (st[g] == CHK_OK) continue;
        if (!first) out.print(',');
        first = false;
        const char* t = st[g] == CHK_STUCK_LOW  ? "stuck_low"
                      : st[g] == CHK_STUCK_HIGH ? "stuck_high" : "error";
        out.printf("{\"ch\":\"%s\",\"type\":\"%s\"}", mapChName(g).c_str(), t);
    }
    out.println(F("]}"));
}

// GET /api/map — capturar el mapa SIN compararlo ni guardarlo.
// Lo usa el asistente para que ingenieria revise antes de aprobar.
void apiMapJson(Print& out) {
    MapResult m;
    NullPrint quiet;
    if (!mapDistRun(u1, u2, m, quiet)) {
        out.printf("{\"api\":%d,\"ok\":false,\"error\":\"fallo el mapeo\"}\n", API_VERSION);
        return;
    }
    out.printf("{\"api\":%d,\"ok\":true,\"channels\":%u,\"units\":%u,\"nets_count\":%u,",
               API_VERSION, m.channels, 1 + linkPeerCount(), m.net_count);
    out.print(F("\"nets_raw\":["));
    for (uint8_t i = 0; i < m.channels; i++) { if (i) out.print(','); out.print(m.net_of[i]); }
    out.print(F("],\"nets\":["));
    bool fn = true;
    for (uint8_t n = 1; n <= m.net_count; n++) {
        if (!fn) out.print(','); fn = false;
        out.print('[');
        bool f = true;
        for (uint8_t i = 0; i < m.channels; i++) {
            if (m.net_of[i] != n) continue;
            if (!f) out.print(','); f = false;
            out.printf("\"%s\"", mapChName(i).c_str());
        }
        out.print(']');
    }
    out.println(F("]}"));
}

void cmd_sysinfo(Print& out) {
    printLine(out);
    out.println(F("  SYSINFO"));
    printLine(out);
    out.printf("  Unidad       : %s\n", wifiUnitId().c_str());
    out.printf("  MAC          : %s\n", WiFi.macAddress().c_str());
    out.printf("  Chip         : %s rev%d, %d core(s)\n",
               ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    out.printf("  Flash        : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
    out.printf("  Firmware     : %u KB\n", (unsigned)(ESP.getSketchSize() / 1024));
    // RSSI al router: la forma mas simple de comparar el rendimiento de la
    // antena antes y despues de recortar el FR4 bajo ella. Mismo sitio, misma
    // distancia, y se mira cuanto cambia el numero.
    if (wifiIsConnected()) {
        int r = WiFi.RSSI();
        const char* q = r > -60 ? "excelente" : r > -75 ? "bueno"
                      : r > -85 ? "justo" : "MALO";
        out.printf("  RSSI al AP   : %d dBm (%s)\n", r, q);
    }
    printLine(out);
    out.printf("  Ultimo reset : %s\n", resetReasonName());
    uint32_t s = millis() / 1000;
    out.printf("  Encendido    : %luh %02lum %02lus\n",
               (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
               (unsigned long)(s % 60));
    printLine(out);
    out.printf("  Heap libre   : %u B\n",  (unsigned)ESP.getFreeHeap());
    out.printf("  Heap min hist: %u B  <- si cae mucho, hay fuga o presion\n",
               (unsigned)ESP.getMinFreeHeap());
    printLine(out);
    out.printf("  Expansores   : U1=%s  U2=%s\n",
               u1_ok ? "OK" : "NO RESP", u2_ok ? "OK" : "NO RESP");
    printLine(out);
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        out.println(F("  *** BROWNOUT: la alimentacion se hundio. Revisa"));
        out.println(F("  *** soldadura de VCC/GND y capacitores de desacoplo."));
        printLine(out);
    }
}

void cmd_wifi_info(Print& out) {
    if (wifiIsConnected()) {
        out.printf("[WiFi] Conectado. IP: %s\n", wifiGetIP().c_str());
        out.printf("[WiFi] Web UI: http://%s/\n", wifiGetIP().c_str());
    } else if (wifiIsPortalActive()) {
        out.printf("[WiFi] Portal activo. SSID: %s\n", wifiGetApSsid().c_str());
        out.println(F("[WiFi] http://192.168.4.1/"));
    } else {
        out.println(F("[WiFi] Desconectado."));
    }
    out.printf("[WiFi] Unidad: %s\n", wifiUnitId().c_str());
}

// =============================================================
bool parseJpin(const String& s, int& out_pin) {
    String t = s; t.trim();
    if (!t.length()) return false;
    if (t[0] == 'j' || t[0] == 'J') t.remove(0, 1);
    if (!t.length()) return false;
    out_pin = t.toInt();
    return true;
}

void printLine(Print& out, int n) {
    for (int i = 0; i < n; i++) out.print('-');
    out.println();
}

String jname(int n) {
    if (n == 31) return String("J_SP");
    char buf[6]; snprintf(buf, sizeof(buf), "J%02d", n);
    return String(buf);
}
