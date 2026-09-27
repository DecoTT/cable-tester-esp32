#pragma once
#include <Arduino.h>
#include "tca9535.h"
#include "shift595.h"

// =============================================================
// raw.h — Capa de primitivas ("modo dev")
//
// No interpreta nada: expone el hardware tal cual para que la PC componga
// experimentos que no estaban previstos al escribir este firmware. Los
// comandos de alto nivel (map, selfcheck, leaktest, rtest) siguen igual;
// esto es una vía paralela, no un reemplazo.
//
// Por qué existe: cada idea nueva de diagnóstico obligaba a reflashear. Con
// estas primitivas la app de PC expresa el experimento y el firmware solo
// ejecuta, así que deja de quedarse obsoleto por cosas que no se nos han
// ocurrido todavía.
//
// ⚠️ CONTENCIÓN: con estas primitivas se pueden poner dos pines como salida
// peleándose entre sí. Los 680 Ω en serie limitan la corriente a ~5 mA, así
// que no se daña nada — pero una lectura hecha en esa situación no significa
// nada. Es responsabilidad de quien compone la secuencia, no del firmware.
//
// ⚠️ El ADC se devuelve SIN corregir por ADC_GAIN_ERR. Es crudo a propósito:
// la corrección la aplica la PC, que así puede revisarla o cambiarla sin
// tocar el firmware.
// =============================================================

enum RawOp : uint8_t {
    RAW_NOP      = 0x00,
    RAW_IO_DIR   = 0x01,  // a = 0:U1 1:U2   b = máscara (bit 1 = entrada)
    RAW_IO_WR    = 0x02,  // a = 0:U1 1:U2   b = valor a escribir
    RAW_IO_RD    = 0x03,  // a = 0:U1 1:U2   → valor leído (16 bits)
    RAW_SR_WR    = 0x04,  //                 b = 16 bits crudos (LEDs + MUX)
    RAW_ADC_RD   = 0x05,  // a = 0:CN1 1:CN2 b = canal 0-15 → milivoltios crudos
    RAW_DELAY_US = 0x06,  //                 b = microsegundos
    RAW_I2C_RD   = 0x07,  // a = dirección   b = registro → byte
    RAW_I2C_WR   = 0x08,  // a = dirección   b = (registro << 8) | valor
    RAW_LINKSTAT = 0x09,  // a = qué dato (ver linkStat en link.h)
    RAW_RGB_OFF  = 0x0C,  // apaga el LED RGB del modulo. Ver raw.cpp: comparte
                          // pin con el reloj del I2C, asi que el trafico del bus
                          // lo enciende con basura.
    RAW_LEDS     = 0x0B,  // b = mascara de LEDs (bits 0-9). Preserva el MUX:
                          // escribir el registro entero con sr_wr borraria la
                          // seleccion de MUX y dejaria las medidas analogicas
                          // leyendo el canal equivocado.
    RAW_SAMPLE   = 0x0A,  // a = canal+1 a excitar (0 = ninguno)  b = nivel
                          // Ciclo de muestreo COMPLETO: las ~9 transacciones
                          // I2C que hace un SAMPLE de verdad, no una lectura
                          // suelta. Sirve para cargar el bus como lo carga un
                          // barrido y ver si es ahi donde se atasca.
};

struct __attribute__((packed)) RawOpCode {
    uint8_t  op;
    uint8_t  a;
    uint16_t b;
};

// 48 ops × 4 B = 192 B: entra en el payload de 200 B de ESP-NOW, así que la
// misma secuencia vale para una unidad local o para un hijo por radio.
#define RAW_MAX_OPS  48
#define RAW_ERR      (-2147483647L - 1)   // INT32_MIN

void rawBegin(TCA9535* u1, TCA9535* u2, Shift595* sr);

// Ejecuta las n ops EN ORDEN y sin ceder el control: una secuencia
// dir → wr → rd no se puede partir o la lectura no vale.
// results[i] = valor devuelto, o RAW_ERR si esa op falló.
// Devuelve cuántas se ejecutaron (se corta en la primera que no existe).
// ok[i] = false si esa op fallo de verdad.
//
// OJO: la validez va APARTE a proposito. Codificarla en el valor no funciona
// porque cualquier patron de 32 bits es un resultado legitimo: una mascara con
// solo el bit 31 (canal J_SP excitado) vale 0x80000000, que como int32_t es
// exactamente INT32_MIN. Usar eso como centinela daba "error de I2C" en una
// lectura perfectamente valida. Es el mismo fallo que ya hubo con 0xFFFFFFFF.
uint8_t rawExec(const RawOpCode* ops, uint8_t n, int32_t* results, bool* ok = nullptr);

// "io_dir 0 0xFFFE" → RawOpCode. err queda con el motivo si devuelve false.
bool rawParse(const String& args, RawOpCode& out, String& err);

const char* rawOpName(uint8_t op);

// Ayuda del comando 'raw' para la consola serie.
void rawPrintHelp(Print& out);
