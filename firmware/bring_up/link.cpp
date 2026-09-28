#include "link.h"
#include "wifi_portal.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#if __has_include(<esp_coexist.h>)
  #include <esp_coexist.h>
  #define CT_HAS_COEX 1
#endif
#include <Preferences.h>

// =============================================================
// Configuración
// =============================================================
#define PREF_NS_LINK   "link"
#define PREF_PEERS     "peers"     // blob: 6 bytes por peer
#define PREF_CHAN      "chan"      // uint8: ultimo canal bueno del master
#define PREF_ROLE      "role"      // uint8: LinkRole configurado, 0 = auto

static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Claves de cifrado ESP-NOW (AES-128).
//
// ⚠️ LIMITACIÓN CONOCIDA — ver docs/LINK_DESIGN.md §6:
// Estas claves viajan dentro del .bin, así que cualquiera con el firmware las
// extrae. Frenan el sniffing casual del password WiFi, NO a un atacante decidido.
static const uint8_t PMK[16] = {
    'C','T','3','2','p','m','k','_','v','1','_','s','b','c','g','p'
};
static const uint8_t LMK[16] = {
    'C','T','3','2','l','m','k','_','v','1','_','l','i','n','k','!'
};

// Descubrimiento: el hijo solo acierta cuando está en el mismo canal que el
// master. Con 600 ms de estancia y un envío cada 150 ms hace ~4 intentos por
// canal, en vez de 1.
static const uint32_t DISCOVER_PERIOD_MS = 150;
static const uint32_t CHANNEL_DWELL_MS   = 600;
static const uint8_t  CHANNEL_MAX        = 13;
static const uint32_t PEER_TIMEOUT_MS    = 15000;
static const uint32_t ANNOUNCE_PERIOD_MS = 3000;   // hijo ya enlazado y con red

// ESP-NOW pierde paquetes: a ~150 paquetes/s en un barrido de 96 canales
// algunos se caen. Sin reintentos, UNA perdida abortaba la prueba entera.
//
// El numero de secuencia es lo que hace seguros los reintentos: un MASK de la
// ronda anterior que llega tarde se descarta en vez de contarse como respuesta
// de la ronda actual, lo que daria datos cruzados sin avisar.
// Subido de 3 a 5 tras medir: con 3 reintentos en 600 ms las tres podian caer
// dentro de la misma ventana mala y abortar un barrido de 96 canales entero.
// Reintentar es barato (solo se reenvia a quien no contesto); abortar no.
static const uint8_t SAMPLE_TRIES = 5;

// =============================================================
// Protocolo
// =============================================================
enum LinkMsgType : uint8_t {
    LMSG_DISCOVER  = 0x01,   // hijo   → broadcast : busca master emparejando
    LMSG_OFFER     = 0x02,   // master → hijo      : acepta emparejar
    LMSG_ACCEPT    = 0x03,   // hijo   → master    : aceptado
    LMSG_CREDS     = 0x04,   // master → hijo      : SSID + clave (cifrado)
    LMSG_CREDS_ACK = 0x05,
    LMSG_ANNOUNCE  = 0x06,   // hijo   → master    : "existo, esta es mi MAC"
    LMSG_ANN_ACK   = 0x07,   // master → hijo      : "te tengo en la lista"
    LMSG_PING      = 0x10,
    LMSG_PONG      = 0x11,
    // Prueba distribuida — docs/MAPPING_DESIGN.md §7
    LMSG_DRIVE     = 0x20,   // master → dueña : excita canal local y MANTIENE
    LMSG_DRIVE_ACK = 0x21,
    LMSG_SAMPLE    = 0x22,   // master → todas : ciclo atómico local
    LMSG_MASK      = 0x23,   // hijo   → master: máscara de 32 bits leída
    LMSG_RELEASE   = 0x24,   // master → todas : estado de reposo
    LMSG_LEDS      = 0x25,   // master → hijo  : veredicto por rango
    LMSG_RAW       = 0x26,   // master → hijo  : ops crudas (modo dev)
    LMSG_RAW_RES   = 0x27,   // hijo   → master: resultados
};

#define LMSG_MAGIC0  'C'
#define LMSG_MAGIC1  'T'

struct __attribute__((packed)) LinkMsg {
    uint8_t magic0, magic1;
    uint8_t type;
    uint8_t len;
    uint8_t payload[200];
};

// =============================================================
// Estado
// =============================================================
static LinkRole _role   = LINK_ROLE_NONE;
static bool     _inited = false;

static uint8_t  _peers[LINK_MAX_PEERS][6];
static uint8_t  _peer_count = 0;
static uint32_t _seen[LINK_MAX_PEERS]  = {0};
static int8_t   _rssi[LINK_MAX_PEERS]  = {0};

static bool     _pairing    = false;
static uint32_t _pair_until = 0;

static uint8_t  _scan_ch   = 1;
static uint32_t _scan_next = 0;
static uint32_t _disc_next = 0;
static uint32_t _ann_next  = 0;

// --- Trabajo diferido del callback ---
// El callback de ESP-NOW corre en la tarea WiFi: ahí NO se toca NVS, ni
// WiFi.begin(), ni se llama esp_now_send()/add_peer(). Solo se marca bandera.
static volatile bool _todo_offer    = false;
static volatile bool _todo_accept   = false;
static volatile bool _todo_pong     = false;
static volatile bool _todo_ann      = false;   // master: llegó un anuncio
static volatile bool _todo_addpeer  = false;   // hay una MAC nueva que persistir
static volatile bool _creds_pending = false;

static uint8_t _src_offer[6], _src_accept[6], _src_pong[6], _src_ann[6], _src_new[6];
static String  _creds_ssid, _creds_pass;

// --- Prueba distribuida ---
static LinkTestHandlers _th = {nullptr, nullptr, nullptr, nullptr, nullptr};

// --- Modo dev ---
static volatile bool    _todo_raw   = false;
static volatile uint8_t _raw_n      = 0;
static volatile uint8_t _raw_seq_rx = 0;
static uint8_t  _raw_ops[LINK_RAW_MAX_OPS * 4];
static volatile uint8_t _raw_seq    = 0;     // ronda actual (master)
static volatile bool    _raw_got    = false;
static volatile uint8_t _raw_res_n  = 0;
static int32_t  _raw_res[LINK_RAW_MAX_OPS];

// Lado hijo: trabajo diferido desde el callback
static volatile bool    _todo_drive   = false;
static volatile bool    _todo_sample  = false;
static volatile bool    _todo_release = false;
static volatile bool    _todo_leds    = false;
static volatile int8_t  _drive_ch     = -1;    // canal local que esta excitando
static volatile bool    _drive_level  = false;
static volatile bool    _sample_level = false;
static volatile uint16_t _leds_mask   = 0;
static uint8_t _src_test[6];

// Lado master: recoleccion de respuestas
static volatile bool     _drive_acked  = false;
static volatile uint8_t  _sample_seq   = 0;   // ronda actual (master)
static volatile uint8_t  _rx_seq       = 0;   // ronda del ultimo SAMPLE (hijo)
static volatile uint32_t _rx_mask[LINK_MAX_PEERS];
static volatile bool     _rx_got[LINK_MAX_PEERS];

// =============================================================
// Helpers
// =============================================================
static String macToStr(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(b);
}

bool linkParseMac(const String& s, uint8_t* out) {
    // Acepta "AA:BB:CC:DD:EE:FF", con '-' o sin separador
    String t;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isxdigit((int)c)) t += c;
        else if (c != ':' && c != '-' && c != ' ') return false;
    }
    if (t.length() != 12) return false;
    for (int i = 0; i < 6; i++)
        out[i] = (uint8_t)strtoul(t.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    return true;
}

static int _findPeer(const uint8_t* mac) {
    for (uint8_t i = 0; i < _peer_count; i++)
        if (memcmp(_peers[i], mac, 6) == 0) return i;
    return -1;
}

static void _savePeers() {
    Preferences p;
    p.begin(PREF_NS_LINK, false);
    if (_peer_count) p.putBytes(PREF_PEERS, _peers, _peer_count * 6);
    else             p.remove(PREF_PEERS);
    p.end();
}

static void _loadPeers() {
    Preferences p;
    p.begin(PREF_NS_LINK, true);
    size_t n = p.getBytesLength(PREF_PEERS);
    _peer_count = 0;
    if (n >= 6 && n % 6 == 0) {
        uint8_t cnt = n / 6;
        if (cnt > LINK_MAX_PEERS) cnt = LINK_MAX_PEERS;
        p.getBytes(PREF_PEERS, _peers, cnt * 6);
        _peer_count = cnt;
    }
    p.end();
}

static bool _registerPeer(const uint8_t* mac, bool encrypted) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, mac, 6);
    pi.channel = 0;                 // 0 = canal actual
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = encrypted;
    if (encrypted) memcpy(pi.lmk, LMK, 16);
    esp_err_t e = esp_now_add_peer(&pi);
    if (e != ESP_OK)
        Serial.printf("[LINK] add_peer %s fallo: %d\n", macToStr(mac).c_str(), (int)e);
    return e == ESP_OK;
}

// Control de flujo de transmision.
//
// ESP-NOW tiene una cola de envio pequeña (~7 entradas). Si se encola mas
// rapido de lo que la radio transmite, esp_now_send() devuelve
// ESP_ERR_ESPNOW_NO_MEM y el paquete SE PIERDE. Antes no se miraba ese valor
// en ninguna llamada, asi que el sintoma era "el hijo no contesto" — idéntico
// a una perdida por aire, pero con causa y arreglo distintos. Y empeoraba al
// subir los reintentos, porque cada reintento mete mas presion en la cola.
static volatile bool     _tx_busy = false;
static volatile uint32_t _tx_fail = 0;
static volatile uint32_t _tx_sent = 0;
static volatile uint32_t _n_sample_rx = 0;
static volatile uint32_t _n_mask_tx = 0;
static volatile uint32_t _chan_fixes = 0;
static volatile uint32_t _mask_ok = 0;    // MASK aceptados
static volatile uint32_t _mask_rej = 0;   // MASK tirados por secuencia
static volatile uint8_t  _last_rej_seq = 0;
static volatile uint8_t  _last_exp_seq = 0;
static uint8_t           _saved_chan = 0;
static volatile uint8_t  _want_chan  = 0;   // canal que pide el master (0 = nada)
static volatile uint32_t _sta_drops  = 0;   // veces que hubo que soltar el WiFi
static volatile bool     _was_linked = false;
static volatile uint32_t _lost_count = 0;   // veces que perdio al master
static volatile uint32_t _lost_at    = 0;
static volatile uint32_t _ms_scanning = 0;  // tiempo total barriendo canales
static volatile uint32_t _last_loop  = 0;
static uint32_t          _ps_next    = 0;

// El estado del callback NO es lo mismo que el retorno de esp_now_send():
//   esp_now_send() != ESP_OK -> no llego ni a salir (cola llena)
//   status == SEND_FAIL      -> salio, pero el otro extremo NO lo confirmo a
//                               nivel MAC. Es la huella de un receptor dormido.
// Distinguirlos importa: son dos fallos con el mismo sintoma visible.
static volatile uint32_t _tx_noack = 0;

static inline void _sentDone(esp_now_send_status_t st) {
    if (st != ESP_NOW_SEND_SUCCESS) _tx_noack++;
    _tx_busy = false;
}

#if ESP_IDF_VERSION_MAJOR >= 5 && ESP_IDF_VERSION_MINOR >= 3
static void _onSent(const wifi_tx_info_t*, esp_now_send_status_t st) { _sentDone(st); }
#else
static void _onSent(const uint8_t*, esp_now_send_status_t st) { _sentDone(st); }
#endif

static bool _send(const uint8_t* mac, LinkMsgType type,
                  const void* payload = nullptr, uint8_t len = 0) {
    // Esperar a que salga el anterior. Acotado: si el callback no llega se
    // sigue igual, que perder un paquete es mejor que bloquear el barrido.
    uint32_t t0 = millis();
    while (_tx_busy && (millis() - t0) < 20) delayMicroseconds(200);

    LinkMsg m;
    m.magic0 = LMSG_MAGIC0;
    m.magic1 = LMSG_MAGIC1;
    m.type   = type;
    m.len    = len;
    if (len && payload) memcpy(m.payload, payload, len);

    _tx_busy = true;
    esp_err_t e = esp_now_send(mac, (const uint8_t*)&m, 4 + len);
    if (e != ESP_OK) { _tx_busy = false; _tx_fail++; return false; }
    _tx_sent++;
    return true;
}

void linkTxStats(uint32_t& sent, uint32_t& failed) { sent = _tx_sent; failed = _tx_fail; }

int32_t linkStat(uint8_t what) {
    uint32_t now = millis();
    switch (what) {
        case 0: return (int32_t)WiFi.channel();
        case 1: return (int32_t)_n_sample_rx;
        case 2: return (int32_t)_n_mask_tx;
        case 3: return _peer_count ? (int32_t)(now - _seen[0]) : -1;
        case 4: return (int32_t)_tx_fail;
        case 5: return (_peer_count && _seen[0] &&
                        (now - _seen[0]) < PEER_TIMEOUT_MS) ? 1 : 0;
        case 6: return (int32_t)_drive_ch;
        case 7: return (int32_t)_chan_fixes;   // veces que hubo que recolocar el canal
        case 8: return (WiFi.status() == WL_CONNECTED) ? 1 : 0;
        case 9:  return (int32_t)_sta_drops;
        case 14: return (int32_t)_tx_noack;
        case 10: return (int32_t)_lost_count;                 // veces perdido
        case 11: return _lost_at ? (int32_t)(now - _lost_at) : -1;  // hace cuanto
        case 12: return (int32_t)_ms_scanning;                // ms barriendo
        case 13: return (int32_t)_n_mask_tx;
        default: return -1;
    }
}

// =============================================================
// Callback de recepción — SOLO marca banderas y copia datos.
// =============================================================
static void _onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < 4) return;
    const LinkMsg* m = (const LinkMsg*)data;
    if (m->magic0 != LMSG_MAGIC0 || m->magic1 != LMSG_MAGIC1) return;
    if (4 + m->len > len) return;

    const uint8_t* src = info->src_addr;
    int idx = _findPeer(src);
    if (idx >= 0) {
        _seen[idx] = millis();
        if (info->rx_ctrl) _rssi[idx] = info->rx_ctrl->rssi;
    }

    switch (m->type) {

    case LMSG_DISCOVER:
        if (_role == LINK_ROLE_MASTER && _pairing) {
            memcpy(_src_offer, src, 6);
            _todo_offer = true;
        }
        break;

    case LMSG_OFFER:
        if (_role == LINK_ROLE_SLAVE && _peer_count == 0) {
            memcpy(_src_accept, src, 6);
            _todo_accept = true;
        }
        break;

    case LMSG_ACCEPT:
        if (_role == LINK_ROLE_MASTER && _pairing) {
            memcpy(_src_new, src, 6);
            _todo_addpeer = true;
            _pairing = false;
        }
        break;

    // Hijo anunciándose: el master lo aprende solo. Esto es lo que evita
    // tener que conectar el master una segunda vez al provisionar por lote.
    case LMSG_ANNOUNCE:
        if (_role == LINK_ROLE_MASTER) {
            memcpy(_src_ann, src, 6);
            _todo_ann = true;
            if (idx < 0) {                     // desconocido → darlo de alta
                memcpy(_src_new, src, 6);
                _todo_addpeer = true;
            }
        }
        break;

    case LMSG_ANN_ACK:
        // Re-sincronizar el canal.
        //
        // MEDIDO EN VIVO: master en 6, L1 en 10, L2 en 5. El hijo deja de
        // barrer canales en cuanto se cree enlazado, y como nada reimponia el
        // canal, una vez desviado se quedaba asi para siempre. Seguia oyendo a
        // medias — los canales vecinos se solapan — asi que parecia perdida de
        // paquetes aleatoria: 74 de 192 SAMPLE recibidos en un barrido.
        //
        // No basta con mirar 'enlazado': se puede estar enlazado Y en el canal
        // equivocado, que es justo el caso que costo encontrar.
        if (_role == LINK_ROLE_SLAVE && m->len >= 1) {
            uint8_t want = m->payload[0];
            // Solo se marca: esp_wifi_set_channel() NO se llama desde aqui,
            // que este callback corre en la tarea WiFi. Lo hace linkLoop().
            if (want >= 1 && want <= CHANNEL_MAX) _want_chan = want;
        }
        break;

    case LMSG_CREDS: {
        if (_role != LINK_ROLE_SLAVE) break;
        // payload: [len_ssid][ssid...][len_pass][pass...]
        if (m->len < 2) break;
        uint8_t ls = m->payload[0];
        if (ls > 32 || (int)(1 + ls + 1) > m->len) break;
        uint8_t lp = m->payload[1 + ls];
        if (lp > 64 || (int)(1 + ls + 1 + lp) > m->len) break;
        char ssid[33] = {0}, pass[65] = {0};
        memcpy(ssid, &m->payload[1],      ls);
        memcpy(pass, &m->payload[2 + ls], lp);
        _creds_ssid = String(ssid);
        _creds_pass = String(pass);
        _creds_pending = true;
        break;
    }

    case LMSG_PING:
        memcpy(_src_pong, src, 6);
        _todo_pong = true;
        break;

    // ── Prueba distribuida: lado HIJO ──
    // Solo se marca; el ciclo real corre en linkLoop, donde si se puede
    // tocar I2C y tardar milisegundos.
    case LMSG_DRIVE:
        if (m->len >= 2) {
            _drive_ch    = (int8_t)m->payload[0];
            _drive_level = m->payload[1] != 0;
            memcpy(_src_test, src, 6);
            _todo_drive = true;
        }
        break;

    case LMSG_SAMPLE:
        if (m->len >= 1) {
            _n_sample_rx++;
            _sample_level = m->payload[0] != 0;
            // El canal a excitar viaja EN el paquete. Antes se usaba el
            // _drive_ch guardado por un DRIVE previo, y un reintento medía con
            // la línea ya soltada: máscara falsa y en silencio.
            if (m->len >= 3) _drive_ch = (int8_t)m->payload[2] - 1;
            // Recolocar el canal con el trafico de la PRUEBA, no solo con los
            // avisos periodicos.
            //
            // MEDIDO: un hijo que pierde al master se pone a barrer, y al pasar
            // por un canal ADYACENTE (7 estando el master en 6) oye lo justo
            // por solapamiento para creerse enlazado. Entonces deja de barrer y
            // se queda ahi, recibiendo ~17% de los paquetes. El ANN_ACK no lo
            // rescataba porque llegaba igual de mal que todo lo demas.
            //
            // Con el canal en cada SAMPLE basta que llegue UNO para corregirlo.
            if (_role == LINK_ROLE_SLAVE && m->len >= 4) {
                uint8_t want = m->payload[3];
                if (want >= 1 && want <= CHANNEL_MAX &&
                    want != (uint8_t)WiFi.channel()) _want_chan = want;
            }
            _rx_seq       = (m->len >= 2) ? m->payload[1] : 0;
            memcpy(_src_test, src, 6);
            _todo_sample = true;
        }
        break;

    case LMSG_RELEASE:
        _drive_ch = -1;
        _todo_release = true;
        break;

    case LMSG_RAW:
        if (m->len >= 2) {
            uint8_t n = m->payload[1];
            if (n > LINK_RAW_MAX_OPS) n = LINK_RAW_MAX_OPS;
            if (m->len >= (uint8_t)(2 + n * 4)) {
                _raw_seq_rx = m->payload[0];
                _raw_n      = n;
                memcpy(_raw_ops, &m->payload[2], n * 4);
                memcpy(_src_test, src, 6);
                _todo_raw = true;
            }
        }
        break;

    case LMSG_LEDS:
        if (m->len >= 2) {
            _leds_mask = (uint16_t)m->payload[0] | ((uint16_t)m->payload[1] << 8);
            _todo_leds = true;
        }
        break;

    // ── Prueba distribuida: lado MASTER ──
    case LMSG_DRIVE_ACK:
        _drive_acked = true;
        break;

    case LMSG_RAW_RES:
        // Mismo filtro por secuencia que MASK: una respuesta tardia de la
        // ronda anterior daria resultados de OTRA secuencia de ops.
        if (m->len >= 2 && m->payload[0] == _raw_seq) {
            uint8_t n = m->payload[1];
            if (n > LINK_RAW_MAX_OPS) n = LINK_RAW_MAX_OPS;
            if (m->len >= (uint8_t)(2 + n * 4)) {
                memcpy(_raw_res, &m->payload[2], n * 4);
                _raw_res_n = n;
                _raw_got   = true;
            }
        }
        break;

    case LMSG_MASK:
        // Con reintentos puede llegar la respuesta de una ronda anterior.
        // Aceptarla contaminaria esta ronda con datos de otro canal.
        if (idx >= 0 && m->len >= 5 && m->payload[0] != _sample_seq) {
            // Llego una respuesta, pero de otra ronda. Si esto crece durante un
            // fallo, el hijo SI contesta y es el master quien la descarta.
            _mask_rej++;
            _last_rej_seq = m->payload[0];
            _last_exp_seq = _sample_seq;
        }
        if (idx >= 0 && m->len >= 5 && m->payload[0] == _sample_seq) {
            _mask_ok++;
            _rx_mask[idx] = (uint32_t)m->payload[1]        | ((uint32_t)m->payload[2] << 8)
                          | ((uint32_t)m->payload[3] << 16) | ((uint32_t)m->payload[4] << 24);
            _rx_got[idx] = true;
        }
        break;

    default:
        break;
    }
}

// =============================================================
// API — peers
// =============================================================
String linkOwnMac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return macToStr(mac);
}

bool linkSwapPeers() {
    if (_peer_count < 2) return false;
    uint8_t tmp[6];
    memcpy(tmp,        _peers[0], 6);
    memcpy(_peers[0],  _peers[1], 6);
    memcpy(_peers[1],  tmp,       6);
    uint32_t ts = _seen[0]; _seen[0] = _seen[1]; _seen[1] = ts;
    int8_t   rs = _rssi[0]; _rssi[0] = _rssi[1]; _rssi[1] = rs;
    _savePeers();
    return true;
}

uint8_t linkPeerCount() { return _peer_count; }

bool linkGetPeer(uint8_t idx, uint8_t* mac_out) {
    if (idx >= _peer_count) return false;
    memcpy(mac_out, _peers[idx], 6);
    return true;
}

String linkPeerMacStr(uint8_t idx) {
    if (idx >= _peer_count) return String("--");
    return macToStr(_peers[idx]);
}

int8_t linkPeerRssi(uint8_t idx) { return idx < _peer_count ? _rssi[idx] : 0; }

bool linkPeerOnline(uint8_t idx) {
    if (idx >= _peer_count || !_seen[idx]) return false;
    return (millis() - _seen[idx]) < PEER_TIMEOUT_MS;
}

bool linkAddPeerMac(const uint8_t* mac) {
    static const uint8_t ZERO[6] = {0};
    if (!memcmp(mac, ZERO, 6) || !memcmp(mac, BROADCAST, 6)) return false;

    uint8_t own[6];
    esp_read_mac(own, ESP_MAC_WIFI_STA);
    if (!memcmp(mac, own, 6)) return false;      // no enlazarse consigo mismo

    if (_findPeer(mac) >= 0) return true;        // ya estaba: no es error
    if (_peer_count >= LINK_MAX_PEERS) return false;

    memcpy(_peers[_peer_count], mac, 6);
    _seen[_peer_count] = 0;
    _rssi[_peer_count] = 0;
    _peer_count++;
    _savePeers();
    if (_inited) _registerPeer(mac, true);
    return true;
}

bool linkAddPeerStr(const String& mac_str) {
    uint8_t mac[6];
    if (!linkParseMac(mac_str, mac)) return false;
    return linkAddPeerMac(mac);
}

void linkClearPeers() {
    for (uint8_t i = 0; i < _peer_count; i++)
        if (esp_now_is_peer_exist(_peers[i])) esp_now_del_peer(_peers[i]);
    _peer_count = 0;
    memset(_peers, 0, sizeof(_peers));
    memset(_seen,  0, sizeof(_seen));
    memset(_rssi,  0, sizeof(_rssi));
    Preferences p;
    p.begin(PREF_NS_LINK, false);
    // remove(), NO clear(): clear() borraria tambien el rol configurado.
    p.remove(PREF_PEERS);
    p.end();
    Serial.println(F("[LINK] Enlaces borrados."));
}

// =============================================================
// API — rol
// =============================================================
LinkRole linkGetRole() { return _role; }

const char* linkRoleName(LinkRole r) {
    switch (r) {
        case LINK_ROLE_MASTER: return "MASTER";
        case LINK_ROLE_SLAVE:  return "LINK";
        default:               return "AUTO";
    }
}

LinkRole linkGetConfiguredRole() {
    Preferences p;
    p.begin(PREF_NS_LINK, true);
    uint8_t r = p.getUChar(PREF_ROLE, (uint8_t)LINK_ROLE_NONE);
    p.end();
    if (r != LINK_ROLE_MASTER && r != LINK_ROLE_SLAVE) return LINK_ROLE_NONE;
    return (LinkRole)r;
}

bool linkSetConfiguredRole(LinkRole r) {
    if (r != LINK_ROLE_NONE && r != LINK_ROLE_MASTER && r != LINK_ROLE_SLAVE) return false;
    Preferences p;
    p.begin(PREF_NS_LINK, false);
    bool ok = p.putUChar(PREF_ROLE, (uint8_t)r) > 0;
    p.end();
    return ok;
}

// =============================================================
// API — ciclo de vida
// =============================================================
bool linkBegin(LinkRole role) {
    _role = role;
    _loadPeers();                                  // antes de esp_now_init

    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[LINK] esp_now_init() FALLO"));
        return false;
    }

    // CRÍTICO: el ahorro de energía WiFi (WIFI_PS_MIN_MODEM, activo por defecto
    // en modo STA) apaga la radio a ratos y hace PERDER paquetes ESP-NOW. Es la
    // causa clásica de "el enlace funciona a veces".
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Inclinar el arbitro de coexistencia hacia el WiFi.
    //
    // El ESP32-C3 tiene UNA sola radio compartida entre WiFi y BLE, y el core
    // de Arduino trae la coexistencia compilada aunque no se use BLE. Cuando
    // la arbitra el software, puede dejar al ESP-NOW sin turnos: los paquetes
    // se encolan bien, el receptor no recibe nada, y llegan de golpe cuando el
    // emisor suelta la radio. Reportado en el foro de Espressif con este mismo
    // chip (esp32.com/viewtopic.php?t=38709).
    //
    // Aqui no se usa BLE para nada, asi que darle toda la preferencia al WiFi
    // no quita nada.
#ifdef CT_HAS_COEX
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
    // esp_wifi_set_ps() es a nivel IDF y NO sobrevive a una reconexion: si el
    // WiFi se cae y vuelve, el core reactiva el ahorro y el enlace empieza a
    // perder paquetes otra vez. WiFi.setSleep(false) deja la marca a nivel
    // Arduino y se reaplica sola en cada reconexion.
    WiFi.setSleep(false);

    // Fijar la potencia de transmision al maximo.
    //
    // El core la deja a su criterio y puede bajarla tras una reconexion. Medido
    // en vivo: el RSSI de los hijos cayo de -16/-30 dBm a -50/-68 dBm con las
    // unidades QUIETAS, y los barridos empezaron a abortar justo entonces.
    // Fijarla no descarta una causa fisica (algo que estorbe a la antena), pero
    // si elimina esta variable del diagnostico.
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    esp_now_set_pmk(PMK);
    esp_now_register_recv_cb(_onRecv);
    esp_now_register_send_cb(_onSent);

    // Arrancar en el ultimo canal bueno conocido. Sin esto, un hijo reiniciado
    // empieza en el 1 y tiene que barrer hasta dar con el master.
    {
        Preferences pr;
        pr.begin(PREF_NS_LINK, true);
        _saved_chan = pr.getUChar(PREF_CHAN, 0);
        pr.end();
        if (role == LINK_ROLE_SLAVE && _saved_chan >= 1 && _saved_chan <= CHANNEL_MAX) {
            esp_wifi_set_channel(_saved_chan, WIFI_SECOND_CHAN_NONE);
            _scan_ch = _saved_chan;
            Serial.printf("[LINK] Arrancando en el canal guardado: %u\n", _saved_chan);
        }
    }
    _registerPeer(BROADCAST, false);               // descubrimiento, sin secretos

    for (uint8_t i = 0; i < _peer_count; i++)
        _registerPeer(_peers[i], true);

    _inited = true;

    Serial.printf("[LINK] ESP-NOW listo. Rol=%s  MAC=%s  canal=%d\n",
                  linkRoleName(_role), linkOwnMac().c_str(), WiFi.channel());
    if (_peer_count) {
        Serial.printf("[LINK] %u peer(s) guardado(s):\n", _peer_count);
        for (uint8_t i = 0; i < _peer_count; i++)
            Serial.printf("[LINK]   #%u %s\n", i, macToStr(_peers[i]).c_str());
    } else {
        Serial.println(F("[LINK] Sin peers. Provisiona con link_master/link_child, "
                         "o empareja con link_pair."));
    }
    return true;
}

void linkLoop() {
    if (!_inited) return;
    uint32_t now = millis();
    if (!_last_loop) _last_loop = now;

    // Reimponer el ahorro de energia APAGADO, cada 5 s.
    //
    // No basta con desactivarlo una vez al arrancar: la STA sigue viva y cada
    // cambio de estado del WiFi puede reactivarlo. Un receptor en ahorro solo
    // despierta en las balizas del AP, asi que pierde la mayoria de los
    // paquetes ESP-NOW — coincide con el ~17% que medimos en un hijo mudo, y
    // con lo reportado por otros (r/esp32: es el RECEPTOR en STA el que se
    // duerme siguiendo el intervalo de balizas).
    if ((int32_t)(now - _ps_next) >= 0) {
        _ps_next = now + 5000;
        esp_wifi_set_ps(WIFI_PS_NONE);
        WiFi.setSleep(false);
    }

    // --- Master: contestar un descubrimiento ---
    if (_todo_offer) {
        _todo_offer = false;
        _registerPeer(_src_offer, false);
        _send(_src_offer, LMSG_OFFER);
        Serial.printf("[LINK] Descubrimiento de %s → OFFER\n", macToStr(_src_offer).c_str());
    }

    // --- Hijo: aceptar el master que respondió ---
    if (_todo_accept) {
        _todo_accept = false;
        _registerPeer(_src_accept, false);
        _send(_src_accept, LMSG_ACCEPT);
        linkAddPeerMac(_src_accept);
        Serial.printf("[LINK] Master encontrado: %s\n", macToStr(_src_accept).c_str());
    }

    // --- Alta de peer nueva (por ACCEPT o por ANNOUNCE) ---
    if (_todo_addpeer) {
        _todo_addpeer = false;
        if (linkAddPeerMac(_src_new))
            Serial.printf("[LINK] Peer dado de alta y guardado: %s\n",
                          macToStr(_src_new).c_str());
        else
            Serial.printf("[LINK] No se pudo añadir %s (¿lista llena?)\n",
                          macToStr(_src_new).c_str());
    }

    // --- Master: confirmar anuncio ---
    if (_todo_ann) {
        _todo_ann = false;
        if (!esp_now_is_peer_exist(_src_ann)) _registerPeer(_src_ann, true);
        // El ACK lleva el canal del master. Ver LMSG_ANN_ACK en el receptor:
        // es lo unico que vuelve a sincronizar a un hijo que se desvio.
        uint8_t ch = (uint8_t)WiFi.channel();
        _send(_src_ann, LMSG_ANN_ACK, &ch, 1);
    }

    if (_todo_pong) {
        _todo_pong = false;
        if (!esp_now_is_peer_exist(_src_pong)) _registerPeer(_src_pong, false);
        _send(_src_pong, LMSG_PONG);
    }

    // ── Prueba distribuida: ejecucion del lado HIJO ──
    // Aqui si se puede tocar I2C. El ciclo de muestreo es atomico dentro de
    // esta unidad, asi que el retardo de la radio hasta este punto da igual:
    // el que maneja sostiene el nivel todo el rato.
    if (_todo_drive) {
        _todo_drive = false;
        if (_th.drive) _th.drive(_drive_ch, _drive_level);
        _send(_src_test, LMSG_DRIVE_ACK);
    }

    if (_todo_sample) {
        _todo_sample = false;
        uint32_t mask = _th.sample ? _th.sample(_drive_ch, _sample_level) : 0xFFFFFFFFu;
        // CRITICO: soltar el canal tras muestrear. Si se mantiene, esta unidad
        // lo sigue excitando en los muestreos de OTROS canales fuente, el pin
        // "sigue" su propio nivel y aparece conectado a todo — un net gigante
        // de falsos positivos. El master reenvia DRIVE antes de cada barrido.
        _drive_ch = -1;
        uint8_t p[5] = { (uint8_t)_rx_seq,
                         (uint8_t)(mask),       (uint8_t)(mask >> 8),
                         (uint8_t)(mask >> 16), (uint8_t)(mask >> 24) };
        if (_send(_src_test, LMSG_MASK, p, 5)) _n_mask_tx++;
    }

    // Recolocar el canal si el master dice que estamos en otro.
    //
    // OJO — limitacion del hardware, documentada por Espressif: el ESP32 tiene
    // UNA sola radio, y esp_wifi_set_channel() NO HACE NADA mientras la STA
    // esta conectada a un AP: el canal lo impone el router. Por eso el hijo se
    // desconecta de WiFi antes de recolocarse.
    //
    // Un hijo no necesita red: solo habla ESP-NOW. El unico que sirve HTTP es
    // el master. Asi el canal vuelve a ser nuestro y deja de haber tres
    // unidades en tres canales distintos (medido: master 6, L1 10, L2 5).
    if (_want_chan) {
        uint8_t want = _want_chan; _want_chan = 0;
        // Recordarlo: al arrancar se empieza ahi en vez de barrer a ciegas.
        if (want != _saved_chan) {
            _saved_chan = want;
            Preferences pr;
            pr.begin(PREF_NS_LINK, false);
            pr.putUChar(PREF_CHAN, want);
            pr.end();
        }
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, false);
            _sta_drops++;
            delay(20);
        }
        if (want != (uint8_t)WiFi.channel()) {
            esp_wifi_set_channel(want, WIFI_SECOND_CHAN_NONE);
            _chan_fixes++;
        }
    }

    if (_todo_release) {
        _todo_release = false;
        if (_th.release) _th.release();
    }

    if (_todo_leds) {
        _todo_leds = false;
        if (_th.leds) _th.leds(_leds_mask);
    }

    // --- Hijo: modo dev. La secuencia se ejecuta ENTERA aqui, sin ceder el
    // control, porque un dir -> wr -> rd partido a la mitad no mide nada.
    if (_todo_raw) {
        _todo_raw = false;
        uint8_t n = _raw_n;
        int32_t res[LINK_RAW_MAX_OPS];
        for (uint8_t i = 0; i < n; i++) res[i] = 0;
        uint8_t done = _th.rawexec ? _th.rawexec(_raw_ops, n, res) : 0;

        uint8_t p[2 + LINK_RAW_MAX_OPS * 4];
        p[0] = _raw_seq_rx;
        p[1] = done;
        memcpy(&p[2], res, done * 4);
        _send(_src_test, LMSG_RAW_RES, p, (uint8_t)(2 + done * 4));
    }

    // --- Hijo: aplicar credenciales recibidas ---
    if (_creds_pending) {
        _creds_pending = false;
        Serial.printf("[LINK] Credenciales del master: '%s'. Guardando...\n",
                      _creds_ssid.c_str());
        wifiSetCreds(_creds_ssid, _creds_pass);
        Serial.println(F("[LINK] Reiniciando para conectar a la red."));
        delay(600);
        ESP.restart();
    }

    // --- Master: vencimiento de la ventana de emparejamiento ---
    if (_pairing && (int32_t)(now - _pair_until) >= 0) {
        _pairing = false;
        Serial.println(F("[LINK] Ventana de emparejamiento cerrada (timeout)."));
    }

    // --- Hijo con master guardado ---
    //
    // OJO: un hijo sin WiFi arranca en el canal 1, pero el master está en el
    // canal de SU router (6, 11, el que sea). ESP-NOW exige el mismo canal, así
    // que anunciarse sin más no llega a ninguna parte.
    //
    // Mientras el master no confirme (ANN_ACK), hay que BARRER CANALES igual que
    // en el emparejamiento. Cuando confirma, se deja de saltar y queda fijado.
    // Si el router cambia de canal, _seen[0] caduca y el barrido se reanuda solo.
    if (_role == LINK_ROLE_SLAVE && _peer_count > 0) {
        bool linked = _seen[0] && (now - _seen[0]) < PEER_TIMEOUT_MS;

        // Registrar la TRANSICION enlazado -> perdido. Sin esto no hay forma
        // de saber si un hijo mudo se creia conectado o estaba buscando.
        if (_was_linked && !linked) { _lost_count++; _lost_at = now; }
        _was_linked = linked;

        if (!linked) {
            _ms_scanning += (now - _last_loop);
            if ((int32_t)(now - _scan_next) >= 0) {
                _scan_ch = (_scan_ch % CHANNEL_MAX) + 1;
                esp_wifi_set_channel(_scan_ch, WIFI_SECOND_CHAN_NONE);
                _scan_next = now + CHANNEL_DWELL_MS;
                if (_scan_ch == 1) {
                    static uint16_t sweep = 0;
                    Serial.printf("[LINK] Buscando al master por canal (barrido %u)...\n", ++sweep);
                }
            }
            if ((int32_t)(now - _ann_next) >= 0) {
                _send(_peers[0], LMSG_ANNOUNCE);
                _ann_next = now + DISCOVER_PERIOD_MS;   // rápido mientras busca
            }
        } else {
            if ((int32_t)(now - _ann_next) >= 0) {
                _send(_peers[0], LMSG_ANNOUNCE);
                _ann_next = now + ANNOUNCE_PERIOD_MS;   // ya enlazado: mantener vivo
            }
        }
    }

    // --- Hijo sin peers: barrer canales buscando master emparejando ---
    if (_role == LINK_ROLE_SLAVE && _peer_count == 0) {
        if ((int32_t)(now - _scan_next) >= 0) {
            _scan_ch = (_scan_ch % CHANNEL_MAX) + 1;
            esp_wifi_set_channel(_scan_ch, WIFI_SECOND_CHAN_NONE);
            _scan_next = now + CHANNEL_DWELL_MS;
            if (_scan_ch == 1) {
                static uint16_t sweep = 0;
                Serial.printf("[LINK] Barrido %u de canales 1-%d, sin master aun...\n",
                              ++sweep, CHANNEL_MAX);
            }
        }
        if ((int32_t)(now - _disc_next) >= 0) {
            _send(BROADCAST, LMSG_DISCOVER);
            _disc_next = now + DISCOVER_PERIOD_MS;
        }
    }

    _last_loop = now;
}

// =============================================================
// API — emparejamiento por radio
// =============================================================
void linkStartPairing(uint32_t window_ms) {
    if (!_inited) return;
    _pairing    = true;
    _pair_until = millis() + window_ms;
    Serial.printf("[LINK] Emparejamiento ABIERTO %lu s.\n",
                  (unsigned long)(window_ms / 1000));
}

bool     linkIsPairing() { return _pairing; }
uint32_t linkPairingRemainingMs() {
    if (!_pairing) return 0;
    int32_t r = (int32_t)(_pair_until - millis());
    return r > 0 ? (uint32_t)r : 0;
}

// =============================================================
// API — operación
// =============================================================
bool linkPushCreds(const String& ssid, const String& pass) {
    if (!_inited || !_peer_count) return false;
    if (ssid.length() > 32 || pass.length() > 64) return false;

    uint8_t buf[200];
    uint8_t i = 0;
    buf[i++] = (uint8_t)ssid.length();
    memcpy(&buf[i], ssid.c_str(), ssid.length()); i += ssid.length();
    buf[i++] = (uint8_t)pass.length();
    memcpy(&buf[i], pass.c_str(), pass.length()); i += pass.length();

    bool all = true;
    for (uint8_t k = 0; k < _peer_count; k++)
        all &= _send(_peers[k], LMSG_CREDS, buf, i);
    return all;
}

/**
 * Difunde credenciales a cualquier CableTester que este escuchando.
 *
 * POR QUE DIFUSION Y NO A LOS PEERS
 *
 * linkPushCreds() manda a los peers EMPAREJADOS, y en el caso que importa no
 * hay ninguno: se acaba de mover el equipo a otra planta, ninguna unidad
 * encuentra su red, todas abren su portal y ninguna conoce a las demas. Con
 * difusion basta configurar una por el movil.
 *
 * QUIEN LA ACEPTA — y quien no
 *
 * El receptor solo aplica credenciales si esta en rol SLAVE, que es el estado
 * de una unidad SIN red (portal o esperando master). Una unidad que ya esta
 * conectada y trabajando es MASTER y las ignora: nadie puede sacarla de su red
 * con una difusion.
 *
 * Se manda varias veces a proposito: ESP-NOW no confirma la difusion a nivel
 * de aplicacion, y el receptor puede estar ocupado sirviendo su propio portal.
 * Tres envios espaciados cuestan medio segundo y evitan tener que ir equipo por
 * equipo si se pierde uno.
 *
 * LIMITE: solo alcanza a quien este en el MISMO CANAL. Con todas en modo AP eso
 * se cumple porque el portal fija el canal 1 (ver wifi_portal.cpp).
 */
bool linkBroadcastCreds(const String& ssid, const String& pass, uint8_t repeticiones) {
    if (!_inited) return false;
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) return false;

    static const uint8_t DIFUSION[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    // La difusion NO puede ir cifrada: ESP-NOW solo cifra unicast.
    //
    // El peer se registra con ifidx STA aunque la unidad este sirviendo su
    // portal en modo AP: al levantar el softAP la libreria pasa a AP_STA, y en
    // AP_STA las dos interfaces COMPARTEN la unica radio y por tanto el canal
    // del AP (el 1, que fija el portal). Por eso el envio sale en el canal en
    // el que escuchan las demas.
    if (!_registerPeer(DIFUSION, false)) return false;

    uint8_t buf[200];
    uint8_t i = 0;
    buf[i++] = (uint8_t)ssid.length();
    memcpy(&buf[i], ssid.c_str(), ssid.length()); i += ssid.length();
    buf[i++] = (uint8_t)pass.length();
    memcpy(&buf[i], pass.c_str(), pass.length()); i += pass.length();

    bool alguno = false;
    for (uint8_t r = 0; r < (repeticiones ? repeticiones : 1); r++) {
        alguno |= _send(DIFUSION, LMSG_CREDS, buf, i);
        delay(150);
    }
    Serial.printf("[LINK] Credenciales difundidas para '%s' (%u envios).\n",
                  ssid.c_str(), (unsigned)(repeticiones ? repeticiones : 1));
    esp_now_del_peer(DIFUSION);
    return alguno;
}

bool linkPing() {
    if (!_inited || !_peer_count) return false;
    bool all = true;
    for (uint8_t k = 0; k < _peer_count; k++)
        all &= _send(_peers[k], LMSG_PING);
    return all;
}

// =============================================================
// Prueba distribuida — primitivas del MASTER
//
// Bloquean, pero bombean linkLoop() mientras esperan para que las
// respuestas de los hijos se procesen. No se llaman desde el callback.
// =============================================================
void linkSetTestHandlers(const LinkTestHandlers& h) { _th = h; }

static bool _waitUntil(bool (*done)(), uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (!done()) {
        if (millis() - t0 > timeout_ms) return false;
        linkLoop();
        delay(1);
    }
    return true;
}

static bool _driveAcked() { return _drive_acked; }

bool linkRemoteDrive(uint8_t peer_idx, uint8_t local_ch, bool level, uint32_t timeout_ms) {
    if (!_inited || peer_idx >= _peer_count) return false;
    uint8_t p[2] = { local_ch, (uint8_t)(level ? 1 : 0) };
    const uint32_t per_try = timeout_ms / SAMPLE_TRIES;
    for (uint8_t t = 0; t < SAMPLE_TRIES; t++) {
        _drive_acked = false;
        _send(_peers[peer_idx], LMSG_DRIVE, p, 2);
        uint32_t t0 = millis();
        while (!_drive_acked && (millis() - t0) < per_try) { linkLoop(); delay(1); }
        if (_drive_acked) return true;
    }
    return false;
}

static bool _allMasksIn() {
    for (uint8_t i = 0; i < _peer_count; i++) if (!_rx_got[i]) return false;
    return true;
}

bool linkSampleAll(bool level, uint32_t* masks_out,
                   int8_t drive_unit, int8_t drive_ch, uint32_t timeout_ms) {
    if (!_inited) return false;
    if (!_peer_count) return true;

    _sample_seq++;
    for (uint8_t i = 0; i < _peer_count; i++) { _rx_got[i] = false; _rx_mask[i] = 0xFFFFFFFFu; }

    // 3er byte: canal a excitar +1 para este hijo (0 = solo escucha).
    // 4o byte: canal WiFi del master. Ver LMSG_SAMPLE en el receptor.
    uint8_t p[4] = { (uint8_t)(level ? 1 : 0), _sample_seq, 0,
                     (uint8_t)WiFi.channel() };

    // Se pregunta a UN hijo a la vez, no a todos de golpe.
    //
    // Antes se mandaba el SAMPLE a los dos peers seguidos: ambos muestreaban y
    // contestaban en el mismo instante, y sus MASK colisionaban en el aire. Los
    // reintentos no ayudaban porque los dos volvian a reintentar a la vez y
    // colisionaban igual — por eso subir SAMPLE_TRIES no cambiaba nada.
    //
    // Medido: 40 de 40 ida-y-vuelta perfectas hablando con un hijo a la vez,
    // mientras el barrido con los dos en paralelo abortaba una y otra vez.
    //
    // Serializar no obliga a que los hijos muestreen a la vez: el que excita
    // SOSTIENE el nivel durante todo el barrido, asi que el desfase da igual.
    const uint32_t per_peer = timeout_ms / _peer_count;
    const uint32_t per_try  = per_peer / SAMPLE_TRIES;

    for (uint8_t i = 0; i < _peer_count; i++) {
        // Solo el dueño recibe el canal; los demas escuchan en alta impedancia.
        p[2] = (drive_unit == (int8_t)(i + 1) && drive_ch >= 0)
             ? (uint8_t)(drive_ch + 1) : 0;
        for (uint8_t t = 0; t < SAMPLE_TRIES && !_rx_got[i]; t++) {
            _send(_peers[i], LMSG_SAMPLE, p, 4);
            uint32_t t0 = millis();
            while (!_rx_got[i] && (millis() - t0) < per_try) { linkLoop(); delay(1); }
        }
    }

    for (uint8_t i = 0; i < _peer_count; i++) masks_out[i] = _rx_mask[i];
    return _allMasksIn();
}

/** Indice del primer peer que no contesto, o -1. Para mensajes de error utiles. */
bool linkRemoteRaw(uint8_t peer_idx, const void* ops, uint8_t n_ops,
                   int32_t* results, uint32_t timeout_ms) {
    if (!_inited || peer_idx >= _peer_count || !ops) return false;
    if (n_ops > LINK_RAW_MAX_OPS) n_ops = LINK_RAW_MAX_OPS;

    _raw_seq++;
    uint8_t p[2 + LINK_RAW_MAX_OPS * 4];
    p[0] = _raw_seq;
    p[1] = n_ops;
    memcpy(&p[2], ops, n_ops * 4);
    const uint8_t len = (uint8_t)(2 + n_ops * 4);

    const uint32_t per_try = timeout_ms / SAMPLE_TRIES;
    for (uint8_t t = 0; t < SAMPLE_TRIES && !_raw_got; t++) {
        _raw_got = false;
        _send(_peers[peer_idx], LMSG_RAW, p, len);
        uint32_t t0 = millis();
        while (!_raw_got && (millis() - t0) < per_try) { linkLoop(); delay(1); }
    }
    if (!_raw_got) return false;

    if (results) {
        for (uint8_t i = 0; i < n_ops; i++)
            results[i] = (i < _raw_res_n) ? _raw_res[i] : 0;
    }
    _raw_got = false;
    return true;
}

int8_t linkFirstSilentPeer() {
    for (uint8_t i = 0; i < _peer_count; i++) if (!_rx_got[i]) return (int8_t)i;
    return -1;
}

void linkReleaseAll() {
    if (!_inited) return;
    for (uint8_t i = 0; i < _peer_count; i++) _send(_peers[i], LMSG_RELEASE);
}

bool linkSendLeds(uint8_t peer_idx, uint16_t led_mask) {
    if (!_inited || peer_idx >= _peer_count) return false;
    uint8_t p[2] = { (uint8_t)(led_mask & 0xFF), (uint8_t)(led_mask >> 8) };
    return _send(_peers[peer_idx], LMSG_LEDS, p, 2);
}

void linkPrintInfo(Print& out) {
    out.println(F("------------------------------------------------"));
    out.println(F("  ENLACE ESP-NOW"));
    out.println(F("------------------------------------------------"));
    out.printf("  Rol ejecucion : %s\n", linkRoleName(_role));
    LinkRole cfg = linkGetConfiguredRole();
    out.printf("  Rol en NVS    : %s\n",
               cfg == LINK_ROLE_NONE ? "AUTO (se deduce)" : linkRoleName(cfg));
    out.printf("  Esta MAC      : %s\n", linkOwnMac().c_str());
    out.printf("  Unidad        : %s\n", wifiUnitId().c_str());
    out.printf("  Canal WiFi    : %d\n", WiFi.channel());
    out.printf("  ESP-NOW       : %s\n", _inited ? "activo" : "NO INICIADO");
    // Si 'cola llena' crece, el problema es de transmision local, NO de que el
    // otro extremo no conteste. Son dos fallos con el mismo sintoma.
    out.printf("  TX enviados   : %lu  (cola llena: %lu)\n",
               (unsigned long)_tx_sent, (unsigned long)_tx_fail);
    // OJO: 'cola llena' solo dice que el paquete se ENCOLO. Que salga no
    // significa que llegue. 'sin ACK' es la entrega real: el paquete se
    // transmitio y el otro extremo NO lo confirmo a nivel MAC.
    out.printf("  TX sin ACK    : %lu   <- entrega fallida (no es lo mismo)\n",
               (unsigned long)_tx_noack);
    // Si 'tirados' crece durante un fallo, el hijo SI contesto y fue el master
    // quien descarto la respuesta por no coincidir la ronda.
    out.printf("  MASK ok/tirad.: %lu / %lu  (ult. tirado seq=%u, esperaba=%u)\n",
               (unsigned long)_mask_ok, (unsigned long)_mask_rej,
               (unsigned)_last_rej_seq, (unsigned)_last_exp_seq);
    out.println(F("------------------------------------------------"));

    if (!_peer_count) {
        out.println(F("  Sin peers enlazados."));
        out.println(F("  Provision por USB:"));
        out.println(F("    link_mac                  ver MAC de esta unidad"));
        out.println(F("    link_master <MAC>         (en el hijo) apuntar al master"));
        out.println(F("    link_child  <MAC>         (en el master) añadir un hijo"));
    } else {
        out.printf("  %u peer(s):\n", _peer_count);
        for (uint8_t i = 0; i < _peer_count; i++) {
            const char* etiq = (_role == LINK_ROLE_SLAVE && i == 0) ? "master" : "hijo  ";
            out.printf("   #%u %s  %s  ", i, etiq, linkPeerMacStr(i).c_str());
            if (linkPeerOnline(i)) {
                int8_t r = _rssi[i];
                const char* q = r > -60 ? "excelente" : r > -75 ? "bueno"
                              : r > -85 ? "justo" : "MALO";
                out.printf("EN LINEA  %d dBm (%s)\n", r, q);
            } else if (_seen[i]) {
                out.printf("SIN RESPUESTA (hace %lu ms)\n",
                           (unsigned long)(millis() - _seen[i]));
            } else {
                out.println(F("nunca contesto"));
            }
        }
    }

    if (_pairing)
        out.printf("  EMPAREJANDO: quedan %lu s\n",
                   (unsigned long)(linkPairingRemainingMs() / 1000));
    out.println(F("------------------------------------------------"));
}
