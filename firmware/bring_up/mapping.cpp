#include "mapping.h"
#include "config.h"
#include <Preferences.h>

#define PREF_NS_MAP  "map"
#define PREF_GOLDEN  "golden"     // blob de 'channels' bytes
#define PREF_GNAME   "gname"
#define PREF_GCH     "gch"        // nº de canales del golden
#define PREF_GSKU    "gsku"
#define PREF_GCRC    "gcrc"
#define PREF_GMETA   "gmeta"
#define PREF_CHCNT   "chcnt"

// Tiempo de asentamiento tras cambiar dirección o nivel. Con 680 Ω serie y
// la capacidad del arnés, 1 ms sobra. Validado con 16 nets limpios.
static const uint32_t SETTLE_US = 1000;

extern void printLine(Print& out, int n);

// CRC16-CCITT: detecta que el golden se perdió o corrompió en vez de
// comparar contra basura sin enterarse.
static uint16_t _crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// =============================================================
// Nombres de canal: "M.J00" master, "L1.J19" / "L2.J_SP" hijos
// =============================================================
String mapChName(uint8_t g) {
    uint8_t unit  = g / MAP_CH_LOCAL;
    uint8_t local = g % MAP_CH_LOCAL;
    char pin[6];
    if (local == 31) snprintf(pin, sizeof(pin), "J_SP");
    else             snprintf(pin, sizeof(pin), "J%02u", local);
    char buf[12];
    if (unit == 0) snprintf(buf, sizeof(buf), "M.%s", pin);
    else           snprintf(buf, sizeof(buf), "L%u.%s", unit, pin);
    return String(buf);
}

void mapRangeBounds(uint8_t r, uint8_t& lo, uint8_t& hi) {
    // 32 canales de una unidad en 5 rangos: 7, 7, 6, 6, 6
    static const uint8_t start[MAP_LED_RANGES + 1] = {0, 7, 14, 20, 26, 32};
    if (r >= MAP_LED_RANGES) { lo = hi = 0; return; }
    lo = start[r];
    hi = start[r + 1] - 1;
}

// =============================================================
// Union-find
// =============================================================
static uint8_t _uf[MAP_CH_MAX];

static uint8_t _find(uint8_t x) {
    while (_uf[x] != x) { _uf[x] = _uf[_uf[x]]; x = _uf[x]; }
    return x;
}
static void _union(uint8_t a, uint8_t b) {
    a = _find(a); b = _find(b);
    if (a != b) _uf[b] = a;
}

// =============================================================
// Acceso local a los 32 canales
//   canal 0-15  → U1 bits 0-15  (J00-J15)
//   canal 16-31 → U2 bits 0-15  (J16-J30, J_SP)
// =============================================================
static void _allOutput(TCA9535& u1, TCA9535& u2, bool high) {
    uint16_t v = high ? 0xFFFF : 0x0000;
    u1.setDirection(0x0000); u2.setDirection(0x0000);
    u1.writePort(v);         u2.writePort(v);
}

static void _onlyDrives(TCA9535& u1, TCA9535& u2, int src) {
    if (src < 0) { u1.setDirection(0xFFFF); u2.setDirection(0xFFFF); return; }
    if (src < 16) {
        u1.setDirection((uint16_t)(0xFFFF & ~(1u << src)));
        u2.setDirection(0xFFFF);
    } else {
        u1.setDirection(0xFFFF);
        u2.setDirection((uint16_t)(0xFFFF & ~(1u << (src - 16))));
    }
}

static void _driveLevel(TCA9535& u1, TCA9535& u2, int src, bool high) {
    if (src < 0) return;
    if (src < 16) u1.writePort(high ? (uint16_t)(1u << src) : 0x0000);
    else          u2.writePort(high ? (uint16_t)(1u << (src - 16)) : 0x0000);
}

static bool _readAll(TCA9535& u1, TCA9535& u2, uint32_t& mask) {
    int32_t a = u1.readPort();
    int32_t b = u2.readPort();
    if (a < 0 || b < 0) return false;
    mask = ((uint32_t)(uint16_t)b << 16) | (uint16_t)a;
    return true;
}

// =============================================================
// mapSampleLocal — el ciclo atómico. Lo usan local Y distribuido.
// =============================================================
uint32_t mapSampleLocal(TCA9535& u1, TCA9535& u2, int drive_ch, bool level, bool* ok) {
    // Precarga al nivel OPUESTO al que se va a excitar: asi un pin conectado
    // tiene que CAMBIAR para seguir al driver, y uno flotante se queda.
    _allOutput(u1, u2, !level);
    delayMicroseconds(SETTLE_US);

    _onlyDrives(u1, u2, drive_ch);
    _driveLevel(u1, u2, drive_ch, level);
    delayMicroseconds(SETTLE_US);

    uint32_t m = 0;
    bool good = _readAll(u1, u2, m);
    if (ok) *ok = good;
    return good ? m : 0xFFFFFFFFu;
}

void mapDriveHold(TCA9535& u1, TCA9535& u2, int ch, bool level) {
    _onlyDrives(u1, u2, ch);
    _driveLevel(u1, u2, ch, level);
    delayMicroseconds(SETTLE_US);
}

void mapRelease(TCA9535& u1, TCA9535& u2) {
    _allOutput(u1, u2, false);
    u1.setDirection(0x0000); u1.writePort(0x0000);
    u2.setDirection(0xFFFF);
}

// =============================================================
// mapBuildNets — agrupa canales en nets a partir de una funcion de conexion
// =============================================================
void mapBuildNets(MapResult& out, uint8_t channels,
                  bool (*connected)(uint8_t, uint8_t, void*), void* ctx) {
    memset(&out, 0, sizeof(out));
    out.channels = channels;
    for (uint8_t i = 0; i < channels; i++) _uf[i] = i;

    for (uint8_t i = 0; i < channels; i++)
        for (uint8_t j = (uint8_t)(i + 1); j < channels; j++)
            if (connected(i, j, ctx)) _union(i, j);

    uint8_t size[MAP_CH_MAX] = {0};
    for (uint8_t i = 0; i < channels; i++) size[_find(i)]++;

    uint8_t id_of_root[MAP_CH_MAX] = {0};
    uint8_t next_id = 1;
    for (uint8_t i = 0; i < channels; i++) {
        uint8_t r = _find(i);
        if (size[r] < 2) { out.net_of[i] = 0; continue; }
        if (!id_of_root[r]) id_of_root[r] = next_id++;
        out.net_of[i] = id_of_root[r];
    }
    out.net_count = (uint8_t)(next_id - 1);
    out.valid     = true;
}

// =============================================================
// mapRun — mapeo LOCAL de 32 canales
// =============================================================
bool mapRun(TCA9535& u1, TCA9535& u2, MapResult& out, Print& log) {
    memset(&out, 0, sizeof(out));
    out.channels = MAP_CH_LOCAL;
    for (uint8_t i = 0; i < MAP_CH_LOCAL; i++) _uf[i] = i;

    log.println(F("[MAP]  Recorriendo 32 canales (lectura complementaria)..."));

    for (uint8_t src = 0; src < MAP_CH_LOCAL; src++) {
        bool okh = false, okl = false;
        uint32_t mh = mapSampleLocal(u1, u2, src, true,  &okh);
        uint32_t ml = mapSampleLocal(u1, u2, src, false, &okl);
        if (!okh || !okl) {
            log.printf("[MAP]  ERROR I2C con %s activo\n", mapChName(src).c_str());
            return false;
        }
        for (uint8_t j = 0; j < MAP_CH_LOCAL; j++) {
            if (j == src) continue;              // el propio pin siempre "sigue"
            if (((mh >> j) & 1) && !((ml >> j) & 1)) _union(src, j);
        }
    }

    mapRelease(u1, u2);

    uint8_t size[MAP_CH_MAX] = {0};
    for (uint8_t i = 0; i < MAP_CH_LOCAL; i++) size[_find(i)]++;
    uint8_t id_of_root[MAP_CH_MAX] = {0};
    uint8_t next_id = 1;
    for (uint8_t i = 0; i < MAP_CH_LOCAL; i++) {
        uint8_t r = _find(i);
        if (size[r] < 2) { out.net_of[i] = 0; continue; }
        if (!id_of_root[r]) id_of_root[r] = next_id++;
        out.net_of[i] = id_of_root[r];
    }
    out.net_count = (uint8_t)(next_id - 1);
    out.valid     = true;

    log.printf("[MAP]  Listo: %u net(s) detectado(s).\n", out.net_count);
    return true;
}

// =============================================================
void mapPrint(const MapResult& m, Print& out) {
    printLine(out, 56);
    out.printf("  MAPA — %u canal(es), %u net(s)\n", m.channels, m.net_count);
    printLine(out, 56);
    if (!m.valid) { out.println(F("  (sin datos)")); printLine(out, 56); return; }

    for (uint8_t n = 1; n <= m.net_count; n++) {
        out.printf("  net %-2u : ", n);
        for (uint8_t i = 0; i < m.channels; i++)
            if (m.net_of[i] == n) out.printf("%s ", mapChName(i).c_str());
        out.println();
    }
    out.print(F("  aislados: "));
    bool any = false;
    for (uint8_t i = 0; i < m.channels; i++)
        if (!m.net_of[i]) { out.printf("%s ", mapChName(i).c_str()); any = true; }
    if (!any) out.print(F("(ninguno)"));
    out.println();
    printLine(out, 56);
}

void mapPrintJson(const MapResult& m, const char* name, Print& out) {
    out.printf("{\"job\":\"%s\",\"channels\":%u,\"nets\":[", name ? name : "", m.channels);
    bool first_net = true;
    for (uint8_t n = 1; n <= m.net_count; n++) {
        if (!first_net) out.print(',');
        first_net = false;
        out.print('[');
        bool first = true;
        for (uint8_t i = 0; i < m.channels; i++) {
            if (m.net_of[i] != n) continue;
            if (!first) out.print(',');
            first = false;
            out.printf("\"%s\"", mapChName(i).c_str());
        }
        out.print(']');
    }
    out.println(F("]}"));
}

// =============================================================
// Golden
// =============================================================
bool mapSaveGolden(const MapResult& m, const String& name) {
    if (!m.valid || !m.channels) return false;
    Preferences p;
    p.begin(PREF_NS_MAP, false);
    bool ok = p.putBytes(PREF_GOLDEN, m.net_of, m.channels) == m.channels;
    p.putUChar(PREF_GCH, m.channels);
    p.putString(PREF_GNAME, name.length() ? name : String("golden"));
    p.putUShort(PREF_GCRC, _crc16(m.net_of, m.channels));
    p.end();
    return ok;
}

bool mapLoadGolden(MapResult& m, String& name_out) {
    Preferences p;
    p.begin(PREF_NS_MAP, true);
    uint8_t ch = p.getUChar(PREF_GCH, 0);
    size_t  n  = p.getBytesLength(PREF_GOLDEN);
    bool ok = (ch > 0 && ch <= MAP_CH_MAX && n == ch);
    uint16_t crc_saved = 0;
    if (ok) {
        memset(m.net_of, 0, sizeof(m.net_of));
        p.getBytes(PREF_GOLDEN, m.net_of, ch);
        name_out  = p.getString(PREF_GNAME, "golden");
        crc_saved = p.getUShort(PREF_GCRC, 0);
    }
    p.end();
    if (!ok) { m.valid = false; m.channels = 0; return false; }

    // Un golden corrupto es peor que uno ausente: compararia contra basura.
    if (crc_saved && _crc16(m.net_of, ch) != crc_saved) { m.valid = false; return false; }

    uint8_t mx = 0;
    for (uint8_t i = 0; i < ch; i++) if (m.net_of[i] > mx) mx = m.net_of[i];
    m.channels  = ch;
    m.net_count = mx;
    m.valid     = true;
    return true;
}

void mapClearGolden() {
    Preferences p; p.begin(PREF_NS_MAP, false);
    p.remove(PREF_GOLDEN); p.remove(PREF_GNAME); p.remove(PREF_GCRC); p.remove(PREF_GCH);
    p.end();
}

void mapClearAll() {
    Preferences p; p.begin(PREF_NS_MAP, false); p.clear(); p.end();
}

bool mapHasGolden() {
    Preferences p; p.begin(PREF_NS_MAP, true);
    uint8_t ch = p.getUChar(PREF_GCH, 0);
    bool ok = ch > 0 && p.getBytesLength(PREF_GOLDEN) == ch;
    p.end();
    return ok;
}

uint16_t mapGoldenCrc() {
    Preferences p; p.begin(PREF_NS_MAP, true);
    uint16_t c = p.getUShort(PREF_GCRC, 0); p.end();
    return c;
}

uint8_t mapGetChCount() {
    Preferences p; p.begin(PREF_NS_MAP, true);
    uint8_t n = p.getUChar(PREF_CHCNT, MAP_CH_LOCAL);
    p.end();
    return (n == 16 || n == 32) ? n : MAP_CH_LOCAL;
}

bool mapSetChCount(uint8_t n) {
    if (n != 16 && n != 32) return false;
    Preferences p; p.begin(PREF_NS_MAP, false);
    p.putUChar(PREF_CHCNT, n);
    p.end();
    return true;
}

String mapGetSku() {
    Preferences p; p.begin(PREF_NS_MAP, true);
    String s = p.getString(PREF_GSKU, ""); p.end();
    return s;
}
bool mapSetSku(const String& sku) {
    if (sku.length() > 40) return false;
    Preferences p; p.begin(PREF_NS_MAP, false);
    bool ok = p.putString(PREF_GSKU, sku) > 0 || sku.length() == 0;
    p.end(); return ok;
}
String mapGetMeta() {
    Preferences p; p.begin(PREF_NS_MAP, true);
    String s = p.getString(PREF_GMETA, ""); p.end();
    return s;
}
bool mapSetMeta(const String& meta) {
    if (meta.length() > 120) return false;
    Preferences p; p.begin(PREF_NS_MAP, false);
    bool ok = p.putString(PREF_GMETA, meta) > 0 || meta.length() == 0;
    p.end(); return ok;
}

// =============================================================
// Comparación
// =============================================================
static inline bool _joined(const MapResult& m, uint8_t i, uint8_t j) {
    return m.net_of[i] && (m.net_of[i] == m.net_of[j]);
}

int mapCompare(const MapResult& golden, const MapResult& dut, Print& out) {
    printLine(out, 56);
    out.println(F("  COMPARACION CONTRA GOLDEN"));
    printLine(out, 56);

    if (golden.channels != dut.channels) {
        out.printf("  ERROR: el golden es de %u canales y la medicion de %u.\n",
                   golden.channels, dut.channels);
        out.println(F("  ¿Falta alguna unidad enlazada, o el golden es de otra configuracion?"));
        printLine(out, 56);
        return -1;
    }

    int abiertos = 0, cortos = 0;
    const uint8_t N = golden.channels;
    for (uint8_t i = 0; i < N; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < N; j++) {
            bool g = _joined(golden, i, j);
            bool d = _joined(dut, i, j);
            if (g && !d) {
                out.printf("  ABIERTO : %s <-> %s\n", mapChName(i).c_str(), mapChName(j).c_str());
                abiertos++;
            } else if (!g && d) {
                out.printf("  CORTO   : %s <-> %s\n", mapChName(i).c_str(), mapChName(j).c_str());
                cortos++;
            }
        }
    }

    printLine(out, 56);
    if (!abiertos && !cortos) out.println(F("  >>> ARNES OK — identico al golden <<<"));
    else {
        out.printf("  ABIERTOS=%d  CORTOS=%d\n", abiertos, cortos);
        out.println(F("  >>> ARNES RECHAZADO <<<"));
    }
    printLine(out, 56);
    return abiertos + cortos;
}

int mapFaultsJson(const MapResult& golden, const MapResult& dut, Print& out) {
    int n = 0;
    out.print('[');
    const uint8_t N = golden.channels;
    for (uint8_t i = 0; i < N; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < N; j++) {
            bool g = _joined(golden, i, j);
            bool d = _joined(dut, i, j);
            if (g == d) continue;
            if (n) out.print(',');
            out.printf("{\"type\":\"%s\",\"a\":\"%s\",\"b\":\"%s\"}",
                       g ? "open" : "short", mapChName(i).c_str(), mapChName(j).c_str());
            n++;
        }
    }
    out.print(']');
    return n;
}

// =============================================================
// LEDs — cada unidad muestra SUS 32 canales en 5 rangos
// =============================================================
uint16_t mapLedMask(const MapResult& golden, const MapResult& dut, uint8_t unit_index) {
    bool bad[MAP_CH_MAX] = {false};
    const uint8_t N = golden.channels;
    for (uint8_t i = 0; i < N; i++)
        for (uint8_t j = (uint8_t)(i + 1); j < N; j++)
            if (_joined(golden, i, j) != _joined(dut, i, j)) { bad[i] = bad[j] = true; }

    const uint16_t REDS[MAP_LED_RANGES] = {
        SR_LED1_RED, SR_LED2_RED, SR_LED3_RED, SR_LED4_RED, SR_LED5_RED };
    const uint16_t GRNS[MAP_LED_RANGES] = {
        SR_LED1_GREEN, SR_LED2_GREEN, SR_LED3_GREEN, SR_LED4_GREEN, SR_LED5_GREEN };

    const uint16_t base = (uint16_t)unit_index * MAP_CH_LOCAL;
    uint16_t mask = 0;
    for (uint8_t r = 0; r < MAP_LED_RANGES; r++) {
        uint8_t lo, hi;
        mapRangeBounds(r, lo, hi);
        bool fail = false;
        for (uint8_t c = lo; c <= hi; c++) {
            uint16_t g = base + c;
            if (g < N && bad[g]) fail = true;
        }
        mask |= fail ? REDS[r] : GRNS[r];
    }
    return mask;
}
