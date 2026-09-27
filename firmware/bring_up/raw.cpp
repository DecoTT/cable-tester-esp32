#include "raw.h"
#include "config.h"
#include <Wire.h>
#include "link.h"
#include "mapping.h"

static TCA9535* _u1 = nullptr;
static TCA9535* _u2 = nullptr;
static Shift595* _sr = nullptr;

void rawBegin(TCA9535* u1, TCA9535* u2, Shift595* sr) {
    _u1 = u1; _u2 = u2; _sr = sr;
}

const char* rawOpName(uint8_t op) {
    switch (op) {
        case RAW_NOP:      return "nop";
        case RAW_IO_DIR:   return "io_dir";
        case RAW_IO_WR:    return "io_wr";
        case RAW_IO_RD:    return "io_rd";
        case RAW_SR_WR:    return "sr_wr";
        case RAW_ADC_RD:   return "adc_rd";
        case RAW_DELAY_US: return "delay_us";
        case RAW_I2C_RD:   return "i2c_rd";
        case RAW_I2C_WR:   return "i2c_wr";
        case RAW_LINKSTAT: return "linkstat";
        case RAW_SAMPLE:   return "sample";
        case RAW_LEDS:     return "leds";
        case RAW_RGB_OFF:  return "rgb_off";
        default:           return "?";
    }
}

static TCA9535* _unit(uint8_t a) {
    if (a == 0) return _u1;
    if (a == 1) return _u2;
    return nullptr;
}

// Lectura cruda del MUX en milivoltios, sin corregir la ganancia del ADC.
static int32_t _adcRead(uint8_t side, uint8_t ch) {
    if (!_sr || ch > 15 || side > 1) return RAW_ERR;
    _sr->selectMux(ch, side);
    delayMicroseconds(50);
    analogRead(PIN_ADC_SCOPE);            // descartar la primera muestra
    uint32_t sum = 0;
    for (uint8_t i = 0; i < ADC_AVG_SAMPLES; i++) sum += analogRead(PIN_ADC_SCOPE);
    float counts = (float)sum / ADC_AVG_SAMPLES;
    return (int32_t)(counts * ADC_VREF * 1000.0f / ADC_FULL_SCALE);
}

static int32_t _i2cRead(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return RAW_ERR;
    if (Wire.requestFrom((int)addr, 1) != 1) return RAW_ERR;
    return (int32_t)Wire.read();
}

static int32_t _i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return (Wire.endTransmission() == 0) ? 0 : RAW_ERR;
}

uint8_t rawExec(const RawOpCode* ops, uint8_t n, int32_t* results, bool* ok) {
    if (n > RAW_MAX_OPS) n = RAW_MAX_OPS;
    uint8_t done = 0;

    for (uint8_t i = 0; i < n; i++) {
        const RawOpCode& o = ops[i];
        int32_t r = RAW_ERR;
        bool    good = true;
        TCA9535* u = nullptr;

        switch (o.op) {
            case RAW_NOP:
                r = 0;
                break;

            case RAW_IO_DIR:
                u = _unit(o.a);
                if (u) r = u->setDirection(o.b) ? 0 : RAW_ERR;
                break;

            case RAW_IO_WR:
                u = _unit(o.a);
                if (u) r = u->writePort(o.b) ? 0 : RAW_ERR;
                break;

            case RAW_IO_RD:
                u = _unit(o.a);
                if (u) {
                    int32_t v = u->readPort();
                    r = (v < 0) ? RAW_ERR : v;
                }
                break;

            case RAW_SR_WR:
                if (_sr) { _sr->write(o.b); r = 0; }
                break;

            case RAW_ADC_RD:
                r = _adcRead(o.a, (uint8_t)o.b);
                break;

            case RAW_DELAY_US:
                delayMicroseconds(o.b);
                r = 0;
                break;

            case RAW_I2C_RD:
                r = _i2cRead(o.a, (uint8_t)o.b);
                break;

            case RAW_I2C_WR:
                r = _i2cWrite(o.a, (uint8_t)(o.b >> 8), (uint8_t)(o.b & 0xFF));
                break;

            case RAW_LEDS:
                if (_sr) { _sr->setAllLeds(false); _sr->setLeds(o.b); r = 0; }
                else good = false;
                break;

            case RAW_RGB_OFF: {
                // El WS2812 del modulo cuelga de IO8, que es TAMBIEN el reloj
                // del I2C. Cada transaccion del bus le mete pulsos que el LED
                // interpreta como color: de ahi que se encienda "aleatorio".
                // No es aleatorio, es el trafico del bus.
                //
                // Apagarlo solo funciona con el bus EN REPOSO. Se suelta el I2C,
                // se manda una trama de ceros y se devuelve el pin a Wire. Si se
                // hiciera a mitad de una transaccion se corromperia la lectura.
                Wire.end();
                pinMode(PIN_RGB_LED, OUTPUT);
                rgbLedWrite(PIN_RGB_LED, 0, 0, 0);
                delayMicroseconds(300);            // latch del WS2812
                Wire.begin(PIN_SDA, PIN_SCL);
                r = 0;
                break;
            }

            case RAW_LINKSTAT:
                r = linkStat(o.a);
                break;

            case RAW_SAMPLE:
                if (_u1 && _u2) {
                    bool sok = false;
                    uint32_t m = mapSampleLocal(*_u1, *_u2,
                                                (int)o.a - 1, o.b != 0, &sok);
                    r = (int32_t)m;      // cualquier patron de 32 bits es valido
                    good = sok;
                } else good = false;
                break;

            default:
                // Op desconocida: se corta aquí en vez de seguir ejecutando
                // una secuencia que ya no es la que pidió quien la compuso.
                if (results) results[i] = RAW_ERR;
                return done;
        }

        if (results) results[i] = r;
        if (ok)      ok[i] = good && (r != RAW_ERR || o.op == RAW_SAMPLE);
        done++;
    }
    return done;
}

// "io_dir 0 0xFFFE" / "io_rd 1" / "delay_us 1000"
bool rawParse(const String& args, RawOpCode& out, String& err) {
    String s = args; s.trim();
    if (!s.length()) { err = "falta la operacion"; return false; }

    String tok[3];
    uint8_t nt = 0;
    int start = 0;
    while (nt < 3 && start < (int)s.length()) {
        int sp = s.indexOf(' ', start);
        if (sp < 0) sp = s.length();
        String t = s.substring(start, sp); t.trim();
        if (t.length()) tok[nt++] = t;
        start = sp + 1;
    }

    out.op = 0xFF;
    for (uint8_t i = 0; i <= RAW_RGB_OFF; i++)
        if (tok[0].equalsIgnoreCase(rawOpName(i))) { out.op = i; break; }
    if (out.op == 0xFF) { err = "operacion desconocida: " + tok[0]; return false; }

    // strtol con base 0 acepta 0x1234 y 4660 indistintamente
    out.a = (nt > 1) ? (uint8_t)strtol(tok[1].c_str(), nullptr, 0) : 0;
    out.b = (nt > 2) ? (uint16_t)strtol(tok[2].c_str(), nullptr, 0) : 0;

    // Los ops de un solo argumento lo llevan en 'b', no en 'a'
    if (out.op == RAW_SR_WR || out.op == RAW_DELAY_US || out.op == RAW_LEDS) {
        if (nt < 2) { err = "falta el valor"; return false; }
        out.b = (uint16_t)strtol(tok[1].c_str(), nullptr, 0);
        out.a = 0;
    }
    return true;
}

void rawPrintHelp(Print& out) {
    out.println(F("  MODO DEV — primitivas crudas"));
    out.println(F("  ------------------------------------------------"));
    out.println(F("  raw  <op> [a] [b]        en esta unidad"));
    out.println(F("  rraw <unidad> <op> ...   en un hijo (1=L1, 2=L2)"));
    out.println();
    out.println(F("  io_dir <u> <mask>   u: 0=U1(J00-J15) 1=U2(J16-J_SP)"));
    out.println(F("                      mask: bit 1 = entrada"));
    out.println(F("  io_wr  <u> <val>    escribe el puerto"));
    out.println(F("  io_rd  <u>          lee el puerto"));
    out.println(F("  sr_wr  <val16>      registro de desplazamiento crudo"));
    out.println(F("  adc_rd <lado> <ch>  lado: 0=CN1 1=CN2 — mV SIN corregir"));
    out.println(F("  delay_us <n>"));
    out.println(F("  i2c_rd <addr> <reg>"));
    out.println(F("  i2c_wr <addr> <(reg<<8)|val>"));
    out.println(F("  leds <mascara>      bits 0-9; par=verde, impar=rojo"));
    out.println(F("  rgb_off             apaga el RGB (lo enciende el trafico I2C)"));
    out.println(F("  linkstat <0-6>      0=canal 1=SAMPLE rx 2=MASK tx"));
    out.println(F("                      3=ms sin master 5=enlazado 6=canal excitado"));
    out.println();
    out.println(F("  Los valores aceptan decimal o 0x hex."));
    out.println(F("  OJO: dos pines como salida se pelean. 680 ohm limitan a"));
    out.println(F("  ~5 mA (no se daña nada) pero la lectura no vale."));
}
