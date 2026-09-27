#pragma once
#include <Arduino.h>

// Flujo:
//   1. wifiBegin() — intenta conectar a credenciales guardadas (12s timeout)
//   2. Si falla → levanta AP "CableTester32-XXXX" con portal captivo
//   3. Usuario selecciona red, pone contraseña → guarda en NVS → reboot
//   4. En el reboot, wifiBegin() conecta al router
//
// El sufijo XXXX son los últimos 2 bytes de la MAC: dos unidades nunca
// levantan APs homónimos.

bool   wifiBegin(uint32_t timeout_ms = 12000);
void   wifiPortalBegin();   // llama solo si wifiBegin() devuelve false
void   wifiPortalLoop();    // llamar en loop() cuando el portal está activo
bool   wifiIsPortalActive();
bool   wifiIsConnected();
String wifiGetIP();
void   wifiClearCreds();    // borrar SSID/pass guardados

// SSID del portal, con sufijo de MAC: "CableTester32-A1B2"
String wifiGetApSsid();

// Identificador corto de la unidad (últimos 2 bytes de MAC), ej. "A1B2"
String wifiUnitId();

// Anunciar rol y SKU por mDNS (TXT del servicio _cabletester._tcp).
// Llamar después de linkBegin(), cuando ya se conoce el rol.
void wifiMdnsSetInfo(const char* role, const String& sku);

// --- Provisión por lote (usado por el flasher vía serial) ---
// Escribe credenciales sin pasar por el portal. false si ssid vacío o demasiado largo.
bool   wifiSetCreds(const String& ssid, const String& pass);
String wifiGetSsid();
String wifiGetPass();   // ⚠️ solo exponer por Serial, NUNCA por la web
