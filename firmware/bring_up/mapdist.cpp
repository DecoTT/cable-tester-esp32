#include "mapdist.h"
#include "link.h"
#include "shift595.h"

extern Shift595 sr;

// Matriz de conexiones descubiertas. 96×96 bits = 1152 bytes: cabe de sobra
// y evita recalcular nada al construir los nets.
static uint8_t _conn[MAP_CH_MAX][MAP_CH_MAX / 8];

static inline void _setConn(uint8_t i, uint8_t j) {
    _conn[i][j >> 3] |= (uint8_t)(1u << (j & 7));
    _conn[j][i >> 3] |= (uint8_t)(1u << (i & 7));
}
static bool _isConn(uint8_t i, uint8_t j, void*) {
    return (_conn[i][j >> 3] >> (j & 7)) & 1;
}

uint8_t mapDistChannels() {
    uint8_t units = (uint8_t)(1 + linkPeerCount());
    if (units * MAP_CH_LOCAL > MAP_CH_MAX) units = MAP_CH_MAX / MAP_CH_LOCAL;
    return (uint8_t)(units * MAP_CH_LOCAL);
}

// Un barrido: excita el canal global src al nivel dado y recoge las mascaras
// de TODAS las unidades. masks[0] = master, masks[1..] = hijos.
static bool _sweep(TCA9535& u1, TCA9535& u2, uint8_t src, bool level, uint32_t* masks) {
    const uint8_t unit  = (uint8_t)(src / MAP_CH_LOCAL);
    const uint8_t local = (uint8_t)(src % MAP_CH_LOCAL);

    // 1. El dueño excita y MANTIENE. Debe ocurrir antes de cualquier muestreo:
    //    las demas unidades leen mientras el sigue excitando.
    if (unit == 0) {
        mapDriveHold(u1, u2, local, level);
    } else {
        if (!linkRemoteDrive((uint8_t)(unit - 1), local, level)) return false;
    }

    // 2. Muestreo. Cada unidad hace su ciclo atomico por su cuenta; el
    //    desfase entre ellas no importa porque el driver sostiene el nivel.
    //    El master se pide a si mismo primero, que es instantaneo.
    masks[0] = mapSampleLocal(u1, u2, (unit == 0) ? local : -1, level);
    if (!linkSampleAll(level, &masks[1], (int8_t)unit, (int8_t)local)) return false;

    return true;
}

// Reintento a nivel de CANAL, no solo de paquete.
//
// Los reintentos de linkSampleAll() reenvian el mismo SAMPLE dentro de una
// ventana corta. Eso cubre una perdida suelta, pero el ESP-NOW falla a RAFAGAS:
// medido en vivo, un hijo puede quedarse mudo varios cientos de ms seguidos y
// agotar los cinco intentos. Entonces se abortaba el barrido de 96 canales
// entero por una racha de ruido.
//
// Aqui se rehace el canal COMPLETO: vuelve a excitar (el hijo dueño puede
// haber perdido tambien el DRIVE) y vuelve a muestrear. Entre intentos se deja
// un hueco creciente para no reintentar dentro de la misma rafaga.
static const uint8_t SWEEP_TRIES = 4;

// Quien no contesto en el ultimo muestreo, para el mensaje de error.
static const char* _silentName() {
    int8_t sp = linkFirstSilentPeer();
    if (sp < 0) return "?";
    return (sp == 0) ? "L1" : "L2";
}

static bool _sweepRetry(TCA9535& u1, TCA9535& u2, uint8_t src, bool level,
                        uint32_t* masks) {
    for (uint8_t t = 0; t < SWEEP_TRIES; t++) {
        if (_sweep(u1, u2, src, level, masks)) return true;
        delay((uint32_t)(t + 1) * 40);
    }
    return false;
}

bool mapDistRun(TCA9535& u1, TCA9535& u2, MapResult& out, Print& log) {
    const uint8_t peers = linkPeerCount();
    if (!peers) return mapRun(u1, u2, out, log);   // una sola unidad

    const uint8_t N = mapDistChannels();
    log.printf("[MAP]  Distribuido: %u canales en %u unidad(es)...\n", N, 1 + peers);

    memset(_conn, 0, sizeof(_conn));
    uint32_t mh[1 + LINK_MAX_PEERS], ml[1 + LINK_MAX_PEERS];
    const uint32_t t_fin = millis() + MAPDIST_DEADLINE_MS;

    for (uint8_t src = 0; src < N; src++) {
        if ((int32_t)(millis() - t_fin) >= 0) {
            log.printf("[MAP]  ABORTADO por plazo (%lu s) en %s. "
                       "Revisa el enlace con 'link_info'.\n",
                       (unsigned long)(MAPDIST_DEADLINE_MS / 1000),
                       mapChName(src).c_str());
            linkReleaseAll(); mapRelease(u1, u2);
            return false;
        }
        if (!_sweepRetry(u1, u2, src, true, mh)) {
            // Nombrar al culpable: "no contesto" sin decir QUIEN obligaba a ir
            // a buscarlo a mano en cada fallo.
            log.printf("[MAP]  ERROR: sin respuesta en fase ALTA de %s  (mudo: %s)\n",
                       mapChName(src).c_str(), _silentName());
            linkReleaseAll(); mapRelease(u1, u2);
            return false;
        }
        if (!_sweepRetry(u1, u2, src, false, ml)) {
            log.printf("[MAP]  ERROR: sin respuesta en fase BAJA de %s  (mudo: %s)\n",
                       mapChName(src).c_str(), _silentName());
            linkReleaseAll(); mapRelease(u1, u2);
            return false;
        }

        // Conectado si SIGUIO las dos transiciones. Un pin flotante retiene
        // la precarga y por eso falla una de las dos.
        for (uint8_t unit = 0; unit <= peers; unit++) {
            // NOTA: 0xFFFFFFFF tambien es una lectura LEGITIMA (los 32 pines
            // altos tras una precarga alta). Saltar la unidad aqui es inocuo
            // porque con ml todo a unos ningun canal cumple !ml[b], asi que no
            // habria conexiones que registrar de todos modos. Se deja el corte
            // por I2C caido, pero NO sirve para distinguir los dos casos: si
            // hiciera falta distinguirlos, hay que propagar la validez aparte.
            if (mh[unit] == 0xFFFFFFFFu && ml[unit] == 0xFFFFFFFFu) continue;
            for (uint8_t b = 0; b < MAP_CH_LOCAL; b++) {
                uint8_t g = (uint8_t)(unit * MAP_CH_LOCAL + b);
                if (g == src || g >= N) continue;
                if (((mh[unit] >> b) & 1) && !((ml[unit] >> b) & 1)) _setConn(src, g);
            }
        }

        if ((src % 16) == 15) log.printf("[MAP]  ...%u/%u\n", src + 1, N);
    }

    linkReleaseAll();
    mapRelease(u1, u2);

    mapBuildNets(out, N, _isConn, nullptr);
    log.printf("[MAP]  Listo: %u net(s) en %u canales.\n", out.net_count, out.channels);
    return true;
}

int mapDistSelfCheck(TCA9535& u1, TCA9535& u2, uint8_t* status, Print& log) {
    const uint8_t peers = linkPeerCount();
    const uint8_t N     = mapDistChannels();
    log.printf("[CHK]  Autodiagnostico de %u canales en %u unidad(es)...\n", N, 1 + peers);

    memset(status, CHK_OK, MAP_CH_MAX);
    uint32_t mh[1 + LINK_MAX_PEERS], ml[1 + LINK_MAX_PEERS];
    int bad = 0;
    const uint32_t t_fin = millis() + MAPDIST_DEADLINE_MS;

    for (uint8_t g = 0; g < N; g++) {
        if ((int32_t)(millis() - t_fin) >= 0) {
            log.printf("[CHK]  ABORTADO por plazo en %s.\n", mapChName(g).c_str());
            break;
        }
        const uint8_t unit  = (uint8_t)(g / MAP_CH_LOCAL);
        const uint8_t local = (uint8_t)(g % MAP_CH_LOCAL);

        if (!_sweepRetry(u1, u2, g, true, mh) || !_sweepRetry(u1, u2, g, false, ml)) {
            log.printf("[CHK]  ERROR de comunicacion en %s\n", mapChName(g).c_str());
            status[g] = CHK_ERR; bad++;
            continue;
        }

        if (mh[unit] == 0xFFFFFFFFu || ml[unit] == 0xFFFFFFFFu) {
            status[g] = CHK_ERR; bad++;
            continue;
        }

        // El canal se lee a si mismo: el registro INPUT del TCA9535 refleja
        // el nivel real del pin aunque este configurado como salida.
        bool up   = (mh[unit] >> local) & 1;
        bool down = !((ml[unit] >> local) & 1);
        if (!up)        { status[g] = CHK_STUCK_LOW;  bad++; }
        else if (!down) { status[g] = CHK_STUCK_HIGH; bad++; }
    }

    linkReleaseAll();
    mapRelease(u1, u2);
    log.printf("[CHK]  Listo: %d canal(es) defectuoso(s) de %u.\n", bad, N);
    return bad;
}

void mapDistLeds(const MapResult& golden, const MapResult& dut) {
    // Esta unidad
    sr.setAllLeds(false);
    sr.setLeds(mapLedMask(golden, dut, 0));
    // Cada hijo, con el veredicto de SUS 32 canales
    for (uint8_t i = 0; i < linkPeerCount(); i++)
        linkSendLeds(i, mapLedMask(golden, dut, (uint8_t)(i + 1)));
}
