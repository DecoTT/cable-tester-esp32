#pragma once
#include <Arduino.h>

// =============================================================
// link.h — Enlace multi-unidad por ESP-NOW
//
// Ver docs/LINK_DESIGN.md para el diseño completo.
//
// CÓMO FUNCIONA UN "ENLACE" EN ESP-NOW
// ESP-NOW no tiene emparejamiento como Bluetooth: no hay handshake obligatorio.
// Un peer es simplemente una MAC guardada en una tabla, más una clave opcional.
// Por eso el enlace se puede dejar hecho DESDE EL FLASHEO, escribiendo las MAC
// en NVS — sin necesidad de que las unidades se busquen por radio.
//
// El emparejamiento por radio (link_pair) sigue existiendo como alternativa
// para cuando no se tiene acceso USB a las unidades.
//
// ORDEN DE PROVISIÓN QUE EVITA CONECTAR EL MASTER DOS VECES
//   1. Flashear master → leer su MAC (link_mac)
//   2. Cada hijo: link_master <MAC_del_master>
//   3. Al arrancar, el hijo se anuncia y el master lo añade solo y lo persiste
// =============================================================

// Máximo de peers guardados. ESP-NOW admite 20 totales y ~7 cifrados;
// esto sobra para un master con varios hijos.
#define LINK_MAX_PEERS  6

enum LinkRole : uint8_t {
    LINK_ROLE_NONE   = 0,   // sin rol fijo: se deduce al arrancar
    LINK_ROLE_MASTER = 1,   // orquesta la prueba; sus peers son los hijos
    LINK_ROLE_SLAVE  = 2,   // ejecuta órdenes; su peer[0] es el master
};

// Inicializa ESP-NOW. Llamar DESPUÉS de configurar WiFi y ANTES de Wire.begin().
bool linkBegin(LinkRole role);

// Llamar en loop(). Anuncios, altas de peer, ventana de emparejamiento y
// barrido de canales.
void linkLoop();

LinkRole    linkGetRole();
const char* linkRoleName(LinkRole r);

// --- Rol configurado explícitamente (persistente en NVS) ---
//
// LINK_ROLE_NONE = "auto": se deduce en runtime (con WiFi → MASTER;
// sin WiFi pero con peer → SLAVE).
//
// Al provisionar por lote hace falta fijarlo: si varias unidades llevan las
// mismas credenciales, la deducción automática las haría master a todas.
LinkRole linkGetConfiguredRole();
bool     linkSetConfiguredRole(LinkRole r);

// --- Peers ---
String  linkOwnMac();                       // MAC de esta unidad, "AA:BB:..."
uint8_t linkPeerCount();
bool    linkGetPeer(uint8_t idx, uint8_t* mac_out);
String  linkPeerMacStr(uint8_t idx);

// Invierte el orden de los hijos (L1 <-> L2) y lo guarda.
//
// Para diagnostico: el master pregunta SIEMPRE primero al indice 0, asi que
// "falla L1" puede significar esa placa concreta O simplemente el primero de
// la cola. Invirtiendo se separa una cosa de la otra.
bool linkSwapPeers();
int8_t  linkPeerRssi(uint8_t idx);          // 0 = nunca se le oyó
bool    linkPeerOnline(uint8_t idx);

bool linkAddPeerMac(const uint8_t* mac);    // añade y persiste; ignora duplicados
bool linkAddPeerStr(const String& mac_str); // idem, aceptando "AA:BB:CC:DD:EE:FF"
void linkClearPeers();                      // deshace todos los enlaces

bool linkParseMac(const String& s, uint8_t* out);

// --- Emparejamiento por radio (alternativa al provisionado por USB) ---
void     linkStartPairing(uint32_t window_ms = 60000);
bool     linkIsPairing();
uint32_t linkPairingRemainingMs();

// --- Operación ---
bool linkPushCreds(const String& ssid, const String& pass);  // master → todos los hijos
bool linkPing();                                             // a todos los peers
void linkPrintInfo(Print& out);

// =============================================================
// Protocolo de prueba distribuida — ver docs/MAPPING_DESIGN.md §7
//
// El ciclo precarga→lectura se ejecuta DENTRO de cada unidad, de forma
// atómica. Por la radio solo viaja el arranque, y eso es insensible al
// retardo: el que maneja SOSTIENE el nivel mientras las demás muestrean.
// =============================================================

// El hijo necesita tocar sus U1/U2/LEDs, que viven en el .ino. En vez de
// meter dependencias de hardware en link.cpp, el .ino registra qué hacer.
struct LinkTestHandlers {
    uint32_t (*sample)(int drive_ch, bool level);  // ciclo atómico → máscara 32b
    void     (*drive)(int ch, bool level);         // excitar y mantener
    void     (*release)();                         // estado de reposo
    void     (*leds)(uint16_t mask);               // veredicto por rango
    // Modo dev (ver raw.h). Las ops viajan como bytes OPACOS: link.cpp no las
    // interpreta, asi que añadir primitivas nuevas no toca el enlace.
    uint8_t  (*rawexec)(const uint8_t* ops, uint8_t n_ops, int32_t* results);
};
void linkSetTestHandlers(const LinkTestHandlers& h);

// --- Primitivas del master (bloqueantes; bombean linkLoop mientras esperan) ---

// Excita un canal en el hijo idx y espera su confirmación.
bool linkRemoteDrive(uint8_t peer_idx, uint8_t local_ch, bool level, uint32_t timeout_ms = 750);

// Pide muestreo a TODOS los hijos y recoge sus máscaras.
// masks_out[i] = máscara del peer i. Devuelve false si falta alguna.
// drive_unit: que unidad excita (0 = master, 1..N = hijo). drive_ch: su canal
// LOCAL. Van dentro del SAMPLE a proposito: si el hijo que excita tuviera que
// recordarlo de un DRIVE anterior, un reintento mediria con la linea ya
// soltada y devolveria una mascara falsa sin avisar. Mandandolo en cada
// paquete, los reintentos son idempotentes.
bool linkSampleAll(bool level, uint32_t* masks_out,
                   int8_t drive_unit = -1, int8_t drive_ch = -1,
                   uint32_t timeout_ms = 1000);

// Índice del primer hijo que no contestó al último muestreo, o -1.
int8_t linkFirstSilentPeer();

// Contadores de transmision ESP-NOW. 'failed' > 0 significa que la cola de
// envio se lleno: es un problema distinto a que el otro extremo no conteste.
void linkTxStats(uint32_t& sent, uint32_t& failed);

// Diagnostico interno, consultable en un HIJO via 'rraw <n> linkstat <que>'.
// Es la unica ventana a un hijo: no esta en la red, solo habla por ESP-NOW.
//   0 canal WiFi        1 SAMPLE recibidos    2 MASK enviados
//   3 ms desde el ultimo paquete del master   4 envios fallidos
//   5 enlazado (1/0)    6 canal excitado ahora (-1 = ninguno)
//   7 veces que hubo que recolocar el canal WiFi
//   8 STA conectada a un AP (1/0)   9 veces que hubo que soltar el WiFi
int32_t linkStat(uint8_t what);

// Ejecuta ops crudas en el hijo peer_idx (0 = L1, 1 = L2).
// ops apunta a n_ops × 4 bytes; results recibe n_ops enteros de 32 bits.
// Devuelve false si el hijo no contestó tras los reintentos.
#define LINK_RAW_MAX_OPS 48
bool linkRemoteRaw(uint8_t peer_idx, const void* ops, uint8_t n_ops,
                   int32_t* results, uint32_t timeout_ms = 800);

// Libera todos los canales de todos los hijos.
void linkReleaseAll();

// Envía a un hijo su máscara de LEDs (veredicto de sus 32 canales).
bool linkSendLeds(uint8_t peer_idx, uint16_t led_mask);
