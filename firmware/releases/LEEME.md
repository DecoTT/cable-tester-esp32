# Binarios publicados

El `.bin` de cada versión, para actualizar por OTA **sin compilar**:

```bash
python tools/ct.py --ota 192.168.1.90 firmware/releases/bring_up-1.7.0.bin
```

Compilar hace falta solo para tocar el firmware; `tools/build.py` se descarga
arduino-cli y el core de ESP32 la primera vez (unos 500 MB).

| Versión | Qué trae |
|---|---|
| 1.7.0 | Disparador con modo `avisar` (cuenta pulsaciones, mide la PC) y contador en `/api/status` |
| 1.6.0 | La PC orquesta por HTTP; ESP-NOW solo aprovisiona WiFi |
| 1.5.0 | Direcciones I²C y enables del MUX corregidos contra el esquemático |
| 1.4.0 | Mapa de bits de los LEDs corregido (van intercalados verde/rojo) |
