"""
discover.py — Descubre testers Cable Tester 32CH en la red local por mDNS.

Es la pieza que evita teclear la IP: la app de PC llama a esto, obtiene la
lista de testers con sus metadatos, y elige (o auto-elige por SKU).

Requisito:
    pip install zeroconf

Uso:
    python discover.py            # lista todos los testers durante 3 s
    python discover.py --sku SKU4589   # solo el master de ese SKU

La PC NO necesita WiFi: basta con que su cable y el WiFi del tester lleguen
al mismo router. mDNS viaja por multicast dentro de la LAN.
"""

import sys
import time
import argparse
from zeroconf import Zeroconf, ServiceBrowser, ServiceListener

SERVICE = "_cabletester._tcp.local."


class _Listener(ServiceListener):
    def __init__(self):
        self.found = {}

    def add_service(self, zc, type_, name):
        info = zc.get_service_info(type_, name)
        if not info:
            return
        txt = {k.decode(): v.decode() for k, v in info.properties.items() if v is not None}
        ip = ".".join(str(b) for b in info.addresses[0]) if info.addresses else "?"
        self.found[name] = {
            "ip":   ip,
            "port": info.port,
            "host": info.server.rstrip("."),
            **txt,
        }

    def update_service(self, zc, type_, name):
        self.add_service(zc, type_, name)

    def remove_service(self, zc, type_, name):
        self.found.pop(name, None)


def discover(timeout=3.0):
    """Devuelve lista de dicts: ip, port, host, unit, fw, api, role, sku."""
    zc = Zeroconf()
    listener = _Listener()
    ServiceBrowser(zc, SERVICE, listener)
    try:
        time.sleep(timeout)
    finally:
        zc.close()
    return list(listener.found.values())


def find_master(sku=None, timeout=3.0):
    """El master del SKU dado (o cualquier master si sku=None), o None."""
    for t in discover(timeout):
        if t.get("role") != "MASTER":
            continue
        if sku and t.get("sku") != sku:
            continue
        return t
    return None


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sku", help="buscar solo el master de este SKU")
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()

    if args.sku:
        t = find_master(args.sku, args.timeout)
        if not t:
            print(f"No se encontro un MASTER para {args.sku}")
            sys.exit(1)
        print(f"{t['sku']}  ->  http://{t['ip']}:{t['port']}/   (unidad {t.get('unit')}, fw {t.get('fw')})")
        sys.exit(0)

    testers = discover(args.timeout)
    if not testers:
        print("Ningun tester encontrado. ¿Misma LAN? ¿Firewall bloqueando multicast?")
        sys.exit(1)

    print(f"{'UNIDAD':7} {'ROL':7} {'SKU':12} {'FW':5} {'API':4} {'IP':16} HOST")
    for t in sorted(testers, key=lambda x: x.get("unit", "")):
        print(f"{t.get('unit','?'):7} {t.get('role','?'):7} {t.get('sku','-'):12} "
              f"{t.get('fw','?'):5} {t.get('api','?'):4} {t['ip']:16} {t['host']}")
