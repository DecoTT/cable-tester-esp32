#pragma once
#include <Arduino.h>
#include "tca9535.h"
#include "shift595.h"

// =============================================================
// mapping.h — Motor de mapeo por golden
//
// Ver docs/MAPPING_DESIGN.md.
//
// El firmware NO sabe cómo está cableado el arnés: lo descubre.
//   1. Arnés bueno conocido (golden) → 'map' + 'map_save'
//   2. Cada arnés siguiente → 'map_check' compara contra el golden
//
// Da igual si una conexión es U1↔U2, U1↔U1 o U2↔U2. Mapear es el primer
// paso para todos los arneses.
//
// Soporta 32 canales (una unidad) o hasta 96 (master + 2 hijos). El ciclo
// de muestreo es IDÉNTICO en ambos casos: en distribuido cada unidad ejecuta
// el mismo ciclo local de forma atómica y solo se coordina el arranque.
// =============================================================

#define MAP_CH_LOCAL   32     // canales de UNA unidad
#define MAP_CH_MAX     96     // master + 2 hijos
#define MAP_LED_RANGES 5      // 5 pares rojo/verde por PCB

// Un byte por canal:
//   net_of[i] == 0  → canal aislado
//   net_of[i] == n  → pertenece al net n
struct MapResult {
    uint8_t net_of[MAP_CH_MAX];
    uint8_t channels;      // cuántos canales son válidos (32, 64 o 96)
    uint8_t net_count;
    bool    valid;
};

// ── Ciclo de muestreo LOCAL — la pieza que comparten local y distribuido ──
//
// Atómico: precarga → alta impedancia → settle → lectura, sin radio de por
// medio. Un pin flotante RETIENE la precarga; uno conectado SIGUE al driver.
// El TCA9535 no tiene pull-ups internos (TI los quitó respecto al PCA9555),
// así que sin la precarga la lectura sería basura.
//
//   drive_ch: canal local a excitar (-1 = esta unidad solo escucha)
//   level:    nivel del driver; la precarga es el nivel OPUESTO
// Devuelve la máscara de 32 bits leída, o 0xFFFFFFFF si falla el I2C.
// ok=false si falla el I2C. OJO: 0xFFFFFFFF es una lectura VALIDA (todos los
// pines altos tras una precarga alta), asi que no sirve como centinela.
uint32_t mapSampleLocal(TCA9535& u1, TCA9535& u2, int drive_ch, bool level, bool* ok = nullptr);

// Excita un canal local y lo MANTIENE (el resto queda como esté).
// Necesario en distribuido: el que maneja debe sostener el nivel mientras
// las demás unidades muestrean, que ocurre en otro instante.
void mapDriveHold(TCA9535& u1, TCA9535& u2, int ch, bool level);

// Estado de reposo seguro: U1 salida en bajo, U2 entrada.
void mapRelease(TCA9535& u1, TCA9535& u2);

// ── Mapeo local (32 canales) ──
bool mapRun(TCA9535& u1, TCA9535& u2, MapResult& out, Print& log);

// Construye los nets a partir de una matriz de conexiones ya medida.
// La usa el mapeo distribuido, que recoge las máscaras por radio.
//   connected(i,j) se consulta via callback para no reservar 96×96 bits
void mapBuildNets(MapResult& out, uint8_t channels,
                  bool (*connected)(uint8_t i, uint8_t j, void* ctx), void* ctx);

void mapPrint(const MapResult& m, Print& out);
void mapPrintJson(const MapResult& m, const char* name, Print& out);

// ── Golden persistente en NVS ──
//
// Un tester por SKU: el golden vive en el equipo. Lleva CRC porque un
// flasheo con merged.bin borra la NVS y un golden corrupto no debe pasar
// desapercibido. La PC es la fuente de verdad y puede reponerlo.
bool     mapSaveGolden(const MapResult& m, const String& name);
bool     mapLoadGolden(MapResult& m, String& name_out);
void     mapClearGolden();
bool     mapHasGolden();
uint16_t mapGoldenCrc();

// ── Vinculación del tester a un SKU ──
// Canales POBLADOS de esta unidad (16 o 32).
//
// Un tester de 16 posiciones solo tiene conector y resistencia serie en
// J00-J15. Los J16-J31 quedan al aire, sin carga, y la lectura por precarga
// ahi es ruido puro: el mismo arnes daba una huella distinta en cada barrido.
// Barrer canales que no existen no es inofensivo, produce resultados aleatorios.
uint8_t mapGetChCount();
bool    mapSetChCount(uint8_t n);

String mapGetSku();
bool   mapSetSku(const String& sku);
String mapGetMeta();
bool   mapSetMeta(const String& meta);
void   mapClearAll();

// Compara contra el golden. Devuelve el número de fallos.
int mapCompare(const MapResult& golden, const MapResult& dut, Print& out);

// Fallos en JSON para la app de PC: [{"type":"open","a":"M.J04","b":"L1.J20"}, ...]
int mapFaultsJson(const MapResult& golden, const MapResult& dut, Print& out);

// Máscara de LEDs (bits 0-9) de UNA unidad, según qué rangos fallaron.
// unit_index: 0 = master, 1..N = hijos. Cada unidad muestra SUS 32 canales.
uint16_t mapLedMask(const MapResult& golden, const MapResult& dut, uint8_t unit_index);

// "M.J00", "L1.J19", "L2.J_SP"
String mapChName(uint8_t global_ch);

void mapRangeBounds(uint8_t r, uint8_t& lo, uint8_t& hi);
