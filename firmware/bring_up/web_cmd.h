#pragma once
#include <Arduino.h>

// Inicializar el servidor web de comandos (llamar después de conectar WiFi)
void webCmdBegin();

// Llamar en loop() cuando WiFi está conectado
void webCmdLoop();

// bring_up.ino expone esto para ejecutar comandos desde web
// out: destino de la salida (Serial o StringPrint para web)
void handleCommand(const String& cmd, Print& out);

// Estado en JSON para el software de PC (GET /api/status).
// Contrato estable y versionado — el texto de /cmd NO lo es.
void apiStatusJson(Print& out);

// GET  /api/golden — devolver el job cargado (respaldo / verificacion)
void apiGoldenGetJson(Print& out);

// POST /api/golden — la PC empuja el job. Cuerpo en lineas clave=valor.
void apiGoldenSet(const String& body, Print& out);

// POST /api/test — corrida completa: mapea, compara contra el golden y
// devuelve resultado + fallas estructuradas.
void apiRunTest(Print& out);

// GET /api/test/last — ultima prueba, incluidas las lanzadas por el switch fisico.
void apiLastTestJson(Print& out);

// GET /api/selfcheck — validar los modulos con el arnes desconectado.
void apiSelfCheckJson(Print& out);

// GET /api/map — capturar el mapa sin comparar ni guardar.
void apiMapJson(Print& out);
