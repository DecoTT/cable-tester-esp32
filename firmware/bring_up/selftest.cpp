#include "selftest.h"
#include "config.h"

// Helpers definidos en bring_up.ino
extern String jname(int n);
extern void   printLine(Print& out, int n);

static const int PAIRS = 16;

// Nombre de la línea del lado que manda / del lado que escucha según el sentido
static String srcName(int dir, int i) { return jname(dir == 0 ? i : i + 16); }
static String dstName(int dir, int i) { return jname(dir == 0 ? i + 16 : i); }

// =============================================================
// Barrido direccional
// =============================================================
void stLoopDirection(TCA9535& u1, TCA9535& u2, int dir, Print& out, LoopResult& res) {
    res = {0, 0, 0, 0, 0};

    TCA9535& drv = (dir == 0) ? u1 : u2;
    TCA9535& rcv = (dir == 0) ? u2 : u1;

    out.printf("[LB]   Sentido: %s manda → %s escucha\n",
               dir == 0 ? "CN1" : "CN2", dir == 0 ? "CN2" : "CN1");

    drv.setDirection(0x0000);   // todo salida
    rcv.setDirection(0xFFFF);   // todo entrada
    drv.writePort(0x0000);
    delay(5);

    for (int i = 0; i < PAIRS; i++) {
        drv.writePort(1u << i);
        delayMicroseconds(500);
        int32_t rd = rcv.readPort();

        if (rd < 0) {
            out.printf("[LB]   %s -> %s : ERROR I2C\n",
                       srcName(dir, i).c_str(), dstName(dir, i).c_str());
            res.i2c_err++;
            res.fail_mask |= (1u << i);
        } else {
            bool     cont  = ((rd >> i) & 1) == 1;
            uint16_t extra = (uint16_t)rd & ~(1u << i);

            if (cont && !extra) {
                res.pass++;
            } else if (!cont && !extra) {
                out.printf("[LB]   %s -> %s : ABIERTO\n",
                           srcName(dir, i).c_str(), dstName(dir, i).c_str());
                res.open++;
                res.fail_mask |= (1u << i);
            } else {
                // Alguna línea ajena se activó → corto o ruteo cruzado
                out.printf("[LB]   %s -> %s : %s + cruce con",
                           srcName(dir, i).c_str(), dstName(dir, i).c_str(),
                           cont ? "OK" : "ABIERTO");
                for (int j = 0; j < PAIRS; j++)
                    if ((extra >> j) & 1) out.printf(" %s", dstName(dir, j).c_str());
                out.println();
                res.cross++;
                res.fail_mask |= (1u << i) | extra;
            }
        }
        drv.writePort(0x0000);
        delay(8);
    }

    out.printf("[LB]   %s: PASS=%d ABIERTOS=%d CRUCES=%d I2C_ERR=%d\n",
               dir == 0 ? "CN1→CN2" : "CN2→CN1",
               res.pass, res.open, res.cross, res.i2c_err);
}

// =============================================================
// Prueba completa bidireccional
// =============================================================
void stFullLoopback(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out) {
    printLine(out, 56);
    out.println(F("  SELF-TEST — Arnés 1:1  (CN1 J00-J15 ↔ CN2 J16-J_SPARE)"));
    printLine(out, 56);

    sr.setAllLeds(false);

    LoopResult fwd, rev;
    stLoopDirection(u1, u2, 0, out, fwd);
    out.println();
    stLoopDirection(u1, u2, 1, out, rev);

    // --- Reporte consolidado ---
    uint16_t bad = fwd.fail_mask | rev.fail_mask;
    int total_fail = 0;
    for (int i = 0; i < PAIRS; i++) if ((bad >> i) & 1) total_fail++;

    out.println();
    printLine(out, 56);
    out.println(F("  RESUMEN POR LÍNEA"));
    printLine(out, 56);
    for (int i = 0; i < PAIRS; i++) {
        bool f_ok = !((fwd.fail_mask >> i) & 1);
        bool r_ok = !((rev.fail_mask >> i) & 1);
        out.printf("  %s <-> %s   ida:%s  vuelta:%s   %s\n",
                   jname(i).c_str(), jname(i + 16).c_str(),
                   f_ok ? "OK  " : "FALLA",
                   r_ok ? "OK  " : "FALLA",
                   (f_ok && r_ok) ? "PASS" : "<<< REVISAR");
    }

    printLine(out, 56);
    out.printf("  Líneas OK: %d/%d\n", PAIRS - total_fail, PAIRS);
    if (total_fail == 0) {
        out.println(F("  >>> ARNÉS Y PCB OK <<<"));
        sr.setLeds(SR_LED1_GREEN | SR_LED2_GREEN | SR_LED3_GREEN |
                   SR_LED4_GREEN | SR_LED5_GREEN);
    } else {
        out.printf("  >>> %d LÍNEA(S) CON FALLA <<<\n", total_fail);
        out.println(F("  Nota: un corto entre dos líneas del mismo conector puede"));
        out.println(F("  reportarse como ABIERTO — ver 'rtest' para diagnóstico fino."));
        sr.setLeds(SR_LED1_RED | SR_LED2_RED | SR_LED3_RED |
                   SR_LED4_RED | SR_LED5_RED);
    }
    printLine(out, 56);

    // Estado de reposo
    u1.setDirection(0x0000); u1.writePort(0x0000);
    u2.setDirection(0xFFFF);
}

// =============================================================
// Resistencia de contacto
//
//   3V3 ─[680Ω]─ CN1_J(i) ─[cable Rc]─ CN2_J(i+16) ─[680Ω]─ GND
//                   ↑ U5 ch i              ↑ U6 ch i
//
//   I  = Vcn2 / 680
//   Rc = (Vcn1 - Vcn2) / I = (Vcn1 - Vcn2) · 680 / Vcn2
//
// Al ser medición diferencial entre dos canales, la no-linealidad
// absoluta del ADC del ESP32-C3 se cancela en buena medida.
// =============================================================
static float readMuxVolts(Shift595& sr, uint8_t ch, int unit) {
    sr.selectMux(ch, unit);
    delayMicroseconds(200);          // settling del MUX + RC (R47·C10)
    analogRead(PIN_ADC_SCOPE);       // descartar primera muestra
    uint32_t sum = 0;
    for (int k = 0; k < ADC_AVG_SAMPLES; k++) {
        sum += analogRead(PIN_ADC_SCOPE);
        delayMicroseconds(60);
    }
    return (sum / (float)ADC_AVG_SAMPLES) * ADC_VREF / ADC_FULL_SCALE;
}

// =============================================================
// Fugas / pines pegados a GND
//
//   3V3 ─[680Ω]─ J(i) ─── cable ─── J(i+16)  (extremo lejano en alta-Z)
//                  ↑ MUX                ↑ MUX
//
// Línea sana: ambos extremos ≈ 3.3 V (no circula corriente, no hay caída).
// Con fuga R a GND: V = 3.3·R/(R+680)  →  R = V·680/(3.3−V)
//
// Un pin del expander pegado a GND aparece como R ≈ 680 Ω, porque el resistor
// serie de esa línea queda entre el conector y el pin aterrizado.
// =============================================================
void stLeakTest(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out) {
    printLine(out, 64);
    out.println(F("  FUGAS / PINES PEGADOS — extremo lejano en alta impedancia"));
    printLine(out, 64);
    out.println(F("  Sana = ~3.3 V en ambos extremos. R≈680 ohm => pin a GND."));
    printLine(out, 64);
    out.println(F("  Linea         V_CN1    V_CN2    R_fuga    Estado"));
    printLine(out, 64);

    int ok = 0, bad = 0;

    for (int dir = 0; dir < 2; dir++) {
        TCA9535& drv = (dir == 0) ? u1 : u2;
        TCA9535& rcv = (dir == 0) ? u2 : u1;

        out.printf("  --- %s sube, %s en alta-Z ---\n",
                   dir == 0 ? "CN1" : "CN2", dir == 0 ? "CN2" : "CN1");

        rcv.setDirection(0xFFFF);          // alta impedancia PRIMERO
        drv.writePort(0x0000);
        drv.setDirection(0x0000);
        delay(5);

        for (int i = 0; i < PAIRS; i++) {
            drv.writePort(1u << i);
            delay(2);

            float v1 = readMuxVolts(sr, i, 0);   // lado CN1
            float v2 = readMuxVolts(sr, i, 1);   // lado CN2
            // El extremo que manda es el que mejor refleja la fuga
            float v  = (dir == 0) ? v1 : v2;
            float vc = v / ADC_GAIN_ERR;         // corregido por error de ganancia

            char r_str[12];
            const char* estado;

            if (vc >= LEAK_OK_VOLT) {
                snprintf(r_str, sizeof(r_str), "    inf");
                estado = "OK";
                ok++;
            } else {
                float r = vc * R_SERIES_OHM / (ADC_VREF - vc);
                snprintf(r_str, sizeof(r_str), "%7.0f", r);
                // ~680Ω es la firma de un pin del expander pegado a GND
                estado = (r > 0.75f * R_SERIES_OHM && r < 1.25f * R_SERIES_OHM)
                         ? "PIN A GND (680R)" : "FUGA A GND";
                bad++;
            }

            out.printf("  %s<->%s  %6.3f   %6.3f   %s   %s\n",
                       jname(i).c_str(), jname(i + 16).c_str(), v1, v2, r_str, estado);

            drv.writePort(0x0000);
            delay(2);
        }
        out.println();
    }

    sr.selectMux(0, -1);
    printLine(out, 64);
    out.printf("  OK=%d  CON FUGA=%d  (de %d mediciones)\n", ok, bad, PAIRS * 2);
    printLine(out, 64);

    u1.setDirection(0x0000); u1.writePort(0x0000);
    u2.setDirection(0xFFFF);
}

void stContactResistance(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out) {
    printLine(out, 64);
    out.println(F("  RESISTENCIA DE CONTACTO — divisor 680Ω/680Ω vía MUX+ADC"));
    printLine(out, 64);
    out.println(F("  Línea         V_CN1    V_CN2    Rc(ohm)   Estado"));
    printLine(out, 64);

    int ok = 0, warn = 0, bad = 0;

    for (int i = 0; i < PAIRS; i++) {
        // U1 maneja la línea i en HIGH, el resto en LOW
        u1.setDirection(0x0000);
        u1.writePort(1u << i);
        // U2: solo la línea i como salida en LOW, el resto en alta impedancia
        u2.setDirection((uint16_t)(0xFFFF & ~(1u << i)));
        u2.writePort(0x0000);
        delay(3);

        float v1 = readMuxVolts(sr, i, 0);   // U5 → lado CN1
        float v2 = readMuxVolts(sr, i, 1);   // U6 → lado CN2

        const char* estado;
        char rc_str[12];

        if (v2 < 0.30f && v1 > 2.50f) {
            snprintf(rc_str, sizeof(rc_str), "  ----");
            estado = "ABIERTO";
            bad++;
        } else if (v1 < 0.30f) {
            snprintf(rc_str, sizeof(rc_str), "  ----");
            estado = "CORTO A GND";
            bad++;
        } else {
            float rc = (v1 - v2) * R_SERIES_OHM / v2;
            if (rc < 0) rc = 0;
            snprintf(rc_str, sizeof(rc_str), "%7.1f", rc);
            if (rc <= RC_OK_OHM)        { estado = "OK";              ok++;   }
            else if (rc <= RC_WARN_OHM) { estado = "ALTA (revisar)";  warn++; }
            else                        { estado = "MALA CONEXION";   bad++;  }
        }

        out.printf("  %s<->%s  %6.3f   %6.3f   %s   %s\n",
                   jname(i).c_str(), jname(i + 16).c_str(), v1, v2, rc_str, estado);
    }

    sr.selectMux(0, -1);
    printLine(out, 64);
    out.printf("  OK=%d  ALTA=%d  MALA=%d   (umbral OK <%.0fΩ, alta <%.0fΩ)\n",
               ok, warn, bad, (double)RC_OK_OHM, (double)RC_WARN_OHM);
    out.println(F("  Si TODAS dan V_CN1=3.3 y V_CN2=0.0, los MUX U5/U6 están"));
    out.println(F("  conectados del lado del IC y no del conector — revisar ruteo."));
    printLine(out, 64);

    u1.setDirection(0x0000); u1.writePort(0x0000);
    u2.setDirection(0xFFFF);
}
