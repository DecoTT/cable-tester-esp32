# API del tester — contrato con el software de PC

**Versión de contrato:** `api = 1`  
**Base:** `http://<ip-del-master>/`

---

## 1. Regla fundamental

> **El software de PC NUNCA debe parsear la salida de `/cmd`.**

`/cmd` devuelve prosa para humanos (`[LB] J04 -> J20 : ABIERTO`). Esos textos cambian
cuando se mejora un reporte o se corrige una traducción. Un programa que los lea con
expresiones regulares se rompe con cada cambio de firmware — justo lo contrario de poder
desarrollar las dos partes por separado.

**Todo lo que consuma un programa va por `/api/*`, que es estable y versionado.**

### 1.1 Compatibilidad

Cada respuesta lleva `"api": N`. El software de PC debe comprobarlo al conectar:

- **Se añaden campos** → `api` NO cambia. El cliente debe ignorar los que no conozca.
- **Cambio incompatible** (se elimina o cambia el significado de un campo) → `api` sube.
  El cliente debe rechazar la conexión y avisar al operador de que hay que actualizar.

---

## 1.2 Dos transportes, un solo contrato

El mismo JSON está disponible por **HTTP** y por **USB serial**. La app de PC elige según el
despliegue; el protocolo no cambia.

| Escenario | Transporte | Cómo |
|---|---|---|
| PC y tester en la misma red | HTTP | `GET /api/status` |
| PC por cable, sin WiFi compartido, o sin acceso a la red | **USB serial** | `api_status` por el COM a 115200 |
| PC con WiFi pero sin red común | HTTP contra el AP del tester | El tester levanta `CableTester32-XXXX` |

| HTTP | Serial (una línea) |
|---|---|
| `GET /api/status` | `api_status` |
| `GET /api/golden` | `api_golden` |
| `POST /api/golden` (cuerpo multilínea) | `api_golden_set sku=X;job=Y;meta=Z;nets=0,1,1,...` |

Por serial, los campos van separados por `;` en una sola línea. La respuesta JSON es idéntica.

### ¿Por qué no Bluetooth?

Se evaluó y se descartó por ahora:

- El ESP32-C3 solo tiene **BLE**, no Bluetooth clásico. No existe SPP, así que **no aparece un
  puerto COM virtual** en la PC: la app tendría que hablar GATT (descubrir servicios, notificaciones,
  paquetes de 20-250 bytes, reconexiones). En Windows eso es más complejo y menos estable que
  serial o HTTP — no simplifica, mueve la complejidad a la PC.
- NimBLE cuesta ~100-150 KB estando al 87% de la partición. Sin margen para OTA ni para el
  mapeo distribuido.
- BLE comparte la radio con WiFi y ESP-NOW en un solo núcleo: riesgo directo al enlace
  master↔hijos.

**USB serial cubre el escenario "PC sin WiFi" con cero firmware y es el transporte más robusto.**

BLE tendría sentido para una app de **teléfono o tablet**. Si eso entra al roadmap, requiere
cambiar el esquema de particiones a `min_spiffs` (1.9 MB por slot) y reflashear todas las
unidades con tabla nueva.

---

## 2. `GET /api/status` — implementado

Responde el estado completo del equipo. Es lo que permite comprobar antes de una prueba
si el tester está en línea y correctamente configurado para el SKU.

```json
{
  "api": 1,
  "fw": "0.6",
  "unit": "2898",
  "mac": "1C:DB:D4:F8:28:98",
  "role": "MASTER",
  "channels_local": 32,
  "peers": [
    {"mac":"E8:F6:0A:2A:70:0C","unit":"700C","online":true,"rssi":-15},
    {"mac":"1C:DB:D4:F7:42:4C","unit":"424C","online":true,"rssi":-19}
  ],
  "peer_count": 2,
  "channels_total": 96,
  "golden": {"loaded":true,"name":"arnes-golden","nets":18},
  "expanders": {"u1":true,"u2":true},
  "wifi": {"connected":true,"ssid":"FTP-6e_2.4","ip":"192.168.0.117","rssi":-52},
  "uptime_s": 1234,
  "reset_reason": "POWERON (normal)",
  "heap_free": 178000
}
```

**Verificación previa a la prueba, del lado PC:**

| Comprobación | Campo |
|---|---|
| ¿Es el equipo correcto? | `unit` / `mac` |
| ¿Tiene los hijos que exige el SKU? | `peer_count`, `peers[].online` |
| ¿Calidad de radio suficiente? | `peers[].rssi` (rechazar por debajo de −85) |
| ¿El hardware responde? | `expanders.u1` y `.u2` |
| ¿Se ha reiniciado solo? | `reset_reason` — un `BROWNOUT` invalida la sesión |

---

## 3. Endpoints previstos — NO implementados todavía

Se documentan para que el software de PC pueda escribirse contra el contrato mientras
el firmware avanza.

### `POST /api/golden` — cargar el job del SKU

La PC es la **fuente de verdad** de la biblioteca de jobs. Antes de cada corrida empuja
el golden del SKU; el tester solo guarda el último como respaldo para modo autónomo.

```json
{ "job":"SKU4589", "channels":96,
  "nets":[["M.J00","L1.J19"],["M.J01","L1.J27","L2.J03"]] }
```

### `POST /api/test` — ✅ IMPLEMENTADO (firmware v0.7)

Ejecuta la corrida completa: mapea (local o distribuido según los hijos enlazados),
compara contra el golden cargado, enciende los LEDs de cada unidad y devuelve el
resultado estructurado. También por serial: `api_test`.

```json
{
  "api": 1,
  "result": "FAIL",
  "job": "SKU4589",
  "channels": 96,
  "duration_ms": 1420,
  "faults": [
    {"type":"open",  "a":"M.J04", "b":"L1.J20"},
    {"type":"short", "a":"M.J07", "b":"M.J08"}
  ]
}
```

`result` ∈ `PASS` · `FAIL` · `ERROR`. `type` ∈ `open` · `short`.

Respuesta real del firmware:

```json
{
  "api": 1, "sku": "SKU4589", "job": "arnes-43p-rev2", "crc": 20190,
  "channels": 96, "units": 3,
  "faults": [{"type":"open","a":"M.J04","b":"L1.J20"}],
  "result": "FAIL", "duration_ms": 3120
}
```

Con `result: "ERROR"` hay un campo `error` con el motivo: sin golden, golden corrupto
(CRC no cuadra), fallo de I²C, alguna unidad sin responder, o **desajuste de canales**
(el golden es de 96 y la configuración actual tiene 64 → falta un hijo enlazado).

**Nombres de canal:** `M.J00`..`M.J_SP` el master, `L1.*` y `L2.*` los hijos, en el orden
de `peers[]`. La PC los traduce a posición de conector con su tabla de fixture.

**Duración medida:** ~3 s para 96 canales en 3 unidades; ~0.3 s para 32 en una sola.

### `GET /api/test/last` — ✅ IMPLEMENTADO

La última prueba, **venga de donde venga**. Es lo que permite que el switch físico no
rompa la trazabilidad: la app sondea `last_test.seq` en `/api/status` y, cuando cambia,
recoge aquí el detalle completo. Misma forma que `/api/test`, más `"seq"`.

Por serial: `api_last`.

### `POST /api/leds` — LEDs bajo control de la app

Máscara de 16 bits, una por unidad:

| Bits | Significado |
|---|---|
| 0-4 | Rojo 1-5 |
| 5-9 | Verde 1-5 |

| Máscara | Efecto |
|---|---|
| `992` | 5 verdes — PASS |
| `31` | 5 rojos — FAIL |
| `961` | Rango 1 en rojo, resto verde |
| `0` | Apagados |

Los rangos cubren los 32 canales de **esa** unidad: `J00-J06`, `J07-J13`, `J14-J19`,
`J20-J25`, `J26-J31`.

Por serial: `leds <unidad> <mask>` · `leds off` · `led_mode auto|manual`.

**`led_mode`** decide quién manda tras una prueba:
- `auto` (por defecto) — el firmware pinta el veredicto por rango en cada unidad
- `manual` — el firmware no toca los LEDs; los controla solo la app

### Disparador físico — ✅ IMPLEMENTADO

Un switch en un GPIO equivale al botón de prueba de la app. En producción el operador
conecta el arnés y pulsa sin volver a la PC. Queda registrado igual que un clic: la prueba
incrementa `last_test.seq` y la app la recoge.

```
trigger <pin> [low|high]     fijar el switch (activo BAJO por defecto, con pull-up interno)
trigger off                  desactivarlo
trigger                      ver el estado actual
```

**Pines disponibles:** `4`, `5`, `6`, `7` (libres en el header) y `10` (ya trae botón físico,
`BTN_START`). Ocupados y rechazados: 0-3 (shift register y ADC), 8-9 (I²C), 18-19 (USB).

> Si se asigna el `10`, deja de parpadear la IP y pasa a disparar la prueba.

En `/api/status` aparece como `"trigger":{"pin":10,"active_low":true}` para que la app
sepa si hay switch y en qué pin.

### `GET /api/map` — mapear sin comparar

Para capturar un golden nuevo desde la PC. Misma forma que `/api/golden`.

---

## 4. Reparto de responsabilidades

Esto es lo que hace posible separar los dos desarrollos: cada cosa vive donde le toca.

| Función | Dónde | Por qué |
|---|---|---|
| Detección eléctrica de nets | **Firmware** | Es lo único que necesita el hardware |
| Comparación contra golden | **Firmware** | Va con la detección; el resultado sale ya estructurado |
| LEDs por rango | **Firmware** | Necesita respuesta inmediata en el equipo |
| Biblioteca de jobs por SKU | **PC** | Cambia a menudo; no cabe ni conviene en NVS |
| Nombres de conector (`JST3 pos 5`) | **PC** | Es del *fixture*, no del circuito — ver §5 |
| Diagrama del arnés con la falla | **PC** | Interfaz gráfica |
| Operador, fecha y hora | **PC** | El tester **no tiene RTC**: no puede ser autoridad de tiempo |
| Registro y trazabilidad | **PC** | |
| Conteo de empaque y etiqueta | **PC** | El *standard pack* es por SKU, no por equipo |

### 4.1 El tester no sabe la hora

No hay RTC ni batería. `uptime_s` cuenta desde el arranque y nada más. **Todos los sellos
de tiempo del registro los pone la PC.** No confiar en el equipo para trazabilidad temporal.

---

## 5. Traducción canal → posición de conector

El firmware trabaja en canales eléctricos: `M.J19`, `L1.J04`. El operador necesita
`JST3 pos 5`.

Esa traducción **pertenece al fixture**, no al circuito: la misma placa con otra PCB de
derivación da otros nombres. Por eso vive en la PC, como parte de la definición del SKU:

```json
{ "fixture":"pcb-breakout-v1",
  "labels": { "M.J19":"JST3 pos 5", "L1.J04":"JST1 pos 12" } }
```

Consecuencia importante: **cada fixture necesita su propio golden y su propia tabla de
etiquetas.** El arnés soldado a contras y el de PCB dan mapas distintos; compararlos
cruzados produciría fallos falsos en masa.

---

## 5.1 Modelo de seguridad — qué protege y qué no

### Falla del lado seguro

Tester equivocado, fixture equivocado o golden de otro SKU ⇒ **RECHAZO**, nunca aprobación
falsa. Cualquier diferencia contra el golden —de más o de menos— aparece como `open` o
`short`, porque se comparan particiones completas. El peor caso es un rechazo injusto.

### El único camino a una aprobación falsa: un golden malo

Si el golden se captura de una muestra defectuosa, a partir de ahí **los arneses buenos
fallan y los que traen ese mismo defecto pasan**. El tester no puede detectarlo solo.

Guardas que debe implementar el software de PC:

| Guarda | Qué evita |
|---|---|
| **Conteo esperado de conexiones por SKU** (del plano, no del tester) | Aceptar un golden con conexiones de menos o de más |
| **Procedencia**: quién, cuándo, qué fixture, qué número de parte | Trazar qué lote se validó contra qué referencia |
| **Doble captura**: dos muestras buenas deben dar el mismo mapa | Que la muestra de referencia fuera la rara |

### Lo que NO es verificable electrónicamente hoy

Las MAC identifican la electrónica. **El fixture es pasivo: el tester no sabe cuál tiene
puesto.** Depende de que el operador conecte el correcto.

**Propuesta para la PCB de derivación** (aún sin diseñar): resistor de identificación a GND
en un canal dedicado, leído por el MUX + ADC. Con los 680 Ω en serie:

| R del fixture | Lectura |
|---|---|
| 1 kΩ | 1.96 V |
| 2.2 kΩ | 2.52 V |
| 4.7 kΩ | 2.88 V |
| 10 kΩ | 3.09 V |

Por encima de 10 kΩ los valores se apelotonan cerca del riel: ~5-6 códigos fiables con un
canal, 25-36 con dos. Convierte «el operador puso el fixture correcto» de supuesto a dato.

---

## 6. Nomenclatura de canales

```
M.J00 .. M.J30, M.J_SP     master        (32 canales)
L1.J00 .. L1.J_SP          primer hijo   (32)
L2.J00 .. L2.J_SP          segundo hijo  (32)
```

El índice de hijo sigue el orden de `peers[]` en `/api/status`. **Con un golden cargado,
cambiar el orden de los peers invalidaría el mapa** — por eso el software de PC debe
verificar las MAC contra las que registró al capturar el golden, no fiarse de la posición.
