# Caracterización del tester — qué puede validar y qué no

**Fecha de las medidas:** 2026-09-25
**Firmware:** v1.3.0 · **Unidades:** 2898 (`192.168.1.90`) y 8744 (`192.168.1.96`)
**Montaje:** bucle 1:1 entre las dos placas, en las dos orientaciones posibles.

⚠️ Se nombran por **qué conecta con qué**, no como "recto" o "cruzado": esos
términos significan cosas opuestas segun si se habla del cable o del conector,
y ya generaron confusion una vez.

Este documento es la **base contra la que se juzga cualquier medida futura**. Si
mañana un umbral parece mal puesto, la respuesta está aquí y no en la memoria de
nadie.

---

## 1. Resumen: capacidades medidas

| Capacidad | Valor | Nota |
|---|---|---|
| Continuidad | 32/32 líneas, huella idéntica en 8 corridas | Sin falsos positivos ni negativos |
| Detección de abierto | Sí | Sale como canal aislado |
| Detección de corto a GND | Sí, **0–1 Ω** medidos | Requiere la medida analógica para clasificar |
| Detección de corto entre líneas | Sí | Net de 3+ canales |
| Corto en el **mismo conector** | Sí | No se asume ninguna topología |
| Resistencia de línea | 0,0–0,8 Ω sobre cable bueno | **Resolución práctica ≈ 1 Ω** |
| Fuga a GND | 3,300 V en línea sana | Detecta por debajo de ~600 kΩ |
| Tiempo | 11–14 s para 64 canales | ~22 s estimado para 96 |

**Lo que NO puede hacer:** no es un miliohmímetro. No distingue 0,5 Ω de 1,5 Ω.
Para validación de arneses sobra —un crimp flojo son decenas de ohms— pero no
sirve para caracterizar conductores.

---

## 2. Huellas de referencia

La huella md5 resume **nets y canales aislados**. Sirve para comprobar
repetibilidad de un vistazo.

| Estado | Huella | Resultado |
|---|---|---|
| `M.CN1↔L1.CN2` y `M.CN2↔L1.CN1` | `41fc2b7e` | 32 nets, 0 aislados |
| `M.CN1↔L1.CN1` y `M.CN2↔L1.CN2` | `f74b8910` | 32 nets, 0 aislados |
| Nada conectado | `930be821` | 0 nets, 64 aislados |
| Señal puenteada a GND | `5ea3a82d` | 31 nets, 2 aislados |
| Corto entre dos líneas | `89591b43` / `a8a2c82d` | 31 nets (uno de 4 canales) |

El mismo cable en dos orientaciones da **huellas distintas** (`41fc2b7e` y
`f74b8910`). Es lo correcto: el golden captura el cableado real, no una
plantilla genérica.

Tras provocar y quitar los dos defectos, el barrido volvió a `41fc2b7e` bit a
bit. Un defecto no deja rastro.

⚠️ **La huella tiene que incluir los aislados.** Si solo cubre los nets, un
barrido con 0 nets produce la huella de la cadena vacía, y comparar
repeticiones deja de comparar nada: informa "idénticas" aunque los canales
cambien. Pasó, y las primeras tandas de "8 de 8" no probaban lo que parecían.

---

## 3. El sesgo entre ADCs — la corrección menos obvia

### El problema

La resistencia de línea se mide con un divisor:

```
3V3 ─[680]─ A ─[cable]─ B ─[680]─ GND
```

Con cable ideal los dos extremos leen 1,65 V, y la **diferencia** entre A y B es
proporcional a la resistencia del cable. Al ser diferencial, el error de
ganancia del ADC (+5,9 % en este chip) se cancela solo.

**Pero eso solo vale si es el MISMO ADC.** Al medir entre dos unidades, cada
extremo se lee con el ADC de su placa, y la diferencia de ganancia entre los dos
chips se cuela como resistencia falsa.

### Lo medido

Con un arnés perfecto, midiendo en un solo sentido:

```
M.J00<->L1.J16   1.833 V   1.781 V   →  21.8 Ω
... las 32 líneas entre 21,3 y 22,6 Ω
```

Midiendo **al revés** las mismas líneas: **0,0 Ω**.

La confirmación directa, comparando pares dentro y fuera de una placa:

| Entre | Sesgo |
|---|---|
| `M.J05 – M.J07` (misma placa) | **0,0 Ω** |
| `L1.J21 – L1.J23` (misma placa) | **−0,2 Ω** |
| `M.J05 – L1.J21` (entre placas) | **22,9 Ω** |
| `M.J00 – M.J02` (misma placa, otro montaje) | **0,0 Ω** |
| `M.J00 – L1.J00` (entre placas, otro montaje) | **21,6 Ω** |

Cero dentro de una placa, ~22 Ω entre placas. No es resistencia: es calibración.

Verificado en **las dos orientaciones**: el sesgo no cambió al recablear — 21,2–22,4 Ω en uno, 21,4–22,9 Ω en el otro. Es
una propiedad del par de placas, no del cableado.

### La consecuencia si no se corrige

Con el umbral de "sospechosa" en 5 Ω, el tester habría reportado **22 Ω en todas
las líneas de un arnés perfecto** y rechazado el 100 % de las piezas buenas.

El `rtest` del firmware **nunca lo habría delatado**, porque mide CN1↔CN2 en una
sola placa y ahí el error se cancela. Solo aparece entre unidades — es decir,
justo en el arnés largo, que es el caso para el que se diseñó el sistema.

### La corrección

Medir en **los dos sentidos y promediar**: el sesgo aparece con signo opuesto en
cada dirección y se cancela. Implementado en `r_bidir()` de `tools/analog.py`.
El valor del sesgo se reporta en su propia columna.

> **El sesgo es también un indicador de salud.** Es constante para un par de
> placas dado (±0,6 Ω sobre 21,8). Si un día cambia mucho sin tocar nada, algo
> pasó en una de las unidades. Conviene que la app lo guarde por par.

---

## 4. Abierto contra corto a GND: el mapa no los distingue

Un canal pegado a GND **nunca sigue al driver**, así que el mapeo digital lo
reporta como **aislado** — exactamente igual que un cable roto.

Para el operador la diferencia lo es todo: un abierto es un crimp mal hecho, un
corto a GND es un pin corrido o una rebaba. Se arreglan de forma distinta.

Los distingue la medida analógica, excitando un extremo y dejando el otro en
alta impedancia:

| Lectura | Diagnóstico |
|---|---|
| Los dos ~3,3 V | Línea sana y unida |
| El excitado ~3,3 V, el otro ~0 V | **Cable abierto** |
| El excitado **baja** | **Pegado a GND** (R sale del divisor) |

Medido sobre el defecto provocado:

```
M.J02    0.001 V   →  PEGADO A GND (0 Ω)
L1.J18   0.003 V   →  PEGADO A GND (1 Ω)
M.J01    3.300 V   →  sana (control)
```

**Pendiente para la app:** cuando un canal que el golden espera salga aislado,
disparar automáticamente esta medida sobre ese canal y reportar el *tipo* de
fallo. Es una medida suelta, no un barrido — barato, y convierte "J02 falla" en
"J02 pegado a GND".

---

## 5. Doble muestra: por qué hacen falta dos lecturas

Con todo desconectado aparecía un net fantasma en ~2 de cada 8 barridos, siempre
en canales al aire.

Un pin realmente conectado **sigue al driver siempre**; uno al aire se mueve por
acoplamiento y **no repite**. Se toman dos muestras por fase y solo valen los
bits en que coinciden (AND para la fase alta, OR para la baja). Con eso: 10 de 10
barridos idénticos.

Las dos muestras viajan en **una sola petición**, así que no cuesta tiempo de
red.

> Antes se atribuyó esta inestabilidad a que la 8744 tenía canales sin poblar y
> se limitó a 16 canales. **Era falso** — todos los módulos llevan los dos pares
> de expansores. `ch_count` sigue siendo útil para un SKU que solo use 16
> posiciones, pero **no era el arreglo**: el arreglo es la doble muestra.

---

## 6. El barrido no asume ninguna topología

Excita **un** canal y escucha en **todos** los demás, en todas las unidades,
incluida la suya. La única exclusión es el canal excitado.

Por eso detecta sin distinción:

- `CN1 ↔ CN2` en la misma placa
- **`CN1 ↔ CN1` en la misma placa** (verificado con un puente real)
- Entre unidades, cualquier conector
- Un canal unido a 3 o más
- Arneses impares, sin parejas

El tiempo escala con **canales × unidades**: ~380 peticiones para 64 canales en
2 unidades (11 s), ~770 para 96 en 3 (~22 s).

---

## 7. Umbrales y de dónde salen

| Umbral | Valor | Origen |
|---|---|---|
| `RC_OK` | 5 Ω | Muy por encima del 0,0–0,8 Ω medido en cable bueno |
| `RC_WARN` | 50 Ω | Un crimp flojo está en decenas de ohms |
| `LEAK_OK_V` | 3,00 V | Una línea sana llegó a 3,300 V en las 32 |
| Resolución | ~1 Ω | Dispersión línea a línea sobre arnés bueno |

---

## 8. Herramientas

| Herramienta | Para qué |
|---|---|
| `tools/sweep.py` | Barrido de mapeo, golden y comparación. Ver §9 |
| `tools/analog.py` | Resistencia y fuga por línea, bidireccional |
| `tools/ct.py` | Bridge de comandos y OTA |
| `rtest` / `leaktest` (firmware) | **Solo una placa**, bucle CN1↔CN2 local |

⚠️ `rtest` y `leaktest` del firmware **no valen entre unidades**: asumen que los
dos extremos están en la misma placa. Para un arnés que cruza unidades hay que
usar `analog.py`, que además corrige el sesgo entre ADCs.


---

## 9. Golden y veredicto

El golden **vive en la PC**, no en el ESP32. Si una unidad se borra o se cambia,
el golden sigue aquí; se monta otra, se le dice qué job correr, y listo.

```bash
# Capturar, con una pieza buena aprobada por ingeniería
python tools/sweep.py 192.168.1.90 192.168.1.96 --save-golden goldens/SKU1234.json

# Validar cada pieza
python tools/sweep.py 192.168.1.90 192.168.1.96 --check goldens/SKU1234.json
```

**Código de salida: 0 si pasa, 1 si rechaza.** La app lo usa directamente, sin
parsear texto.

El golden guarda las **unidades y su orden**, porque el orden define los nombres
de canal: el mismo fichero con las unidades al revés no significa lo mismo. Si
el montaje no coincide en número de canales, se rechaza antes de medir.

### Se comparan conexiones, no nets

Si se compararan nets, un solo corto renumeraría todos los siguientes y saldrían
decenas de fallos por un único defecto.

### Los cortos se agrupan por nodo

Un puente físico une dos parejas y crea **cuatro** parejas nuevas. Sin agrupar,
el operador ve cuatro fallos y tiene que deducir que son uno:

```
CORTO  L1.J00 <-> L1.J02        }
CORTO  L1.J00 <-> M.J02         }  el mismo puente,
CORTO  L1.J02 <-> M.J00         }  cuatro veces
CORTO  M.J00 <-> M.J02          }
```

Agrupado queda en lo que hay que ir a mirar:

```
CORTO  unidos y NO deberian: L1.J00 L1.J02 M.J00 M.J02
```

Verificado con un puente físico real: **un puente → un fallo**, y la medida
analógica lo confirma en **0,0–0,2 Ω** (corto duro, no fuga resistiva).


### Estabilidad a largo plazo

La orientación `M.CN1↔L1.CN2` se midió al principio de la sesión y otra vez al
final, con **tres recableados y dos defectos provocados de por medio**. Las dos
veces: `41fc2b7e`, bit a bit.

Un defecto no deja rastro, y el equipo no "aprende" mal ni arrastra el error a
la pieza siguiente.


---

## 10. Validar módulos antes del golden — paso BLOQUEANTE

```bash
python tools/sweep.py <ip1> <ip2> <ip3> --modulos    # arnés DESCONECTADO
```

Código de salida 0 si todos los canales están sanos, 1 si alguno está pegado a
masa.

**Por qué es bloqueante y no un aviso:** en el mapa, un canal pegado a GND y un
cable roto se ven **exactamente igual** — los dos salen como "aislado". Si se
captura un golden con una placa defectuosa, ese canal queda congelado como
"sin conexión" y el tester **deja de vigilar esa línea para siempre**.

### Caso real medido (2026-09-25)

Con el arnés puesto salían 12 canales aislados. Al quitarlo, la causa se partió
en dos grupos con arreglos distintos:

| Origen | Canales | Prueba |
|---|---|---|
| **Placa 700C** | `J00`, `J04`, `J12` | Siguen a 0 Ω sin arnés |
| **Arnés / fixture** | `M.J05`, `M.J09`, `L2.J07`, `L2.J09` | Quedan libres al quitarlo |

Las unidades 2898 y 424C: 64 de 64 canales sanos.

El patrón `J00, J04, J12` en una sola placa es demasiado regular para ser
casualidad, y coincide con el mecanismo ya documentado en
[REV_NEXT.md](REV_NEXT.md): vías huérfanas de una revisión anterior que EasyEDA
absorbe en el plano de GND al regenerarlo, porque al no estar conectadas a nada
no las reconoce como señal.

**Sin este paso, los dos defectos habrían entrado en el golden como estado
normal.**


---

## 11. Los MUX CD74HC4067 — validados contra esquemático (2026-09-26)

Último IC que quedaba por cotejar. **Coincide: 32 de 32 canales.**

### Lo que dice el esquemático

| | U5 (`MUX_A_EN#`) | U6 (`MUX_B_EN#`) |
|---|---|---|
| `I0`…`I15` | `J00`…`J15` (CN1) | `J16`…`J30` + `J_SPARE` |
| Común (pin 1) | `ADC_MUX_OUT` | `ADC_MUX_OUT` — **el mismo nodo** |
| `S0`-`S3` | `MUX_S0`-`S3` | `MUX_S0`-`S3` — **compartidos** |

El índice del MUX **es** el índice del canal: `In → J(n)` en U5 y `In → J(16+n)`
en U6, sin desplazamientos. `I15` de U6 es `J_SPARE`.

Como los dos comparten la salida común y los bits de selección, **la única cosa
que distingue de qué conector se lee son los enables**. Por eso equivocarlos no
da error: da lecturas del conector contrario, que parecen perfectamente
normales.

### La primera prueba no probaba nada

Se excitó `J00` y se leyó el índice 0 por los dos MUX: **1785 mV y 1784 mV**.
Iguales. Parecía preocupante y no significaba nada — el arnés estaba puesto y
une `J00` con `J16`, así que los dos MUX miraban **el mismo nodo**. Con el
arnés conectado, esa comparación no puede distinguir el caso correcto del
cambiado.

### La que sí distingue

Excitar **un** canal con **todo lo demás forzado a 0 V** —nada flotante— y
barrer los 32 índices por los dos MUX. El nodo alto tiene que aparecer en su
`(MUX, índice)` y en ningún otro:

```
J01 excitado:  adc_rd 0 1 = 3300 mV
               los otros 31 = 3-4 mV
```

Repetido para los 32 canales: **cada uno se ve exactamente donde toca**,
`J_SPARE` incluido. Si los enables estuvieran cambiados, cada canal aparecería
en el MUX contrario; si los bits `S0`-`S3` estuvieran desordenados, aparecería
en otro índice.

> ⚠️ **No usar `io_rd` para esto.** Con las entradas en alta impedancia el
> primer intento dio parejas fantasma (`J10 → bits 1,3,4,6,10,11,12`): es el
> acoplamiento de §5, el mismo que obligó a la doble muestra. Con el ADC y todo
> excitado la lectura es determinista.

### De regalo: un hilo abierto

El barrido destapó que el arnés rojo tenía **6 parejas de las 7** del golden de
`MPG-IN1`. Falta `J02↔J18`:

| Línea | Excitado | Otro extremo | |
|---|---|---|---|
| `J00↔J16` | 1782 mV | 1781 mV | unidas (divisor) |
| `J05↔J21` | 1786 mV | 1785 mV | unidas (divisor) |
| **`J02↔J18`** | **3300 mV** | **3 mV** | **abierta** |

---

## 12. El osciloscopio lento — techo medido y qué sirve

### El techo, medido

| | |
|---|---|
| Petición mínima (ida y vuelta de red) | 40,8 ms |
| 44 `adc_rd` en una petición | 176,8 ms |
| **Coste por muestra** | **3,09 ms** → ~324 muestras/s |
| Conmutar de MUX entre muestras | sin coste apreciable |

El límite no es la red: es que cada `adc_rd` promedia **64 lecturas** del ADC.

### Lo que esto descarta

**No hay diagrama de ojo, ni tiempo de subida, ni jitter.** Eso exige muestrear
a GHz. El RC de un arnés con 680 Ω en serie son decenas de **nanosegundos**: a
3 ms de resolución el flanco ya ocurrió antes de la primera muestra, siempre.
Pedirle integridad de señal a este equipo es pedirle cinco órdenes de magnitud
que no tiene.

### Lo que sí, y es lo que vale

El dato no es el flanco: es el **nivel** y su **estabilidad**. Medido sobre el
arnés rojo de la 8744, excitando un extremo y escuchando el otro en alta
impedancia:

| Línea | Nivel alto | Desviación | |
|---|---|---|---|
| `J00↔J16` | 3300 mV | **0,0 mV** | unida |
| `J05↔J21` | 3300 mV | **0,0 mV** | unida |
| `J02↔J18` | 1842 mV | **335 mV** | **al aire** |

Un pin conectado sigue al excitador y **no se mueve**. Uno al aire vaga por
acoplamiento. La desviación típica separa los dos casos mejor que el valor
medio — y es el mismo fenómeno de §5, el que obligó a la doble muestra.

De ahí sale algo que el PASS/FAIL no ve: **un contacto intermitente**. Sacudiendo
el arnés mientras se muestrea, un crimp flojo aparece en la traza aunque una
prueba suelta lo apruebe.

### El acoplamiento, a la vista

Excitando `M.J00` y leyendo todos los demás canales al aire:

```
M.J16   3300 mV   <- conectado de verdad por el arnes
M.J01   2417 mV   <- NO conectado: acoplamiento
M.J17   2391 mV   <- NO conectado: acoplamiento
M.J02   1673 mV
...
M.J15    765 mV
```

Dos canales al aire a **2,4 V**. Si el barrido se fiara de una sola muestra, los
daría por conectados. Esta pantalla es la demostración visual de por qué el
mapeo exige que **dos muestras coincidan**.

### Límite duro de la API

**48 ops por petición** (`RAW_MAX_OPS`). Pasarse no da error: el firmware
**trunca en silencio** y faltan muestras. El primer prototipo pidió 54 y se
quedó sin datos sin decir nada. El código calcula cuántas muestras caben.

---

## 13. La etiqueta en blanco — un bug de media bobina

La plantilla `standard` por defecto ponía el QR en `y=40` con 40 pt de alto
—acaba en **80**— y una línea de texto en `y=72`, sobre una etiqueta de **72 pt**.

pdfkit **añade una página** cuando algo cae por debajo del borde inferior. En
pantalla eso es una hoja en blanco y no se nota; en una impresora de rollo es
**una etiqueta en blanco por cada etiqueta buena**: media bobina tirada y un
adhesivo vacío que el operador descarta a mano. Además, el QR salía cortado.

Medido: una etiqueta standard → **2 páginas**. Tras el arreglo → **1**.

El arreglo va en el motor, no solo en la plantilla, porque las plantillas ya
guardadas en cada SKU llevan el mismo defecto copiado. El texto se mide por
**altura de línea** preguntándosela a pdfkit, no por el cuerpo de la fuente: una
estimación corta vuelve a paginar. Cada ajuste deja un aviso en el log.

> Se ajusta en vez de omitir: una etiqueta a la que le falta el QR se ve; una
> página en blanco de más pasa desapercibida hasta que se acaba el rollo.

---

## 14. El recorrido de socket y el fantasma, otra vez

**Medido en planta (2026-09-28)**, recorriendo el socket del arnés de 45 líneas:

| Conector | Esperados | Detectados |
|---|---|---|
| CN12 | 8 | **18** |
| CN14 | 10 | **14** |
| CN9 | 8 | **11** |
| CN11 | 8 | **0** |
| CN10 | 8 | 4 |

Tocando un pin se encendían hasta **seis canales a la vez**. Los tres síntomas
—sobran, faltan, y uno que no detecta nada— son **la misma causa**: el
acoplamiento del §5, y un detalle del diseño que lo amplifica.

### Por qué se comía conectores enteros

Un canal detectado queda marcado como *visto* para no repetirlo. Cuando un
fantasma se anota, **gasta el canal**: el pin de verdad ya no se puede
registrar. Por eso CN11 dio **0 de 8** — sus ocho canales ya estaban gastados
por fantasmas de CN12 y CN14.

### Por qué la doble muestra NO bastaba aquí

El recorrido escuchaba con **una sola muestra**, mientras el barrido usa dos
(`sample2`). Eso ya era un error. Pero con la punta **sostenida** la doble
muestra tampoco alcanza: un vecino acoplado se carga y se lee alto en las dos.

### El filtro que sí distingue: la tensión

Del barrido de acoplamiento del §12, con un canal excitado:

```
M.J16   3300 mV   <- conectado de verdad
M.J01   2417 mV   <- acoplamiento
M.J17   2391 mV   <- acoplamiento
```

**Un pin con la punta encima está a 3,3 V; un fantasma se queda por debajo.**
Ahora cada candidato se confirma leyendo su ADC y se exige **≥ 2900 mV**, que
deja margen a los dos lados. Cuesta una lectura por candidato, y solo se paga
cuando hay candidato.

Esto funciona porque `sample` con `drive_ch = -1` deja **todos los canales como
entradas**: la punta sostiene el nodo y el ADC lo ve tal cual.

Los descartes se muestran en pantalla con su tensión. Un filtro silencioso
parecería que la app no ve nada.

---

## 15. LEDs — el mapa de bits estaba invertido

**Corregido en firmware v1.4.0 (2026-09-26).**

`config.h` decía *"bits 0-4 los cinco rojos, bits 5-9 los cinco verdes"*. El
esquemático dice otra cosa: van **intercalados verde/rojo por canal**, y el
canal 5 vive en el **segundo** 74HC595.

| bit | real (U3/U4) | lo que decía config.h |
|---|---|---|
| 0 | `C1_Green` (U3 QA) | C1_Red |
| 1 | `C1_Red` (U3 QB) | C2_Red |
| 2 | `C2_Green` (U3 QC) | C3_Red |
| 3 | `C2_Red` (U3 QD) | C4_Red |
| … | … | … |
| 8 | `C5_Green` (U4 QA) | C4_Green |
| 9 | `C5_Red` (U4 QB) | C5_Green |

Pedir "todos los rojos" encendía `C1_Green, C1_Red, C2_Green, C2_Red, C3_Green`
— una mezcla, que es exactamente lo que se observó en planta.

### Por qué la verificación anterior no sirvió

El comentario decía *"VERIFICADO visualmente con led_test"*. Encendiéndolos
**uno a uno en secuencia** solo se comprueba que todos funcionan, no en qué
orden están. Para verificar un mapa de bits hay que encender **un patrón que
distinga**: cinco del mismo color, o una zona aislada.

La verificación que sí valió (v1.4.0): cinco verdes → cinco rojos → **una zona
roja con las otras cuatro verdes**. El tercer caso es el decisivo.

### Los bits de MUX sí eran correctos

Se confirma solo: las medidas analógicas dependen de ellos y salieron bien todo
el día. El error estaba **solo** en los LEDs.

### Regla en producción

- **Pasa** → los cinco verdes.
- **Falla** → se pone **roja la zona** que contiene cada canal en fallo; el
  resto queda verde.

Los 32 canales se reparten en 5 zonas: **7, 7, 6, 6, 6**.

### La primitiva `leds`, no `sr_wr`

Para encender LEDs desde la PC hay una op dedicada que **preserva la selección
de MUX**. Escribir el registro de 16 bits entero con `sr_wr` la borraría, y las
medidas analógicas pasarían a leer el canal equivocado **sin dar ningún error**.
