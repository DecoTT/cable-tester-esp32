# Cable Tester 32 Channels — Arquitectura del Sistema
**Versión:** 0.4 — Verificado en PCB física  
**Fecha:** 2026-08-03  
**Esquemático fuente:** EasyEDA Rev 1.0, 2026-03-02  
**Módulo ESP32:** ESP32-C3-MINI-1-N4, SBC Rev1.1

### Documentos relacionados

| Documento | Contenido |
|---|---|
| [CARACTERIZACION.md](CARACTERIZACION.md) | **Qué puede validar el tester y qué no.** Umbrales, resolución, huellas de referencia y el sesgo entre ADCs. Leer antes de tocar cualquier umbral |
| [MAPPING_DESIGN.md](MAPPING_DESIGN.md) | Motor de mapeo por golden |
| [LINK_DESIGN.md](LINK_DESIGN.md) | Enlace ESP-NOW entre unidades |
| [API.md](API.md) | Contrato JSON para el software de PC |
| [REV_NEXT.md](REV_NEXT.md) | Cambios pendientes de PCB |

---

## 1. Arquitectura física (dos PCBs apiladas)

El sistema se compone de **dos módulos separados**:

```
┌─────────────────────────────────────────┐
│  Módulo ESP32-C3 (SBC Rev1.1)           │
│  ← plugs via headers H1/H2 (1x15, 2.54mm) →  │
└─────────────────────────────────────────┘
                 ↕ apilado
┌─────────────────────────────────────────┐
│  Módulo Tester 32P                       │
│  U1, U2, U3, U4, U5, U6                 │
│  CN1, CN2 (conectores para arnés)        │
└─────────────────────────────────────────┘
```

El módulo ESP32 se conecta al tester via **H1 y H2** (headers hembra en la PCB tester).  
El tester tiene su propia BOM separada del módulo ESP.

---

## 2. Hardware

### 2.1 Módulo ESP32-C3 (SBC Rev1.1)

| Campo | Valor |
|-------|-------|
| MCU | ESP32-C3-MINI-1-N4 (4MB flash, 40MHz crystal) |
| USB | USB nativo IO18/IO19 (sin puente CP2102/CH340) |
| Regulador | ME6211C33M5G-N → 3.3V |
| LED RGB | WS2812B en IO8 con R9=330Ω |
| Botón BOOT | IO9 → GND (pull-up 10kΩ) |
| Botón RESET | EN → GND (pull-up 10kΩ) |
| Headers | J1 y J2: 1×15 pins, pitch 2.54mm |

### 2.2 Módulo Tester 32P

| Ref | IC | Función |
|-----|----|---------|
| U1 | TCA9535PWR | GPIO expander 16-bit I2C — J00-J15 (CN1) |
| U2 | TCA9535PWR | GPIO expander 16-bit I2C — J16-J30+J_SPARE (CN2) |
| U3 | 74HC595D | Shift register — bits 0-7 → LEDs (C1-C5 Rojo, C1-C3 Verde) |
| U4 | 74HC595D | Shift register — bits 8-15 → LEDs (C4-C5 Verde) + MUX control |
| U5 | CD74HC4067M96 | MUX 16:1 analógico — Scope Lite CN1 (J00-J15) |
| U6 | CD74HC4067M96 | MUX 16:1 analógico — Scope Lite CN2 (J16-J30) |
| CN1 | JST B20B-PHDSS-B(LF)(SN) | Conector 20 pines — lado A |
| CN2 | JST B20B-PHDSS-B(LF)(SN) | Conector 20 pines — lado B |
| R4-R35 | 680Ω × 32 | Serie por línea activa (protección de salidas) |
| R1,R38-R46 | 2.2kΩ × 10 | Serie para LEDs (C1_Red..C5_Green) |
| R36, R37 | 4.7kΩ × 2 | Pull-ups I2C (SDA y SCL → 3V3) |
| R47 | 1kΩ | Serie ADC_MUX_OUT → ADC_SCOPE |
| C10 | 22pF | Filtro ADC_SCOPE a GND |

---

## 3. GPIO del ESP32-C3 — CONFIRMADOS

> Fuente: esquemático EasyEDA Rev 1.0, header H2 del módulo tester.

| GPIO | Señal | Función | Notas |
|------|-------|---------|-------|
| IO0 | LED_DATA | 74HC595 U3 serial data in | Puerto libre |
| IO1 | LED_CLK | 74HC595 U3/U4 shift clock | Puerto libre |
| IO2 | LED_LATCH | 74HC595 U3/U4 storage clock | Strapping pin — pull-up externo OK |
| IO3 | ADC_SCOPE | ADC input desde MUX (vía R47+C10) | ADC1_CH3, libre |
| IO4 | LINK_RX | UART inter-módulo RX | Fase 3 |
| IO5 | LINK_TX | UART inter-módulo TX | Fase 3 |
| IO8 | I2C_SCL | I2C clock — R36 4.7kΩ pull-up | Strapping + LED RGB en módulo |
| IO9 | I2C_SDA | I2C data — R37 4.7kΩ pull-up | Strapping + BOOT button en módulo |
| IO10 | BTN_START | Botón inicio de test | INPUT_PULLUP |
| IO18 | USB D- | USB nativo ESP32-C3 | Reservado |
| IO19 | USB D+ | USB nativo ESP32-C3 | Reservado |
| IO6, IO7 | — | No conectados en tester | Disponibles para expansión |

> **Nota IO8/IO9:** Aunque son strapping pins y tienen funciones en el módulo ESP (SCL=LED_RGB, SDA=BOOT), 
> los pull-ups de 4.7kΩ en la PCB tester mantienen los niveles correctos durante el boot.  
> IO9 en HIGH durante reset = modo normal (no bootloader). ✓

---

## 4. 74HC595 — Mapa de 16 bits (U3 + U4 daisy-chain)

**Cómo funciona la cadena:**  
LED_DATA → U3(SI) → [shift 8] → U3(QH') → U4(SI) → [shift 8]  
LED_LATCH activa simultáneamente los registros de salida de U3 y U4.

Al escribir 16 bits **MSB first**:
- U3-QA recibe el bit 0 (último en entrar)
- U4-QH recibe el bit 15 (primero en entrar)

| Bit | Señal | Función |
|-----|-------|---------|
| 0 | C1_Red | LED1 rojo (canal 1) — R1 2.2kΩ — KT-0603R |
| 1 | C2_Red | LED3 rojo (canal 2) — R38 |
| 2 | C3_Red | LED4 rojo (canal 3) — R39 |
| 3 | C4_Red | LED5 rojo (canal 4) — R40 |
| 4 | C5_Red | LED6 rojo (canal 5) — R41 |
| 5 | C1_Green | LED7 verde (canal 1) — R42 — 19-219/GHC |
| 6 | C2_Green | LED8 verde (canal 2) — R43 |
| 7 | C3_Green | LED9 verde (canal 3) — R44 |
| 8 | C4_Green | LED10 verde (canal 4) — R45 |
| 9 | C5_Green | LED11 verde (canal 5) — R46 |
| 10 | MUX_S0 | CD74HC4067 select bit 0 (U5 y U6) |
| 11 | MUX_S1 | CD74HC4067 select bit 1 |
| 12 | MUX_S2 | CD74HC4067 select bit 2 |
| 13 | MUX_S3 | CD74HC4067 select bit 3 |
| 14 | MUX_**B**_EN# | Enable **U6** (CN2) — active LOW. ⚠️ El esquemático decía U5 |
| 15 | MUX_**A**_EN# | Enable **U5** (CN1) — active LOW. ⚠️ El esquemático decía U6 |

> ✅ Bits 0-9 verificados con `led_test` (2026-08-03).  
> ⚠️ **Bits 14-15 INTERCAMBIADOS respecto al esquemático — verificado 2026-09-21.**  
> Se detectó con `rtest` **sin arnés**: las 16 líneas leyeron V_CN1/V_CN2 al revés. Nunca se
> vio antes porque todas las mediciones previas llevaban el arnés de bucle, con el que ambos
> lados son el mismo nodo y el intercambio es invisible. `config.h` ya lo corrige. Solo afecta
> lecturas analógicas (`rtest`, `leaktest`, `scope_read`); `selftest` y `map` son digitales.

---

## 5. TCA9535 — Mapa de canales (CONFIRMADO)

### 5.1 Direcciones I2C

| IC | A2 | A1 | A0 | Dirección | Canales |
|----|----|----|----|-----------|---------|
| U1 | GND | GND | **3V3** | **0x21** | J00-J15 (CN1) |
| U2 | GND | GND | **GND** | **0x20** | J16-J30+J_SPARE (CN2) |

Pull-ups I2C: R36 y R37, 4.7kΩ a 3V3 (en la PCB tester).

> ⚠️ Verificar físicamente los pines A0 de U1 y U2 en la PCB antes de flashear.
> Si `scan_i2c` no detecta ambos, probar intercambiar 0x20/0x21 en config.h.

### 5.2 Mapeo Port → Línea Jxx (CONFIRMADO desde esquemático)

La numeración TCA9535 mapea directamente:

```
U1 (0x21) — CN1:
  Port 0: P00=J00, P01=J01, P02=J02, P03=J03, P04=J04, P05=J05, P06=J06, P07=J07
  Port 1: P10=J08, P11=J09, P12=J10, P13=J11, P14=J12, P15=J13, P16=J14, P17=J15

U2 (0x20) — CN2:
  Port 0: P00=J16, P01=J17, P02=J18, P03=J19, P04=J20, P05=J21, P06=J22, P07=J23
  Port 1: P10=J24, P11=J25, P12=J26, P13=J27, P14=J28, P15=J29, P16=J30, P17=J_SPARE
```

**En el firmware**, `readPort()` devuelve `(Port1 << 8) | Port0`:
- bit N del valor 16-bit = canal J(N) para U1, o canal J(N+16) para U2

---

## 6. CD74HC4067 — Scope Lite

### 6.1 Conexiones

| IC | Pin COMMON I/O | Canales I0-I15 | Select | Enable |
|----|---------------|----------------|--------|--------|
| U5 | ADC_MUX_OUT | J00-J15 | MUX_S0-S3 (shared) | MUX_A_EN# (bit 14) |
| U6 | ADC_MUX_OUT | J16-J30 (J_SPARE=I15) | MUX_S0-S3 (shared) | MUX_B_EN# (bit 15) |

**Ruta de señal:**  
`J(xx) → U5 o U6 → ADC_MUX_OUT → R47 (1kΩ) → ADC_SCOPE → C10 (22pF) → IO3 (ADC1_CH3)`

### 6.2 Operación

- Solo **uno** de U5/U6 debe estar activo a la vez (via MUX_A_EN# y MUX_B_EN#)
- Ambos EN# en HIGH = ambos deshabilitados (estado seguro de reposo)
- Los selects S0-S3 seleccionan el canal 0-15 dentro del MUX activo

### 6.3 Capacidades

- Medición de voltaje DC: 0-3.3V (sin escalar)
- Detección de señal AC: flancos, amplitud básica
- **Limitación:** ADC del ESP32-C3 tiene no-linealidad cerca de 0V y 3.3V. El R47+C10 actúa como filtro RC (τ≈22ns) — útil hasta ~7 MHz en teoría pero el ADC limita a ~1-10 kHz de muestreo práctico.

---

## 7. Conectores CN1 / CN2

| Conector | IC controlador |
|----------|---------------|
| CN1 (20p) | U1 (TCA9535, 0x21) + U5 (MUX) |
| CN2 (20p) | U2 (TCA9535, 0x20) + U6 (MUX) |

### 7.1 Pinout exacto (del esquemático Rev 1.0)

| Pin | CN1 | CN2 | | Pin | CN1 | CN2 |
|----|------|------|--|----|------|------|
| 1 | J00 | J16 | | 11 | J08 | J24 |
| 2 | J01 | J17 | | 12 | J09 | J25 |
| 3 | J02 | J18 | | 13 | J10 | J26 |
| 4 | J03 | J19 | | 14 | J11 | J27 |
| **5** | **GND** | **GND** | | **15** | **GND** | **GND** |
| 6 | J04 | J20 | | 16 | J12 | J28 |
| 7 | J05 | J21 | | 17 | J13 | J29 |
| 8 | J06 | J22 | | 18 | J14 | J30 |
| 9 | J07 | J23 | | 19 | J15 | **J_SPARE** |
| **10** | **GND** | **GND** | | **20** | **GND** | **GND** |

> ### ✅ El arnés de prueba es 1:1 — y eso es exactamente lo correcto
>
> Los 4 pines de GND caen en la **misma posición** (5, 10, 15, 20) en ambos conectores.
> Por lo tanto un arnés recto pin-N→pin-N:
> - une GND con GND (sin conflicto), y
> - produce el mapeo de señal `J(i) ↔ J(i+16)` para i = 0..15.
>
> Como `U1 bit i = J(i)` y `U2 bit i = J(i+16)`, el firmware puede manejar `U1[i]` y
> leer `U2[i]` — **mismo índice de bit en ambos lados**. No hace falta tabla de traducción.

---

## 8. Modos de operación

| Modo | Conectores | Canales activos | Descripción |
|------|-----------|-----------------|-------------|
| 16P | Solo CN1 | J00-J15 | 16 líneas, arneses pequeños |
| 20P | CN1 completo | J00-J15 + auxiliares | No implementado en Rev 1.0 |
| 32P | CN1 + CN2 | J00-J30 + J_SPARE | Capacidad máxima (32 líneas) |

**Auto-detección futura** (via J_SPARE o ADC): resistor ID en adaptador:
- 1kΩ → 16P, 4.7kΩ → 20P, 10kΩ → 32P

---

## 9. Self-Test (Single Board)

### Adaptador Loopback (cross-connect)

```
CN1 J00 ←→ CN2 J16    (U1 bit 0  ↔ U2 bit 0)
CN1 J01 ←→ CN2 J17    (U1 bit 1  ↔ U2 bit 1)
...
CN1 J14 ←→ CN2 J30    (U1 bit 14 ↔ U2 bit 14)
CN1 J15 ←→ CN2 J_SPARE (U1 bit 15 ↔ U2 bit 15)
```

**Por qué el índice coincide:** U1 bit N = J(N), U2 bit N = J(N+16). Al hacer cross-connect, drive U1[N] → leer U2[N].

### Secuencia de self-test

| # | Test | Comando | ICs validados |
|---|------|---------|--------------|
| 1 | LED Test | `led_test` | U3, U4 |
| 2 | Expander Walk | `expander_test` | U1, U2 |
| 3 | I2C Scan | `scan_i2c` | Bus I2C, U1, U2 |
| 4 | Continuity | `loopback16` | U1, U2, CN1, CN2, cables |
| 5 | Short detect | (incluido en loopback16) | Ídem |
| 6 | Scope Lite | `scope_read J05` | U5, U6, ADC |
| 7 | UART link | Fase 3 | LINK_TX/RX |

---

## 10. Comandos Serial (USB CDC @ 115200)

| Comando | Descripción |
|---------|-------------|
| `help` | Lista de comandos |
| `scan_i2c` | Escanear bus I2C |
| `led_test` | Walk visual 5 rojo + 5 verde (valida U3/U4) |
| `expander_test` | Walk de J00-J31 (valida U1/U2) |
| `all_input` | Poner U1 en input y leer estado de 32 canales |
| `drive J05 HIGH` | Drive J05 HIGH via U1 (J00-J15) |
| `drive J05 LOW` | Drive J05 LOW |
| `read_all` | Leer 32 canales con detalle bit a bit |
| `selftest` | **Continuidad en AMBOS sentidos + reporte consolidado por línea** |
| `rtest` | **Resistencia de contacto por línea (divisor + MUX/ADC)** |
| `loopback16` | Continuidad solo CN1→CN2 (legacy, subconjunto de `selftest`) |
| `scope_read J05` | Leer ADC_SCOPE canal J05 vía U5 |
| `scope_read J20` | Leer ADC_SCOPE canal J20 vía U6 |
| `sr_dump` | Debug: mostrar estado actual del shift register |
| `wifi_info` / `wifi_reset` | Estado de red / volver al portal |

Todos los comandos funcionan igual por **USB serial** y por **web** — `web_cmd.cpp` enruta
al mismo `handleCommand()`. Cualquier comando nuevo aparece automáticamente en ambos.

### 10.1 OTA — actualización de firmware por WiFi

Ruta `POST /update` colgada del servidor de `web_cmd.cpp` (botón *Firmware OTA* en la UI).
Sube el `.bin` generado en `tools/build/bring_up.ino.bin`.

> ⚠️ El OTA se cuelga del `WebServer` de `web_cmd`, **no** de uno nuevo. `wifi_portal.cpp`
> tiene su propia instancia en el puerto 80; no chocan porque son mutuamente excluyentes
> (AP portal vs STA conectado), pero nunca instanciar un tercer servidor en el 80.

---

## 11. Riesgos e inconsistencias

### Riesgos activos

| # | Riesgo | Impacto | Estado |
|---|--------|---------|--------|
| R1 | Mapa de bits 0-9 del shift register | LEDs en orden incorrecto | ✅ Cerrado 2026-08-03 — `led_test` correcto |
| R2 | Direcciones TCA9535 0x20/0x21 | I2C no responde | ✅ Cerrado 2026-08-03 — `scan_i2c` = 2 disp. |
| R3 | IO8 (SCL) = LED RGB en módulo ESP | Glitch en SCL | Mitigado — RGB solo antes de `Wire.begin()` |
| R4 | Corto entre dos líneas del **mismo** conector se reporta como ABIERTO | Diagnóstico equivocado | Documentado — ver §11.1 |
| R5 | Sketch usa 83% de la partición app (1.07/1.25 MB) | OTA deja de caber si crece | Vigilar; si sube, usar esquema `min_spiffs` |
| R6 | Solo 1 módulo ESP32 funcional (el 2° no arranca) | Sin repuesto | OTA reduce ciclos de conector USB |

### 11.1 Punto ciego de la detección de cortos

El test digital maneja la línea *i* en HIGH y **todas las demás en LOW activo**. Si dos líneas
están en corto dentro del cable, el driver pelea contra sí mismo:

```
3V3 ─[680Ω]─┬─ nodo en corto ─┬─[680Ω]─ GND
            └─ ~1.65 V ───────┘
```

1.65 V cae justo en la zona indeterminada del TCA9535, así que el lado receptor lee basura.
**El corto se detecta como falla, pero se reporta como ABIERTO.** Para distinguirlos usar
`rtest`, que mide niveles analógicos reales en lugar de umbrales digitales.

### Huecos resueltos vs pendientes

| # | Hueco | Estado |
|---|-------|--------|
| H1 | GPIO no documentados | ✅ Resuelto — esquemático confirmado |
| H2 | MUX_S0-S3 sin GPIO asignado | ✅ Resuelto — via shift register U4 |
| H3 | U5 vs U6 enable independiente | ✅ Resuelto — MUX_A_EN# y MUX_B_EN# en bits 14/15 |
| H4 | Pines CN1/CN2 auxiliares | ✅ Resuelto — GND en 5/10/15/20, resto señal |
| H5 | Mapa de bits SR sin confirmar visualmente | ✅ Resuelto 2026-08-03 — `led_test` visualmente correcto |
| H6 | Direcciones I2C exactas U1/U2 | ✅ Resuelto 2026-08-03 — 0x20 y 0x21 confirmados |
| H7 | ¿Los MUX U5/U6 tapean del lado del conector o del IC? | ✅ Resuelto 2026-08-03 — **lado del conector** (se lee el punto medio del divisor, no 3.3/0) |

### 11.2 Punto ciego de `rtest` — descubierto en el bring-up

`rtest` fuerza el extremo lejano como **salida en LOW** para armar el divisor. Un pin
del expander **pegado a GND** es eléctricamente idéntico a esa configuración, así que
`rtest` le da OK a una línea rota. Por eso existe `leaktest`, que sube la línea con el
extremo lejano en **alta impedancia**: una línea sana llega al riel (~3.3 V) y una con
fuga se queda abajo. **Correr siempre los dos juntos.**

### 11.3 Hallazgo de la primera unidad (2026-08-03)

PCB tester #1, arnés 1:1: **15/16 líneas PASS** en ambos sentidos, Rc ≈ 0 Ω.

`J04 ↔ J20` falla en ambos sentidos. Diagnóstico por medición:

| Medición | Resultado |
|---|---|
| `drive J05 HIGH` + `scope_read J05` (línea sana) | 3.300 V — llega al riel |
| `drive J04 HIGH` + `scope_read J04` | 1.733 V — algo la carga |
| Corregido por ganancia del ADC (÷1.0588) | 1.637 V |
| R equivalente a GND = V·680/(3.3−V) | **≈ 669 Ω ≈ 680 Ω** |

Una R de exactamente 680 Ω a tierra es la firma de **el pin P04 de U2 aterrizado**:
el resistor serie de la línea queda entre el conector y el pin, así que un pin en corto
a GND se ve desde J20 como 680 Ω.

#### Confirmado como DEFECTO DE DISEÑO (no de ensamble)

`leaktest` sobre **ambas** PCBs tester, mismo ESP32 y mismo arnés:

| Placa | CN1 sube (V) | CN2 sube (V) | R_fuga |
|---|---|---|---|
| #1 | 1.732 | 0.003 | 669 Ω |
| #2 | 1.728 | 0.003 | 666 Ω |

**La asimetría entre sentidos descarta el arnés** sin necesidad de test adicional:

- Un corto en el **arnés** estaría del lado del conector → daría ~0 V manejando desde
  cualquier extremo (simétrico). No es lo observado.
- Un corto en el **pin de U2**, detrás de su 680 Ω, produce exactamente lo medido:
  - CN2 sube → U2 no puede levantar el pin aterrizado → 0 V
  - CN1 sube → divisor 680/680 a través del cable → 1.65 V

Comportamiento **idéntico en las dos placas** ⇒ está en los archivos de fabricación,
no en el ensamble. U1 P04 está sano; solo U2 P04 falla, siendo el mismo componente
y el mismo índice de pin → sospecha de layout local a U2.

#### Causa raíz — CONFIRMADA en EasyEDA

**Vía huérfana de una revisión anterior.** Versiones previas del layout cruzaban señales de
la capa A a la B, lo que generó vías en esas líneas. Al rediseñar, el cruce desapareció pero
**la vía quedó**, ya sin net asignado. Al regenerar el plano de GND, EasyEDA no la reconoce
como señal (no tiene net) y el pour la absorbe → la vía queda pegada a tierra.

Por eso el corto está del lado IC del resistor de 680 Ω: es justo donde vivía el ruteo cruzado.

✅ **Corregido en Rev 1.1** — vía eliminada, la línea queda con la misma topología que las otras 31.

> ⚠️ **Riesgo residual a barrer antes de fabricar Rev 1.1:** ese cruce generó *varias* vías, no
> una. J04 se detectó solo porque su vía cayó sobre el pour. Otras vías huérfanas pueden estar
> **flotando** (invisibles a cualquier test eléctrico, pero quedan como stubs) o **cerca del pour
> sin tocarlo** (pasan hoy, fallan con humedad/temperatura/vibración).
>
> Verificar **todas** las vías sin net asignado, no solo la corregida. Las 32 líneas deben tener
> topología idéntica; cualquiera con una vía de más es sospechosa. Un DRC estándar NO detecta esto.

Impacto operativo Rev 1.0: **31 de 32 líneas usables**. J20 fuera de servicio.

#### Muestra mala conocida (golden-fail)

Conservar **una** PCB Rev 1.0 etiquetada como muestra de falla caracterizada:

| Parámetro | Valor esperado |
|---|---|
| Línea afectada | J04 ↔ J20 |
| `leaktest` CN1 sube | ~1.73 V → R ≈ 669 Ω |
| `leaktest` CN2 sube | ~0.003 V → R ≈ 0 Ω |
| `rtest` | OK (falso negativo — punto ciego conocido) |

Es el único caso de prueba **negativo** real del proyecto. Una placa buena solo demuestra
ausencia de falsos positivos; esta demuestra que la suite sigue detectando la falla.
Correrla tras cualquier cambio al firmware de pruebas.

---

## 12. Plan de Firmware por Fases

### Fase 0 — Bring-up ✅ COMPLETA
- [x] GPIO confirmados desde esquemático
- [x] Driver TCA9535 (read/write/direction)
- [x] Driver 74HC595 con control de MUX y LEDs
- [x] `scan_i2c` detecta U1 (0x21) y U2 (0x20) en PCB física
- [x] `led_test` muestra secuencia visual correcta
- [x] WiFi + portal captivo + UI web de comandos

### Fase 1 — Validación de arnés ← ACTUAL (v0.4)
- [x] `selftest` — barrido bidireccional CN1↔CN2 con reporte por línea
- [x] `rtest` — resistencia de contacto vía divisor 680/680 + MUX/ADC
- [x] OTA por web (`POST /update`)
- [ ] Ejecutar `selftest` con el arnés 1:1 → esperado 16/16
- [ ] Ejecutar `rtest` → valida H7 (dónde tapean los MUX) y da Rc por línea

### Fase 2 — Modos y reportes
- [ ] Mode 16P / 32P seleccionable
- [ ] Auto-detección de adaptador (ADC en J_SPARE)
- [ ] Reporte PASS/FAIL estructurado
- [ ] Resultado indicado en LEDs rojo/verde por canal

### Fase 3 — Protocol Layer
- [ ] UART inter-módulo: PING/PONG → LINK_OK
- [ ] Modo dual-board (Controller/Receiver)
- [ ] Estructura BLE (NimBLE) preparada

### Fase 4 — GUI / Control remoto
- [ ] WiFi: servidor web o WebSocket
- [ ] BLE activo

---

## 13. Bring-up Checklist

### Antes de flashear
- [ ] Confirmar A0 de U1 (debe ser 3V3 = addr 0x21)
- [ ] Confirmar A0 de U2 (debe ser GND = addr 0x20)
- [ ] Verificar que IO8 no tenga carga que tire SCL durante boot
- [ ] Verificar que IO9 esté en HIGH durante boot (pull-up 10kΩ en módulo ESP)

### Hardware check sin firmware
- [ ] 3.3V presente en VCC de U1, U2, U3, U4, U5, U6
- [ ] Resistores 680Ω visibles en líneas J00-J31
- [ ] Pull-ups R36/R37 (4.7kΩ) en SDA/SCL

### Flasheo: módulo nuevo vs actualización

| Caso | Qué escribir | Check «Conservar WiFi» |
|---|---|---|
| **Módulo recién ensamblado** | `merged.bin` @ `0x0` | **DESTILDADO** |
| Actualizar unidad ya funcionando | `bring_up.ino.bin` @ `0x10000` | Tildado |
| Cambió `partitions.csv` | `merged.bin` @ `0x0` | Destildado |

> ⚠️ **El fallo más confuso del proyecto.** Un módulo virgen no tiene bootloader (`0x0`) ni
> tabla de particiones (`0x8000`). Si le escribes solo la app en `0x10000`, **esptool reporta
> éxito** pero el chip nunca arranca: se enumera por USB y se desconecta (es la ROM, no tu
> firmware). ESPConnect lo delata con `No plausible partition table entry found`.
>
> El flasher ahora lo detecta: antes de un flasheo de solo-app lee 2 bytes en `0x8000` y
> comprueba el magic `AA 50`. Si falta, ofrece cambiar a flasheo completo.
> Botón directo: **🆕 Módulo nuevo (full flash)**.

### Con firmware v0.4 (arnés 1:1 conectado CN1↔CN2)
```
1. scan_i2c          → 0x20 y 0x21          [✅ verificado 2026-08-03]
2. led_test          → 5 rojos + 5 verdes    [✅ verificado 2026-08-03]
3. selftest          → esperado 16/16 líneas OK en ambos sentidos
4. rtest             → Rc de pocos ohms por línea; valida H7
```

---

## 14. Roadmap

```
v0.1 — Bring-up inicial (GPIO asumidos)
v0.2 — GPIO confirmados + MUX via shift register
v0.3 — WiFi + portal captivo + UI web de comandos
v0.4 — Selftest bidireccional + resistencia de contacto + OTA  ← ACTUAL
v0.5 — Modos 16P/32P + auto-detección adaptador
v0.6 — UART inter-módulo dual-board (requiere 2° ESP32)
v0.7 — BLE (NimBLE)
v1.0 — Release producción
```
