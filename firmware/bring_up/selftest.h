#pragma once
#include <Arduino.h>
#include "tca9535.h"
#include "shift595.h"

// =============================================================
// selftest.h — Pruebas de validación de arnés y PCB
//
// Topología asumida (CONFIRMADA desde esquemático Rev1.0):
//   Arnés 1:1 (pin N de CN1 → pin N de CN2)  ⇒  J(i) ↔ J(i+16)
//   U1 bit i = J(i)  |  U2 bit i = J(i+16)
//   GND en pines 5/10/15/20 de ambos conectores (no se testean)
// =============================================================

// Resultado de un barrido direccional
struct LoopResult {
    int  pass;
    int  open;      // línea no llegó al otro extremo
    int  cross;     // llegó, pero además apareció otra línea (corto/ruteo)
    int  i2c_err;
    uint16_t fail_mask;   // bit i = línea i falló
};

// Barrido en un sentido.
//   dir = 0 → CN1 manda (U1 out), CN2 escucha (U2 in)
//   dir = 1 → CN2 manda (U2 out), CN1 escucha (U1 in)
void stLoopDirection(TCA9535& u1, TCA9535& u2, int dir, Print& out, LoopResult& res);

// Prueba completa: ambos sentidos + reporte consolidado.
void stFullLoopback(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out);

// Medición de resistencia de contacto por línea vía divisor + MUX/ADC.
// Detecta crimps flojos y soldaduras frías que el test digital pasa por alto.
//
// OJO — punto ciego conocido: fuerza el extremo lejano como salida en LOW para
// armar el divisor. Un pin PEGADO a GND es indistinguible de uno sano en esa
// configuración. Correr SIEMPRE 'leaktest' junto con este.
void stContactResistance(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out);

// Fugas / pines pegados: sube cada línea con el extremo lejano en ALTA IMPEDANCIA
// y verifica que llegue al riel. Una línea sana lee ~3.3 V; cualquier valor menor
// implica una resistencia parásita a GND, cuyo valor se reporta.
// Es la prueba que 'rtest' no puede hacer por construcción.
void stLeakTest(TCA9535& u1, TCA9535& u2, Shift595& sr, Print& out);
