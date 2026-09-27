# Cambios pendientes de hardware — próxima revisión

Lista viva de cambios acordados pero **aún no fabricados**. Al abrir EasyEDA, trabajar desde aquí.

---

## Alcance confirmado — solo DOS cambios

| Ref | Componente | LCSC | Encapsulado | Estado |
|---|---|---|---|---|
| D2 | `USBLC6-2SC6` (ST) | **C7519** | SOT-23-6 | ⬅ **PEDIR** |
| R3 | `RC0603FR-071KL` (YAGEO) 1 kΩ ±1% | **C22548** | 0603 | ✅ en stock |

### Sin cambios (decidido)

| Ref | Componente | Motivo |
|---|---|---|
| LDO1 | `ME6211C33M5G-N` — SOT-23-5 (C82942) | Se queda. Sin problemas de ensamble |
| F1 | `0466.500NRHF` — 1206 (C151133) | Se queda. Cumplió su función |

> F1 es de acción **muy rápida** (serie 0466 Nano2 Very Fast-Acting), no retardada.
> Aguanta los picos de ~350 mA del TX WiFi sin abrirse.
> Conviene tener repuestos: es el único componente de sacrificio del módulo.

> ⚠️ R3 en stock es **0603**. Si la huella actual de R3 fuera 0402, hay que dibujarla como
> 0603 en la revisión nueva — y las placas ya fabricadas no se podrían retrofitear con
> ese stock.

---

## Módulo ESP32 SBC (Rev 1.1 → siguiente)

### 1. D2 — cambiar a encapsulado manejable a mano

| | Actual | Propuesto |
|---|---|---|
| Referencia | `USBLC6-2P6` | **`USBLC6-2SC6`** |
| Encapsulado | SOT-666 | **SOT-23-6** |
| Pitch | 0.5 mm | **0.95 mm** |
| Tamaño | 1.6 × 1.6 mm | 2.9 × 1.6 mm |

**Motivo:** el SOT-666 es inviable para pick-and-place manual. Terminaciones bajo el cuerpo,
imposibles de inspeccionar o retocar sin microscopio. El SOT-23-6 tiene patas laterales
visibles y casi el doble de pitch.

Mismo die de ST, mismas especificaciones eléctricas, misma función. Solo cambia la huella.

### ✅ Pinout VERIFICADO contra la hoja de datos de ST (REV.2, junio 2005)

ST documenta **ambos encapsulados en la misma hoja con un único diagrama funcional** —
el pinout es idéntico:

| Pin | Señal | Red en el esquemático |
|---|---|---|
| 1 | I/O1 | D+ |
| 2 | GND | GND |
| 3 | I/O2 | D− |
| 4 | I/O2 | IO18 (vía R8) |
| 5 | V_BUS | VBUS_USB |
| 6 | I/O1 | IO19 (vía R7) |

**El esquemático no se toca. Solo se cambia la huella.**

Área de PCB según ST: SOT-666 = 2.9 mm² · **SOT23-6L = 9 mm²** (más del triple).

Marcado para identificarlos a ojo: `-2SC6` → **`UL26`** · `-2P6` → **`F`**.

**No eliminar el TVS.** Protege el USB nativo del ESP32-C3, y el equipo va a enchufarse y
desenchufarse en planta. Cambiar el encapsulado quita el dolor de ensamble sin perder la protección.

### 2. R3 — dejar de sobreexcitar LED1

| | Actual | Propuesto |
|---|---|---|
| R3 | 50 Ω | **1 kΩ** |
| Corriente | ~26 mA | ~1.3 mA |

Cálculo con LED rojo `XL-1608SURC-06` (Vf ≈ 2.0 V): (3.3 − 2.0) / 50 = **26 mA**.
Un 0603 típico aguanta ~20 mA continuos → va por encima del máximo. Se degrada con el tiempo
y consume presupuesto de corriente para nada. Con 1 kΩ da 1.3 mA, de sobra visible.
Si se quiere más brillo, 470 Ω → 2.8 mA.

---

## Recomendaciones abiertas — NO confirmadas

Lo de abajo no está aprobado. Se deja anotado porque tiene coste real demostrado, pero la
decisión es del diseñador.

### 3. Conector polarizado / marca de pin 1

**Motivo:** un desfase de un pin al conectar cortocircuitó 3V3 a GND y abrió F1
(ver `ARCHITECTURE.md`). El fusible cumplió, pero es de un solo uso y el módulo se conecta
y desconecta constantemente durante el desarrollo.

Opciones: header polarizado, o serigrafía inequívoca de pin 1 en PCB **y** en el conector.

### 4. Considerar protección contra inversión en VCC_5V

Un Schottky en serie o un P-FET en la entrada limitaría el daño de una mala inserción.
Cuesta caída de tensión (Schottky) o área (P-FET). **Decisión pendiente** — el arreglo de
fondo es mecánico (punto 3); esto es defensa en profundidad.

---

## PCB Tester 32P (Rev 1.0 → 1.1)

### 5. Vía huérfana en U2 P04 — ✅ YA CORREGIDO en Rev 1.1

Vía sobrante de una revisión previa donde las señales cruzaban capa A↔B. Al quedar sin net
asignado, el pour de GND la absorbió → pin P04 de U2 aterrizado → línea J04↔J20 muerta.
Detalle completo en `ARCHITECTURE.md` §11.3.

### 6. Barrer TODAS las vías huérfanas — ⚠️ PENDIENTE

Aquel cruce generó **varias** vías, no una. J04 se detectó porque su vía cayó sobre el pour.
Las demás pueden estar flotando (invisibles a cualquier test eléctrico) o cerca del cobre sin
tocarlo (pasan hoy, fallan con humedad o vibración).

**El DRC estándar no detecta esto.** Verificar manualmente que las 32 líneas tengan topología
idéntica; cualquiera con una vía de más es sospechosa.

---

## Notas de ensamble (no son cambios de PCB)

- Llevamos 3 de 5 eventos por ensamble o manipulación: un módulo muerto de origen, uno con
  Guru Meditation intermitente durante el flasheo, y uno con F1 abierto por desfase de conector.
- Un módulo que falla el stub flasher de forma **intermitente** es sospechoso de alimentación
  marginal. Validar con `sysinfo`: si reporta `BROWNOUT`, hay soldadura fría o desacoplo pobre.
- Etiquetar físicamente cada unidad con los últimos 4 dígitos de su MAC — el firmware usa ese
  mismo identificador en el SSID del portal y en `link_info`.
