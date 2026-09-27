# Cable Tester 32CH — firmware y herramientas

Tester de arneses multipolar de 32 líneas por equipo, sobre **ESP32-C3**.
Varios equipos se combinan para arneses más grandes: tres dan 96 canales.

Este repositorio lleva el **firmware**, las **herramientas de línea de comandos**
y la **documentación técnica**. La aplicación de producción está en el
repositorio hermano
**[`cable-tester-esp32-app`](https://github.com/DecoTT/cable-tester-esp32-app)**,
y espera encontrar este clonado **al lado**, en la misma carpeta padre.

```
D:\loquesea\
  ├── CableTester32\        ← este repositorio
  └── CableTester32App\     ← la app
```

---

## Por dónde empezar

| Si quieres… | Lee |
|---|---|
| Retomar el proyecto sin releer nada más | **[ESTADO.md](ESTADO.md)** |
| Saber qué puede medir y por qué los umbrales están donde están | [docs/CARACTERIZACION.md](docs/CARACTERIZACION.md) |
| El contrato JSON del firmware | [docs/API.md](docs/API.md) |
| Lo que hay que corregir en la próxima revisión de PCB | [docs/REV_NEXT.md](docs/REV_NEXT.md) |

---

## La arquitectura en una línea

**La PC orquesta; el equipo mide y obedece.**

```
PC (bridge Node) ──HTTP /api/raw──▶ ESP32  ×N
```

El golden vive en la PC, no en el ESP32: si un equipo se borra o se sustituye,
se monta otro y se le dice qué job correr. ESP-NOW se quedó **solo** para
mandar credenciales WiFi a un equipo que aún no está en la red — como
transporte de medida se descartó tras probar y descartar nueve hipótesis con
medida (ver ESTADO.md §1).

---

## Herramientas

```bash
python tools/ct.py --list                 # equipos en la red (mDNS)
python tools/ct.py --ota <ip> <fichero>   # actualizar firmware
python tools/ct.py <ip> "<comando>"       # consola

python tools/sweep.py <ip1> <ip2> --modulos           # validar placas SIN arnés
python tools/sweep.py <ip1> <ip2> --save-golden f.json --esperadas 46
python tools/sweep.py <ip1> <ip2> --check f.json      # código de salida 0/1

python tools/analog.py <ip1> <ip2>        # resistencia y fuga por línea
```

El entorno de Python lo prepara `setup.bat` de la app, o a mano:

```bash
python -m venv .venv
.venv\Scripts\activate
pip install -r tools/requirements.txt
```

---

## Actualizar un equipo

El `.bin` publicado está en [`firmware/releases/`](firmware/releases/), así que
**no hace falta compilar**:

```bash
python tools/ct.py --ota 192.168.1.90 firmware/releases/bring_up-1.7.0.bin
```

Para tocar el firmware, `python tools/build.py` descarga arduino-cli y el core
de ESP32 la primera vez (unos 500 MB) y compila `firmware/bring_up`.

> **Un módulo virgen no arranca con solo la app.** No tiene bootloader ni tabla
> de particiones: hay que grabarle `merged.bin` a `0x0`. `tools/flasher.py` lo
> detecta y lo avisa.

---

## Lo que NO va en este repositorio

Los **goldens** son el cableado de un cliente y de su fixture, no del producto.
Viajan en la exportación de la app (Configuración → Datos de la estación), nunca
por git.
