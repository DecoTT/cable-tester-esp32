#!/usr/bin/env python3
"""
analog.py — Caracterización analógica de las líneas, orquestada desde la PC.

Mide lo que el mapeo digital no ve. El mapa dice "conectado / no conectado";
esto dice CUÁNTO, que es lo que distingue un crimp flojo de uno bueno antes de
que se convierta en un intermitente en campo.

    python analog.py 192.168.1.90 192.168.1.96

Dos medidas por línea:

  RESISTENCIA — un extremo a 3V3 por su resistencia serie, el otro a GND por
  la suya. El cable queda en medio de un divisor:

      3V3 ─[680]─ A ─[cable]─ B ─[680]─ GND

  Con cable ideal los dos lados leen 1.65 V. La DIFERENCIA entre A y B es
  proporcional a la resistencia del cable, y al ser diferencial se cancela
  sola el error de ganancia del ADC (+5.9% en este chip).

      Rc = 1360 · (Va − Vb) / (3.3 − (Va − Vb))

  FUGA — un extremo a 3V3, el otro en alta impedancia. Una línea sana llega al
  riel por los dos lados. Si baja, hay un camino a GND, y la resistencia de esa
  fuga sale del mismo divisor.

OJO con lo que esto NO puede ver: si el extremo lejano está pegado a GND, la
medida de resistencia da lo mismo que un cable perfecto a un pin en bajo. Por
eso las dos medidas van juntas y la de fuga es la que desempata.
"""

import argparse
import sys
import urllib.request

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from sweep import Unit, sweep, build_nets, ch_name, discover   # noqa: E402

R_SERIES = 680.0
VREF = 3.3

# Umbrales. Heredados de config.h para que firmware y PC digan lo mismo.
RC_OK = 5.0        # conexión sana
RC_WARN = 50.0     # sospechosa: crimp flojo o soldadura fría
LEAK_OK_V = 3.00   # por encima de esto, sin fuga


def mv(unit, local_ch):
    """Lee el ADC del canal local, en voltios. Crudo, sin corregir ganancia."""
    side = 0 if local_ch < 16 else 1
    ch = local_ch % 16
    r = unit.raw([f"adc_rd {side} {ch}"])[0]
    return None if r is None else r / 1000.0


def set_line(unit, local_ch, mode):
    """mode: 'hi' fuerza alto, 'lo' fuerza bajo, 'z' alta impedancia."""
    u, bit = (0, local_ch) if local_ch < 16 else (1, local_ch - 16)
    if mode == "z":
        unit.raw([f"io_dir {u} 0xFFFF", f"io_dir {1 - u} 0xFFFF"])
        return
    unit.raw([
        f"io_dir {u} {0xFFFF & ~(1 << bit)}",
        f"io_dir {1 - u} 0xFFFF",
        f"io_wr  {u} {(1 << bit) if mode == 'hi' else 0}",
        "delay_us 2000",
    ])


def r_from_delta(va, vb, clamp=True):
    """Resistencia del cable a partir de la diferencia entre extremos.

    clamp=False deja pasar valores negativos, que hacen falta para promediar
    las dos direcciones sin sesgar el resultado a cero.
    """
    d = va - vb
    if d >= VREF:
        return float("inf")
    if d <= 0 and clamp:
        return 0.0
    return 1360.0 * d / (VREF - d)


def r_bidir(ua, la, ub, lb):
    """Resistencia midiendo en LOS DOS SENTIDOS y promediando.

    MEDIDO: en un sentido salian 21.8 ohm en las 32 lineas de un arnes bueno;
    en el otro, 0.0. El desajuste era constante (~11 ohm) y NO era resistencia:
    era la diferencia de ganancia entre los ADC de las dos placas.

    El metodo es diferencial para que el error del ADC se cancele, pero eso
    solo vale si es el MISMO ADC. Entre unidades hay dos chips distintos y el
    sesgo se cuela como resistencia falsa. Con el umbral de "sospechosa" en
    5 ohm, el tester marcaba el 100% de las lineas sanas.

    Promediando ida y vuelta el sesgo se cancela: aparece con signo opuesto en
    cada sentido. Devuelve (resistencia, sesgo_estimado).
    """
    set_line(ua, la, "hi"); set_line(ub, lb, "lo")
    r_ida = r_from_delta(mv(ua, la), mv(ub, lb), clamp=False)
    set_line(ub, lb, "hi"); set_line(ua, la, "lo")
    r_vta = r_from_delta(mv(ub, lb), mv(ua, la), clamp=False)
    return max(0.0, (r_ida + r_vta) / 2.0), (r_ida - r_vta) / 2.0


def leak_ohm(v):
    """Resistencia de fuga a GND deducida de la caída en alta impedancia."""
    if v >= VREF - 0.01:
        return float("inf")
    if v <= 0.001:
        return 0.0
    return R_SERIES * v / (VREF - v)


def pairs_from_map(units):
    """Descubre las parejas con un barrido, en vez de darlas por supuestas."""
    conn = sweep(units, verbose=False)
    nets, _ = build_nets(conn, sum(u.n for u in units))
    out = []
    for net in nets:
        if len(net) != 2:
            continue                       # solo parejas limpias
        out.append(tuple(net))
    return out, nets


def locate(units, g):
    for u in units:
        if u.base <= g < u.base + u.n:
            return u, g - u.base
    raise ValueError(g)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hosts", nargs="*")
    ap.add_argument("--auto", action="store_true")
    a = ap.parse_args()

    hosts = a.hosts or discover()
    if not hosts:
        print("No se encontro ninguna unidad.")
        return 1

    units, base = [], 0
    for i, h in enumerate(hosts):
        u = Unit(h, i, base)
        base += u.n
        units.append(u)
    print("Unidades: " + ", ".join(f"U{u.index}={u.host} ({u.n} ch)" for u in units))

    print("\nDescubriendo las lineas con un barrido...")
    pairs, nets = pairs_from_map(units)
    raros = [n for n in nets if len(n) != 2]
    print(f"  {len(pairs)} pareja(s) limpia(s)"
          + (f", {len(raros)} net(s) de mas de 2 canales (no se miden)" if raros else ""))
    if not pairs:
        print("  Nada que medir.")
        return 1

    print(f"\n{'LINEA':<22}{'Va':>8}{'Vb':>8}{'R cable':>10}   {'V fuga A':>9}{'V fuga B':>9}   ESTADO")
    print("-" * 88)

    n_ok = n_warn = n_bad = 0
    for ga, gb in pairs:
        ua, la = locate(units, ga)
        ub, lb = locate(units, gb)
        name = f"{ch_name(ga, units)}<->{ch_name(gb, units)}"

        # --- Resistencia, en los dos sentidos ---
        rc, sesgo = r_bidir(ua, la, ub, lb)
        set_line(ua, la, "hi"); set_line(ub, lb, "lo")
        va, vb = mv(ua, la), mv(ub, lb)

        # --- Fuga: A arriba, B en alta impedancia ---
        set_line(ub, lb, "z")
        set_line(ua, la, "hi")
        fa, fb = mv(ua, la), mv(ub, lb)

        if None in (va, vb, fa, fb):
            print(f"{name:<22}{'ERROR de lectura':>40}")
            n_bad += 1
            continue

        peor = min(fa, fb)
        estado, mark = "OK", ""
        if peor < LEAK_OK_V:
            estado, mark = f"FUGA A GND ({leak_ohm(peor):.0f} ohm)", "  <<<"
            n_bad += 1
        elif rc > RC_WARN:
            estado, mark = "RESISTENCIA ALTA", "  <<<"
            n_bad += 1
        elif rc > RC_OK:
            estado = "sospechosa"
            n_warn += 1
        else:
            n_ok += 1

        print(f"{name:<22}{va:>8.3f}{vb:>8.3f}{rc:>9.1f}o{sesgo:>7.1f}o   "
              f"{fa:>8.3f}{fb:>8.3f}   {estado}{mark}")

    for u in units:
        u.release()

    print("-" * 94)
    print(f"  {n_ok} sana(s)   {n_warn} sospechosa(s)   {n_bad} defectuosa(s)"
          f"   de {len(pairs)}")
    print(f"\n  Umbrales: sana <{RC_OK:.0f} ohm, sospechosa <{RC_WARN:.0f} ohm,"
          f" sin fuga >{LEAK_OK_V:.2f} V")
    return 0


if __name__ == "__main__":
    sys.exit(main())
