#pragma once

// Plazo GLOBAL de cualquier operacion distribuida.
//
// Sin esto, con los hijos incomunicados cada ronda agota sus esperas por
// separado: 2 hijos x 500 ms = 1 s por ronda, x 96 canales x 2 fases x 4
// reintentos = mas de 10 minutos con el loop() parado. El equipo respondia al
// ping (la pila de red vive en otra tarea) pero no al servidor web, y parecia
// colgado. En produccion la app se quedaria esperando en vez de recibir un
// "no se pudo" en segundos.
//
// Los plazos por paquete NO bastan: hace falta un tope para la operacion
// entera.
#define MAPDIST_DEADLINE_MS  90000
#include <Arduino.h>
#include "mapping.h"
#include "tca9535.h"

// =============================================================
// mapdist.h — Mapeo sobre varias unidades enlazadas.
//
// Ver docs/MAPPING_DESIGN.md §7.
//
// Numeración global:  canal = indice_unidad × 32 + canal_local
//   indice 0 = master · 1..N = hijos en el orden de peers[]
//
// Si no hay hijos enlazados, se reduce al mapeo local de 32 canales:
// el mismo comando sirve para 1, 2 o 3 unidades.
// =============================================================

// Canales totales según los peers enlazados (32, 64 o 96).
uint8_t mapDistChannels();

// Recorre todos los canales globales y construye el mapa.
// Con hijos usa el protocolo ESP-NOW; sin ellos llama a mapRun().
bool mapDistRun(TCA9535& u1, TCA9535& u2, MapResult& out, Print& log);

// Reparte el veredicto por rangos: enciende los LEDs de esta unidad y
// manda a cada hijo los suyos.
void mapDistLeds(const MapResult& golden, const MapResult& dut);

// ── Autodiagnóstico de los módulos ───────────────────────────
//
// Con el arnés DESCONECTADO: excita cada canal y comprueba que puede subir
// Y bajar. Un pin pegado a GND no llega a alto; uno pegado a 3V3 no baja.
//
// Es digital, así que cubre las 96 líneas a través del enlace — a diferencia
// de 'leaktest', que usa el MUX+ADC y solo ve la unidad local.
//
// Fue justo esta clase de fallo (U2 P04 aterrizado por una vía huérfana) la
// que costó una revisión de PCB: conviene detectarlo antes de mapear nada.
enum : uint8_t { CHK_OK = 0, CHK_STUCK_LOW = 1, CHK_STUCK_HIGH = 2, CHK_ERR = 3 };

// status[] recibe un código por canal. Devuelve nº de canales defectuosos, o -1.
int mapDistSelfCheck(TCA9535& u1, TCA9535& u2, uint8_t* status, Print& log);
