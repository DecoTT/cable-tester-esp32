#!/usr/bin/env python3
"""
sweep.py — Barrido de mapeo orquestado desde la PC.

La PC habla con CADA unidad por HTTP, sin master ni hijos. No hay ESP-NOW de
por medio, así que no hay canales que sincronizar, ni emparejamiento, ni roles.
Cuando algo falla, dice qué unidad y qué error.

    python sweep.py 192.168.1.90 192.168.1.91 192.168.1.92
    python sweep.py --auto              descubre las unidades por mDNS
    python sweep.py --auto --repeat 15  repite y compara huellas

Las unidades se numeran por el ORDEN en que se pasan: la primera es U0
(canales 0-31), la segunda U1 (32-63), etc. Ese orden define los nombres de
canal, así que tiene que ser siempre el mismo para un SKU dado.
"""

import argparse
import hashlib
import os
import json
import sys
import time
import urllib.error
import urllib.request

CH_PER_UNIT = 32


class Unit:
    """Una unidad del tester, hablada por HTTP."""

    def __init__(self, host, index, base=0):
        self.host = host
        self.index = index
        self.base = base          # primer canal global de esta unidad
        # Canales POBLADOS, que no son siempre 32: un tester de 16 posiciones
        # tiene J16-J31 al aire, sin conector ni resistencia serie, y leerlos
        # es interpretar ruido — daba una huella distinta en cada barrido.
        self.n = 32
        try:
            with urllib.request.urlopen(f"http://{host}/api/status", timeout=6) as r:
                self.n = json.loads(r.read().decode()).get("channels", 32)
        except Exception:
            pass

    def raw(self, ops, timeout=10):
        """Ejecuta ops (una por línea) y devuelve la lista de resultados.

        Van en UNA petición a propósito: una secuencia dir → wr → rd partida a
        la mitad no mide nada, así que el firmware las ejecuta sin ceder el
        control.
        """
        body = "\n".join(ops).encode()

        # Reintentos: una petición perdida no puede tirar un barrido de 30 s.
        # Es seguro reintentar porque las ops son idempotentes — fijan estado y
        # leen, no acumulan nada. El nivel de excitación lo sostiene la unidad
        # dueña entre peticiones, así que repetir una lectura da lo mismo.
        ultimo = None
        for intento in range(3):
            try:
                req = urllib.request.Request(
                    f"http://{self.host}/api/raw", data=body,
                    headers={"Content-Type": "text/plain"},
                )
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    d = json.loads(r.read().decode())
                break
            except Exception as e:
                ultimo = e
                time.sleep(0.2 * (intento + 1))
        else:
            raise RuntimeError(f"U{self.index} ({self.host}) no responde tras "
                               f"3 intentos: {ultimo}")

        if "error" in d:
            raise RuntimeError(f"U{self.index} ({self.host}): {d['error']}")
        return d["results"]

    def sample(self, drive_ch, level):
        """Ciclo de muestreo completo. drive_ch=-1 → solo escucha.

        El firmware devuelve la máscara como entero CON signo, así que una
        máscara con el bit 31 puesto (canal J_SP excitado) llega negativa.
        Se normaliza aquí: null significa error, un negativo no.
        """
        r = self.raw([f"sample {drive_ch + 1} {1 if level else 0}"])[0]
        return None if r is None else (r & 0xFFFFFFFF)

    def sample2(self, drive_ch, level):
        """Dos muestras de la misma fase en UNA peticion.

        Ahorra la mitad de los viajes de red sin cambiar el metodo: las dos
        ops se ejecutan seguidas en el firmware, sin ceder el control.
        """
        op = f"sample {drive_ch + 1} {1 if level else 0}"
        r = self.raw([op, op])
        if r[0] is None or r[1] is None:
            return None
        return (r[0] & 0xFFFFFFFF, r[1] & 0xFFFFFFFF)

    def drive_hold(self, ch, level):
        """Excita un canal y lo MANTIENE mientras las demás muestrean."""
        lvl = 0xFFFF if level else 0x0000
        u, bit = (0, ch) if ch < 16 else (1, ch - 16)
        ops = [
            f"io_dir {u} {0xFFFF & ~(1 << bit)}",
            f"io_dir {1 - u} 0xFFFF",
            f"io_wr  {u} {(1 << bit) if level else 0}",
            "delay_us 1000",
        ]
        self.raw(ops)

    def release(self):
        self.raw(["io_dir 0 0x0000", "io_wr 0 0x0000", "io_dir 1 0xFFFF"])


def ch_name(g, units):
    for u in units:
        if u.base <= g < u.base + u.n:
            local = g - u.base
            pin = "J_SP" if local == 31 else f"J{local:02d}"
            return f"{'M' if u.index == 0 else f'L{u.index}'}.{pin}"
    return f"?{g}"


def sweep(units, verbose=True):
    """Barrido completo. Devuelve {canal: set(canales conectados)}."""
    n = sum(u.n for u in units)
    conn = {i: set() for i in range(n)}

    for src in range(n):
        owner = next(i for i, u in enumerate(units)
                     if u.base <= src < u.base + u.n)
        local = src - units[owner].base
        masks = {}
        for level in (True, False):
            # El dueño excita y SOSTIENE: las demás leen mientras tanto, y el
            # desfase entre ellas da igual porque el nivel se mantiene.
            units[owner].drive_hold(local, level)
            for u in units:
                # Dos muestras por fase, y solo valen los bits en que COINCIDEN.
                #
                # Un pin realmente conectado sigue al driver siempre; uno al
                # aire se mueve por acoplamiento y no repite. Sin esto, con
                # nada conectado salia un net fantasma en ~2 de cada 8 barridos.
                # Para 'alto' se exige alto en las dos (AND); para 'bajo', bajo
                # en las dos (OR, que deja 0 solo si ambas son 0).
                pair = u.sample2(local if u.index == owner else -1, level)
                if pair is None:
                    m = None
                else:
                    a, b = pair
                    m = (a & b) if level else (a | b)
                masks.setdefault(u.index, {})[level] = m

        # Conectado si SIGUIÓ las dos transiciones. Un pin flotante retiene la
        # precarga y por eso falla una de las dos.
        for ui, ph in masks.items():
            hi, lo = ph[True], ph[False]
            if hi is None or lo is None:
                raise RuntimeError(f"I2C caido en U{ui} con {ch_name(src, units)}")
            for b in range(units[ui].n):
                g = units[ui].base + b
                if g == src:
                    continue
                if (hi >> b) & 1 and not ((lo >> b) & 1):
                    conn[src].add(g)
                    conn[g].add(src)

        if verbose and (src % 16) == 15:
            print(f"  ...{src + 1}/{n}", flush=True)

    for u in units:
        u.release()
    return conn


def build_nets(conn, n):
    """Union-find: agrupa canales en nets."""
    parent = list(range(n))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for i, js in conn.items():
        for j in js:
            a, b = find(i), find(j)
            if a != b:
                parent[b] = a

    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    nets = sorted([g for g in groups.values() if len(g) > 1])
    isolated = sorted(i for g in groups.values() if len(g) == 1 for i in g)
    return nets, isolated


def fingerprint(nets, isolated, units):
    """Huella del resultado COMPLETO: nets Y aislados.

    Los aislados TIENEN que entrar. Si la huella solo cubre los nets, un
    barrido con 0 nets produce la huella de la cadena vacia, y comparar
    repeticiones deja de comparar nada: sale "identicas" aunque los canales
    aislados cambien de un barrido a otro. Paso justo eso.
    """
    body = "\n".join(" ".join(ch_name(c, units) for c in net) for net in nets)
    body += "\nAISLADOS: " + " ".join(ch_name(c, units) for c in isolated)
    return hashlib.md5(body.encode()).hexdigest()[:8]


def report(nets, isolated, units):
    total = sum(u.n for u in units)
    print(f"\n  MAPA — {total} canales, {len(nets)} nets")
    print("  " + "-" * 56)
    for i, net in enumerate(nets, 1):
        print(f"  net {i:<3}: " + " ".join(ch_name(c, units) for c in net))
    print("  aislados: " +
          (" ".join(ch_name(c, units) for c in isolated) or "(ninguno)"))
    return fingerprint(nets, isolated, units)


def save_golden(path, nets, isolated, units):
    """Guarda el golden en la PC.

    La fuente de verdad es este fichero, NO el ESP32: si una unidad se borra o
    se cambia, el golden sigue aqui. Se guarda tambien la configuracion de
    unidades porque el ORDEN define los nombres de canal — un golden con las
    unidades en otro orden no significa lo mismo.
    """
    data = {
        "version": 1,
        "creado": time.strftime("%Y-%m-%d %H:%M:%S"),
        "unidades": [{"host": u.host, "canales": u.n} for u in units],
        "total_canales": sum(u.n for u in units),
        "huella": fingerprint(nets, isolated, units),
        "nets": [[ch_name(c, units) for c in net] for net in nets],
        "aislados": [ch_name(c, units) for c in isolated],
    }
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    return data


def _pairs(nets):
    """Conexiones como pares ordenados, para comparar sin depender del orden."""
    out = set()
    for net in nets:
        for i, a in enumerate(net):
            for b in net[i + 1:]:
                out.add(tuple(sorted((a, b))))
    return out


def check_golden(path, nets, isolated, units):
    """Compara contra el golden. Devuelve (ok, lista de fallos).

    Se comparan CONEXIONES, no nets. Si se compararan nets, un solo corto
    cambiaria la numeracion de todos los siguientes y saldrian decenas de
    fallos por un unico defecto.
    """
    with open(path, encoding="utf-8") as f:
        g = json.load(f)

    fallos = []
    esperado_total = g.get("total_canales")
    real_total = sum(u.n for u in units)
    if esperado_total != real_total:
        fallos.append(("CONFIG", f"el golden es de {esperado_total} canales y "
                                 f"este montaje tiene {real_total}"))
        return False, fallos

    esp = _pairs(g["nets"])
    real = _pairs([[ch_name(c, units) for c in net] for net in nets])

    for a, b in sorted(esp - real):
        fallos.append(("ABIERTO", f"{a} <-> {b}   esperada y NO encontrada"))

    # Los cortos se agrupan por el nodo que forman.
    #
    # Un solo puente fisico une dos parejas y crea CUATRO parejas nuevas. Sin
    # agrupar, el operador ve cuatro fallos y tiene que deducir que son uno.
    # Agrupando ve "estos 4 canales estan unidos y no deberian", que es lo que
    # tiene que ir a mirar.
    extra = real - esp
    if extra:
        padre = {}

        def find(x):
            padre.setdefault(x, x)
            while padre[x] != x:
                padre[x] = padre[padre[x]]
                x = padre[x]
            return x

        for a, b in extra:
            ra, rb = find(a), find(b)
            if ra != rb:
                padre[rb] = ra
        grupos = {}
        for a, b in extra:
            grupos.setdefault(find(a), set()).update((a, b))
        for g in sorted(grupos.values(), key=lambda x: sorted(x)):
            fallos.append(("CORTO", "unidos y NO deberian: " +
                           " ".join(sorted(g))))
    return not fallos, fallos


def validar_modulos(units):
    """Salud de los modulos, SIN arnes. Devuelve la lista de canales malos.

    Es el primer paso de cualquier puesta en marcha, y es BLOQUEANTE: sin esto
    no hay forma de saber si un canal aislado es culpa del arnes o de la placa,
    porque los dos se ven igual en el mapa. Y si se captura un golden con una
    placa defectuosa, el defecto queda congelado dentro del patron de
    referencia y el tester deja de vigilar esas lineas PARA SIEMPRE.

    Cada canal se excita en alto con todo lo demas en alta impedancia. Un canal
    sano llega al riel (3.3 V); uno pegado a masa se hunde.
    """
    from analog import set_line, mv, leak_ohm, locate, LEAK_OK_V
    malos = []
    total = sum(u.n for u in units)
    for g in range(total):
        u, l = locate(units, g)
        for o in units:
            if o is not u:
                set_line(o, 0, "z")
        set_line(u, l, "hi")
        v = mv(u, l)
        if v is None or v < LEAK_OK_V:
            malos.append((ch_name(g, units), v, leak_ohm(v) if v is not None else -1))
    for u in units:
        u.release()
    return malos, total



def medir_resistencias(nets, units):
    """Resistencia de cada pareja limpia. Devuelve [(nombre, R, sesgo), ...].

    Una linea puede dar "conectada" en el mapa y estar mal: con 680 ohm en
    serie, un contacto de 116 ohm deja pasar el nivel logico igual. MEDIDO en
    un arnes golden real — 116 ohm frente a 1.0 ohm de sus vecinas, y ademas
    intermitente. Un tester de sola continuidad lo habria aprobado.

    Por eso la captura del golden mide y se bloquea: un crimp flojo congelado
    en el patron de referencia hace que el tester acepte ese defecto siempre.
    """
    from analog import r_bidir, locate, RC_WARN
    out = []
    for net in nets:
        if len(net) != 2:
            continue
        ga, gb = net
        ua, la = locate(units, ga)
        ub, lb = locate(units, gb)
        r, ses = r_bidir(ua, la, ub, lb)
        out.append((f"{ch_name(ga, units)}<->{ch_name(gb, units)}", r, ses))
    for u in units:
        u.release()
    return out


def revisar_resistencias(nets, units):
    """Imprime y devuelve True si todas pasan."""
    from analog import RC_OK, RC_WARN
    med = medir_resistencias(nets, units)
    malas = [(n, r) for n, r, _ in med if r > RC_WARN]
    sosp = [(n, r) for n, r, _ in med if RC_OK < r <= RC_WARN]
    print(f"\n  Resistencia de {len(med)} linea(s):")
    if malas:
        for n, r in malas:
            print(f"    {n:<24}{r:>8.1f} ohm   <<< RESISTENCIA ALTA")
    if sosp:
        for n, r in sosp:
            print(f"    {n:<24}{r:>8.1f} ohm   sospechosa")
    if not malas and not sosp:
        peor = max((r for _, r, _ in med), default=0.0)
        print(f"    todas OK (la peor, {peor:.1f} ohm)")
    return not malas


def discover(timeout=4):
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        return []
    found = []

    class L:
        def add_service(self, zc, t, name):
            info = zc.get_service_info(t, name, timeout=2000)
            if info and info.addresses:
                found.append(".".join(str(b) for b in info.addresses[0]))

        def update_service(self, *a):
            pass

        def remove_service(self, *a):
            pass

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, "_cabletester._tcp.local.", L())
        time.sleep(timeout)
    finally:
        zc.close()
    return sorted(found)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hosts", nargs="*", help="IPs de las unidades, EN ORDEN")
    ap.add_argument("--auto", action="store_true", help="descubrir por mDNS")
    ap.add_argument("--repeat", type=int, default=1, help="repetir y comparar")
    ap.add_argument("--save-golden", metavar="FICHERO",
                    help="guarda este barrido como golden")
    ap.add_argument("--esperadas", type=int, metavar="N",
                    help="nº de conexiones que ingenieria aprueba para el SKU; "
                         "sin esto no hay forma de saber que FALTA un hilo")
    ap.add_argument("--check", metavar="FICHERO",
                    help="compara contra un golden y reporta fallos")
    ap.add_argument("--modulos", action="store_true",
                    help="valida los modulos SIN arnes (hacerlo ANTES del golden)")
    a = ap.parse_args()

    hosts = a.hosts
    if a.auto or not hosts:
        hosts = discover()
        if not hosts:
            print("No se encontro ninguna unidad.")
            return 1
        print("Descubiertas (ORDEN IMPORTA):", ", ".join(hosts))

    units = []
    base = 0
    for i, h in enumerate(hosts):
        u = Unit(h, i, base)
        base += u.n
        units.append(u)
    print("Unidades: " + ", ".join(f"U{u.index}={u.host} ({u.n} canales)" for u in units))
    huellas = {}

    if a.modulos:
        print("\nVALIDACION DE MODULOS — el arnes tiene que estar DESCONECTADO")
        malos, total = validar_modulos(units)
        if not malos:
            print(f"  >>> {total}/{total} OK — ningun canal pegado a GND <<<")
            return 0
        print(f"\n  {'CANAL':<10}{'V':>9}{'R a GND':>10}")
        print("  " + "-" * 29)
        for nm, v, r in malos:
            print(f"  {nm:<10}{v:>9.3f}{r:>10.0f}")
        print(f"\n  >>> {len(malos)} canal(es) DEFECTUOSO(S) de {total} <<<")
        print("  NO capturar un golden asi: congelaria el defecto.")
        return 1

    for r in range(1, a.repeat + 1):
        print(f"\n=== Barrido {r}/{a.repeat} — {len(units)} unidad(es) ===")
        t0 = time.time()
        try:
            conn = sweep(units, verbose=(a.repeat == 1))
        except (urllib.error.URLError, RuntimeError, TimeoutError) as e:
            print(f"  FALLO: {e}")
            continue
        nets, iso = build_nets(conn, sum(u.n for u in units))
        if a.repeat == 1:
            h = report(nets, iso, units)
            print(f"  huella {h}   ({time.time() - t0:.1f}s)")

            if a.save_golden:
                # Medir ANTES de guardar: un crimp flojo no se ve en el mapa.
                # Un hilo que FALTA no tiene resistencia que medir, asi que la
                # comprobacion anterior no lo ve. Y al capturar el PRIMER golden
                # de un SKU no hay contra que comparar. Lo unico que lo cierra
                # es el numero que aprueba ingenieria.
                #
                # MEDIDO en dos arneses golden reales: uno tenia las 46
                # conexiones pero un contacto a 116 ohm; el otro tenia los
                # contactos perfectos y le faltaba un hilo. La comprobacion de
                # resistencia APROBO el segundo.
                if a.esperadas is not None and len(nets) != a.esperadas:
                    print(f"\n  >>> RECHAZADO: {len(nets)} conexiones, "
                          f"ingenieria espera {a.esperadas} <<<")
                    print("  Falta o sobra cableado. NO se guarda el golden.")
                    return 1
                if a.esperadas is None:
                    print("\n  AVISO: sin --esperadas no se puede detectar "
                          "un hilo que FALTE.")
                if not revisar_resistencias(nets, units):
                    print("\n  >>> NO se guarda el golden <<<")
                    print("  Repara esas lineas: congelarlas haria que el "
                          "tester acepte el defecto siempre.")
                    return 1
                d = save_golden(a.save_golden, nets, iso, units)
                print(f"\n  GOLDEN guardado en {a.save_golden}")
                print(f"  {len(d['nets'])} nets, {len(d['aislados'])} aislados, "
                      f"huella {d['huella']}")

            if a.check:
                ok, fallos = check_golden(a.check, nets, iso, units)
                print()
                if ok:
                    print("  >>> OK — coincide con el golden <<<")
                else:
                    print(f"  >>> RECHAZADO — {len(fallos)} fallo(s) <<<")
                    for tipo, txt in fallos:
                        print(f"    {tipo:<8} {txt}")
                return 0 if ok else 1
        else:
            h = fingerprint(nets, iso, units)
            print(f"  {len(nets)} nets, {len(iso)} aislados   "
                  f"huella {h}   ({time.time() - t0:.1f}s)")
        huellas.setdefault(h, []).append(r)

    if a.repeat > 1:
        print(f"\nResultados distintos: {len(huellas)}")
        for h, v in huellas.items():
            print(f"  {h} -> barridos {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
