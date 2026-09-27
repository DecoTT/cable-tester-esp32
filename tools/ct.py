#!/usr/bin/env python3
"""
ct.py — Bridge de línea de comandos para el Cable Tester 32CH.

Manda comandos al tester por HTTP y escribe la respuesta en stdout. Pensado
para trabajar en vivo: conectar un cable, leer, conectar otro, leer.

    python ct.py map                       usa el tester recordado
    python ct.py 192.168.1.90 map          indica el tester
    python ct.py 2898 map                  por número de unidad (mDNS)
    python ct.py "map; selfcheck"          varios comandos en orden
    python ct.py --host 192.168.1.90       recuerda ese tester y sale
    python ct.py --list                    testers en la red
    python ct.py --ota                     sube build/bring_up.ino.bin por OTA
    python ct.py --ota 192.168.1.90 x.bin  OTA a un tester concreto

OJO: /cmd devuelve TEXTO para leerlo una persona, y su formato puede cambiar
sin aviso. Esta herramienta es para depurar. La app de producción habla por
/api/* (JSON versionado) y no debe parsear nada de aquí. Ver docs/API.md.
"""

import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

STATE = Path(__file__).parent / ".ct_host"
TIMEOUT = 90          # 'map' distribuido puede tardar bastante


def _post(host, cmd, timeout=TIMEOUT):
    url = f"http://{host}/cmd"
    req = urllib.request.Request(
        url, data=cmd.encode("utf-8"),
        headers={"Content-Type": "text/plain; charset=utf-8"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def ota(host, path, timeout=180):
    """Sube firmware por OTA. Devuelve (ok, mensaje).

    Seguro por diseño: se escribe en la ranura que NO esta corriendo y el
    arranque solo cambia si el hash cuadra. Una subida cortada deja el equipo
    arrancando la version vieja. Y si algo saliera muy mal, el bootloader de
    ROM esta en silicio: siempre se puede recuperar por USB.
    """
    data = Path(path).read_bytes()
    boundary = "----ct32boundary"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="fw.bin"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()

    req = urllib.request.Request(
        f"http://{host}/update", data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}",
                 "Content-Length": str(len(body))},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return True, r.read().decode("utf-8", errors="replace").strip()
    except Exception as e:
        return False, str(e)


def _status(host, timeout=8):
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def discover(timeout=4):
    """Testers en la red, vía mDNS. Devuelve [(unidad, ip, fw, rol), ...]."""
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        return []

    found = []

    class L:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=2000)
            if not info or not info.addresses:
                return
            ip = ".".join(str(b) for b in info.addresses[0])
            txt = {k.decode(): v.decode() for k, v in (info.properties or {}).items()
                   if k and v}
            found.append((txt.get("unit", "?"), ip,
                          txt.get("fw", "?"), txt.get("role", "?")))

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
    return found


def resolve(token):
    """IP, nombre mDNS o número de unidad → host utilizable."""
    if not token:
        return None
    if token.count(".") == 3 or ":" in token or token.endswith(".local"):
        return token
    if token.isdigit() and len(token) == 4:          # número de unidad
        for unit, ip, _, _ in discover():
            if unit == token:
                return ip
        return f"cabletester-{token}.local"
    return token


def remembered():
    return STATE.read_text().strip() if STATE.exists() else None


def main(argv):
    args = list(argv[1:])

    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0

    if args[0] == "--list":
        rows = discover()
        if not rows:
            print("No se encontro ningun tester. "
                  "Revisa que la PC este en la misma red (sin doble NAT).")
            return 1
        print(f"{'UNIDAD':<8}{'ROL':<8}{'FW':<8}IP")
        for unit, ip, fw, role in sorted(rows):
            print(f"{unit:<8}{role:<8}{fw:<8}{ip}")
        return 0

    if args[0] == "--ota":
        # ct.py --ota [ip] [ruta.bin]
        rest = args[1:]
        host = None
        if rest and (rest[0].count(".") == 3 or rest[0].endswith(".local")
                     or (rest[0].isdigit() and len(rest[0]) == 4)):
            host = resolve(rest[0]); rest = rest[1:]
        host = host or remembered()
        if not host:
            print("No hay tester recordado. ct.py --host <ip>"); return 2
        binp = rest[0] if rest else str(Path(__file__).parent / "build" /
                                        "bring_up.ino.bin")
        if not Path(binp).exists():
            print(f"No existe {binp}"); return 2

        try:
            before = _status(host).get("fw", "?")
        except Exception:
            before = "?"
        kb = Path(binp).stat().st_size // 1024
        print(f"OTA -> {host}  (tiene {before}, subiendo {kb} KB)")

        ok, msg = ota(host, binp)
        if not ok:
            print(f"Fallo la subida: {msg}")
            print("El equipo sigue con la version anterior — el arranque solo")
            print("cambia si el hash cuadra. Reintenta, o flashea por USB.")
            return 1

        print("Subido. Esperando el reinicio...")
        for _ in range(30):
            time.sleep(2)
            try:
                after = _status(host, timeout=4).get("fw", "?")
            except Exception:
                continue
            if after != before:
                print(f"Listo: {before} -> {after}")
                return 0
            print(f"  sigue reportando {after}...")
        print("No se vio cambiar la version. Comprueba con: ct.py --list")
        return 1

    if args[0] == "--host":
        if len(args) < 2:
            print("Uso: ct.py --host <ip|unidad>")
            return 2
        host = resolve(args[1])
        STATE.write_text(host)
        try:
            st = _status(host)
            print(f"Tester {st.get('unit','?')} ({st.get('role','?')}) "
                  f"fw {st.get('fw','?')} en {host} — recordado.")
        except Exception as e:
            print(f"Recordado {host}, pero no contesta: {e}")
            return 1
        return 0

    # ¿El primer argumento es un tester o ya es el comando?
    host = None
    first = args[0]
    if first.count(".") == 3 or first.endswith(".local") or \
       (first.isdigit() and len(first) == 4):
        host = resolve(first)
        args = args[1:]

    if host is None:
        host = remembered()
        if not host:
            print("No hay tester recordado. Usa: ct.py --host <ip|unidad>  "
                  "o  ct.py --list")
            return 2

    cmd_line = " ".join(args).strip()
    if not cmd_line:
        print("Falta el comando.")
        return 2

    # Varios comandos en una llamada: se mandan EN ORDEN y por separado, no
    # concatenados, porque el firmware interpreta una linea por comando.
    rc = 0
    for cmd in [c.strip() for c in cmd_line.split(";") if c.strip()]:
        try:
            out = _post(host, cmd)
        except urllib.error.URLError as e:
            print(f"[ct] {host} no contesta: {e.reason}")
            return 1
        except Exception as e:
            print(f"[ct] error hablando con {host}: {e}")
            return 1
        if len(cmd_line.split(";")) > 1:
            print(f"\n=== {cmd} ===")
        print(out.rstrip())
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
