# Estado del proyecto — 2026-09-26

Punto de partida para retomar sin releer el historial. Lo que **no** está aquí,
está en [docs/CARACTERIZACION.md](docs/CARACTERIZACION.md), que es el documento
de referencia de qué puede medir el tester y por qué los umbrales están donde
están.

---

## 1. Arquitectura actual — la PC orquesta

**La PC habla con CADA unidad por HTTP.** No hay master ni hijos. No hay ESP-NOW
en la ruta de medida.

```
PC (bridge Node)  ──HTTP /api/raw──>  unidad 0  (M)
                  ──HTTP /api/raw──>  unidad 1  (L1)
                  ──HTTP /api/raw──>  unidad 2  (L2)
```

ESP-NOW **se queda en el firmware**, pero solo como canal de aprovisionamiento:
sirve para mandarle las credenciales WiFi a una unidad que todavía no está en la
red. Para eso funciona bien.

### Por qué se abandonó ESP-NOW como transporte de medida

Aguantaba 3-4 barridos y se caía. Se probaron y descartaron **con medida** nueve
hipótesis (cola de envío, potencia de TX, reinicios, fuga de memoria, bus I²C,
filtro de secuencia, placa defectuosa, posición en la cola, deriva de canal).
Cada arreglo movía el fallo de sitio sin quitarlo.

Causa de fondo: **el ESP32-C3 tiene una radio y un canal a la vez**. Un hijo no
puede escuchar ESP-NOW en el canal del master y a la vez buscar un AP por todos
los canales.

El camino por HTTP da **8 de 8 barridos con huella idéntica** y es igual de
rápido.

### El ORDEN de las unidades es parte de la receta

Define los nombres de canal (`M`, `L1`, `L2`). Un golden capturado con otro
orden **no significa lo mismo**. Se guarda dentro del job.

---

## 2. Estado del hardware

| Unidad | IP | Papel |
|---|---|---|
| `2898` | 192.168.1.90 | Tester #1 — posición **M** |
| `700C` | 192.168.1.101 | Tester #1 — posición **L1** |
| `424C` | 192.168.1.102 | Tester #1 — posición **L2** |
| `8744` | 192.168.1.96 | Tester #2 — socket amarillo |

Las cuatro en **firmware v1.8.0**, todas actualizables por **OTA**:

```bash
python tools/ct.py --ota 192.168.1.90
```

Las IP las mueve el DHCP: comprobar con `python tools/ct.py --list`.

---

## 3. Errores encontrados y corregidos

Los tres del mismo patrón: **una verificación previa escrita con confianza que
no podía distinguir el caso correcto del incorrecto**.

### 3.1 Direcciones I²C invertidas (fw 1.5.0)

`config.h` tenía `U1 = 0x21` cuando el esquemático dice `0x20`. El firmware
llamaba `J00` a lo que físicamente es `J16`: **todos los nombres desplazados 16
posiciones**.

Invisible para cualquier prueba: con un bucle `CN1↔CN2`, `J00↔J16` y `J16↔J00`
son el mismo net. El golden funcionaba y las piezas malas se rechazaban bien;
solo el informe apuntaba al conector equivocado.

**Comprobado con multímetro**: excitando el canal 0, antes había tensión en
`CN2 pin 1`; después de corregir, en `CN1 pin 1`.

### 3.2 Enables del MUX (fw 1.5.0)

Estaban "corregidos" desde septiembre para compensar 3.1. Los dos errores **se
cancelaban**. Devueltos a lo que dice el esquemático: `A_EN = bit 14`,
`B_EN = bit 15`.

### 3.3 Mapa de bits de los LEDs (fw 1.4.0)

Decía "5 rojos y luego 5 verdes". Van **intercalados verde/rojo por canal**, y
el canal 5 vive en el segundo 595.

### 3.4 El RGB "aleatorio" — no era aleatorio

El WS2812 cuelga de `IO8`, que es **el reloj del I²C**. El tráfico del bus se
pinta en el LED. No se puede animar durante una medida sin corromper lecturas
**sin dar error**.

Hay una op `rgb_off` que lo apaga con el bus en reposo. La animación de avance
se hace en los LEDs rojos/verdes, que cuelgan del registro de desplazamiento y
no estorban.

**Decisión (2026-09-26): el RGB no se usa.** No aporta nada que no digan los
LEDs rojo/verde, y cada destello que se le manda viaja por el reloj del I²C.
Por eso deja de ser un pendiente: no hay que arreglarlo, hay que no usarlo.

Apagarlo **no es un paso aparte**: `rgb_off` va como última op dentro de
`release()` en `bridge/measure.js`, la misma petición que suelta el bus. Así lo
heredan los cinco caminos que terminan una medida —barrido, resistencias,
módulos, clasificar aislado y socket— sin que haya que acordarse en cada uno.
Antes solo lo apagaba el barrido. También se apaga al abrir las unidades, por
si una viene encendida de una sesión que murió sin soltar el bus.

**Comprobado en la 2898**: escribiendo `0xA5A5` en el expansor y releyéndolo con
un `rgb_off` de por medio sale `42405` igual que sin él — el `Wire.end()` /
`Wire.begin()` que hace la op no rompe el bus ni pierde el estado.

Donde **sigue** encendiéndose a propósito es en el arranque del firmware
(rojo/verde/azul y el pulso de "conectado", antes de `Wire.begin()`). Ahí es
seguro y es el único indicador que hay todavía sin red.

> Si algún día se rediseña la PCB, el pin sigue mal elegido. No está anotado en
> `docs/REV_NEXT.md`.

---

### 3.5 Los MUX — validados, sin cambios

Último IC cotejado contra el esquemático (2026-09-26). **No había nada que
corregir**: `In → J(n)` en U5 y `In → J(16+n)` en U6, `I15` de U6 = `J_SPARE`,
y los enables en su sitio. Los tres ICs del módulo tester quedan verificados.

Comprobado **midiendo los 32 canales**, no leyendo código: excitar uno con todo
lo demás a 0 V y ver que aparece en su MUX y en ningún otro. Detalle y el porqué
de la primera prueba fallida en
[docs/CARACTERIZACION.md §11](docs/CARACTERIZACION.md).

---

---

## 3.6 El botón físico — el equipo avisa, la PC mide

Firmware **1.7.0**. Un pulsador entre **IO4 y GND** (normalmente abierto) en cada
master. Al pulsarlo corre **la misma prueba que el botón de la pantalla**, con
el SKU y el operador que estén puestos.

**El equipo NO prueba por su cuenta**, y esa fue la decisión de diseño:
`runTestCore` exige un golden en la NVS del ESP32, y aquí el golden vive en la
PC. Sin golden local la prueba interna aborta y —lo importante— **ni siquiera
incrementa `seq`**, así que la app no se entera de nada: un botón que no hace
nada visible es peor que no tener botón.

Así que el disparador tiene dos modos, y el de fábrica es el primero:

| Modo | Qué hace |
|---|---|
| `avisar` (defecto) | Solo cuenta la pulsación en `trigger.presses`. Mide la PC. Funciona igual con una unidad que con tres |
| `local` | El equipo corre su prueba con el golden de su NVS |

El bridge sondea `/api/status` y, cuando el contador sube, lanza la prueba.
**Un contador y no un "pulsado sí/no"**: la pulsación dura ~100 ms y el sondeo
va cada 400, así que con un booleano se perderían pulsaciones y una pulsación
larga se leería como varias.

### El SKU lo decide el EQUIPO que se pulsa

**Bug corregido (2026-09-27).** Se usaba `sku_activo` a secas, así que pulsar el
botón de la 2898 corría el job de la 8744: el operador pulsaba en un puesto y se
medía otro. Ahora se resuelve por la unidad que pulsó, con este orden:

1. El SKU que hay en pantalla — **solo si su job incluye ese equipo**.
2. El último SKU corrido **en ese equipo** (por eso `tests.tester_mac` se rellena).
3. Si el equipo tiene un único job aprobado, ese.
4. Si no se puede decidir: **avisa y no corre nada**. Arrancar el producto
   equivocado da un RECHAZADO que parece un defecto de la pieza.

`GET /api/boton/estado` responde qué haría cada botón **sin pulsarlo**, y la
ficha del equipo lo muestra.

### El cerrojo es por unidad, no global

Dos barridos sobre la misma placa se pisan los expansores; sobre placas
distintas no se estorban en nada. El cerrojo global serializaba toda la planta
por un problema que solo existe dentro de cada tester. Ahora se reservan los
hosts del job — el arnés grande reserva sus tres unidades.

El bridge necesita saber **qué producto y quién**: la pantalla los guarda en
`settings` (`sku_activo`, `operador_activo`) porque el botón se pulsa aunque el
navegador esté cerrado. Sin ellos no prueba — avisa y no inventa trazabilidad.

> **Dos bridges NO comparten cerrojo.** Si hace falta una segunda instancia
> (probar un cambio, una copia de datos), arrancarla con `CT32_SIN_BOTON=1`:
> si no, dos vigilantes ven la misma pulsación y disparan dos pruebas.

**Verificado de punta a punta (2026-09-27):** pulsación → `presses` 0→1 →
prueba #35 `PASS` en 3238 ms, auditada como `boton:8744`, y el veredicto
apareció solo en la pantalla.

Las unidades sin botón se sondean cada 10 s en vez de cada 400 ms; con firmware
anterior a 1.7.0 el bridge las ignora sin dar error.

---

## 3.6.1 Entrar por EQUIPO, no solo por producto

Hasta ahora todo se entraba por el SKU: elegir producto y que el job decida qué
equipos usa. Pero medio trabajo real es al revés —"voy a ver qué le pasa a la
8744", "a esta le monto otro job"— y por la ruta del SKU eso obliga a adivinar
qué producto toca **antes** de poder mirar el equipo.

Hay un **selector de equipo** en la cabecera, al lado del operador. Fijar uno no
cambia Pruebas (ahí manda el job, como siempre): fija sobre qué equipo trabajan
Diagnóstico y el avanzado.

### Montar un job EN el equipo — y el bug que lo impedía

`Configuración → ⋮ → Ver detalles` muestra el golden grabado en la NVS del
ESP32 y permite cambiarlo. Con **"dejarlo listo para trabajar solo"** además
pone el disparador en modo `local`: golden dentro + botón que mide por su
cuenta.

> **`POST /api/golden` no grababa nada, desde siempre.** `apiGoldenSet` nunca
> asignaba `m.channels`, y `mapSaveGolden` sale por `if (!m.valid ||
> !m.channels) return false` sin escribir. Respondía `ok:false` con `crc:0` y el
> equipo se quedaba igual. No se notó porque la PC orquesta y nadie usaba esa
> ruta. Corregido en **1.7.1**.

**El CRC se calcula en el bridge**, no se acepta el que devuelve el equipo:
comparar su respuesta consigo misma no comprueba nada — si grabara mal la mitad
de los bytes, devolvería el CRC de lo que grabó y todo parecería correcto.

**Verificado sin PC (2026-09-27):** golden de MPG-IN3 montado en la 8744 (crc
65122, calculado aquí = el del equipo), disparador en `local`, y el equipo
juzgando la pieza **él solo: PASS en 556 ms**. Con el golden equivocado montado
a propósito (MPG-IN1 sobre el arnés de MPG-IN3) dio FAIL con las 7 líneas
esperadas abiertas — que es lo que tiene que pasar.

> Un arnés de más de un equipo no se puede montar así: el modo autónomo mide con
> una sola unidad. La app lo rechaza antes de intentarlo.

---

## 3.7 Estaciones — varios puestos en un bridge

Un **puesto de trabajo es un equipo con botón**, y lleva **su** producto y **su**
operador (tabla `estaciones`). Se configuran en Configuración → Estaciones.

**Por qué no bastan los ajustes globales:** con dos puestos colgando del mismo
bridge, un `sku_activo` único hace que el operador de un puesto se lleve el
producto del otro. El botón funcionaría igual y la trazabilidad mentiría — peor
que no registrar nada.

### Registro parcial antes que pieza perdida

Si a un puesto le falta el operador, la prueba **se registra igual** y queda
marcada `parcial = 1`. Antes se negaba a correr, y eso hacía que la pieza no
existiera en el historial: ni que se probó, ni que falló. Un registro incompleto
se completa después; una pieza que no aparece está perdida.

### Cuándo se hereda el contexto de la pantalla

Solo en dos casos, y por una razón concreta en cada uno:

| Caso | Por qué |
|---|---|
| Hay **un único** puesto con botón | La pantalla *es* ese puesto |
| La pantalla está en **ese mismo producto** | Quien lo tiene seleccionado es quien está probando |

Para un puesto que corre otro producto, heredar sería **firmar la pieza con el
nombre de otra persona**. En ese caso: sin operador → parcial.

> El SKU sí se hereda siempre, porque ya va filtrado por "tiene que ser un job
> de ESTE equipo": el producto de otro puesto no cuela.

Y una trampa evitada: la regla mira **cuántos puestos con botón hay de verdad**,
no si existe una fila en `estaciones`. Guardar una estación en blanco no puede
cambiar cómo se comporta un botón que ya funcionaba.

---

## 3.8 Cuántos equipos por PC, y si pueden ir solos

**Simultáneo:** sí, entre testers distintos. Desde que el cerrojo es por unidad,
dos puestos con sus propios equipos pueden probar a la vez; el bridge es
asíncrono y una prueba son ~4 s de peticiones HTTP pequeñas, no CPU. Lo que
nunca puede solaparse es dos cosas sobre **la misma** placa.

**Resuelto para 2-4 puestos** con la tabla `estaciones` (§3.7): cada uno lleva
su producto y su operador, y lo que no tenga se registra como parcial en vez de
perderse. El límite real por encima de eso está sin medir — se medirá cuando
haya más de cuatro, no antes.

**Autónomo (sin PC):** sí, con dos condiciones. El equipo necesita su golden en
la NVS (`push-job`) y el disparador en modo `local`. Entonces arranca solo, y al
pulsar mide contra su golden y canta el veredicto en los LEDs — cinco verdes o
la zona en rojo. No guarda historial: solo la última prueba en RAM, que la app
puede recoger por `/api/test/last` cuando vuelva.

Dos límites del modo autónomo:

- **Una sola unidad.** El mapeo repartido entre equipos se abandonó (ESP-NOW,
  §1), así que un arnés de 96 canales no se puede probar sin PC.
- **Sin trazabilidad.** Ni lote, ni etiqueta, ni quién lo probó. Sirve para no
  parar la línea si se cae la PC, no para producir a ciegas.

---

## 3.9 Mover los equipos a otra planta — WiFi por lote

Al llegar a planta ninguna unidad encuentra la red de aquí, así que **todas
abren su portal**. El SSID del AP ya lleva diferenciador —`CableTester32-8744`,
los dos últimos bytes de la MAC, el mismo id que se ve en la app— así que desde
el móvil se sabe a cuál te conectas.

### El procedimiento

1. Móvil → red `CableTester32-<id>` de **cualquiera** de ellas.
2. Se abre el portal solo (captivo). Elegir la red de la planta y la clave.
3. Marcar **"Mandar esta red a los demás CableTester"**.
4. Guardar. Esa unidad difunde las credenciales, se reinicia y se conecta; las
   demás las reciben, las guardan y se reinician solas.

### Los controles, y por qué cada uno

| Control | Qué evita |
|---|---|
| Solo difunde si el operador **marca la casilla** | Reconfigurar en silencio equipos que nadie ha tocado |
| Solo la aplican las unidades **sin red** (rol SLAVE: portal o esperando) | Que una difusión saque de su red a un equipo que está trabajando |
| Se manda **antes** de responder al móvil | Al reiniciar, esta unidad se lleva su AP y ya no hay "después" |
| El portal fija el **canal 1** | ESP-NOW solo alcanza a quien esté en el mismo canal. Con el canal por defecto no está garantizado que coincidan |

> En modo AP la librería pasa a AP_STA, y las dos interfaces **comparten la
> única radio** y por tanto el canal del AP. Por eso el envío por la interfaz
> STA sale en el canal 1, que es donde escuchan las demás.

### El fallo que lo hacía imposible (corregido en 1.8.0)

Con el portal abierto, `loop()` hacía `wifiPortalLoop(); return;` y **nunca
llamaba a `linkLoop()`**. Una unidad en modo AP recibía las credenciales por
ESP-NOW —el callback las dejaba en `_creds_pending`— y nadie las aplicaba: se
quedaba en el portal para siempre. El aprovisionamiento por lote no podía
funcionar justo en el caso para el que existe.

### ⚠️ Sin verificar en hardware

Compilado y razonado, **no probado**: hace falta tener dos unidades en modo AP a
la vez y un móvil, y desde la PC no se puede provocar sin dejarlas sin red.
Cuando se pruebe, lo que hay que mirar:

- Que las otras unidades salgan del portal **solas**, sin tocarlas.
- Que una unidad **ya conectada** ignore la difusión (rol MASTER).
- Que sin marcar la casilla **no se difunda nada**.

Si la difusión no llega, el portal lo dice y siempre queda configurarlas una a
una — no se pierde nada, solo tiempo.

---

## 4. Herramientas

### Línea de comandos (`tools/`)

```bash
python tools/ct.py --list                    # unidades en la red
python tools/ct.py --ota <ip>                # actualizar firmware
python tools/ct.py <ip> "<comando>"          # consola

python tools/sweep.py <ip1> <ip2> --modulos          # validar placas SIN arnés
python tools/sweep.py <ip1> <ip2> --repeat 8         # repetibilidad
python tools/sweep.py <ip1> <ip2> --save-golden f.json --esperadas 46
python tools/sweep.py <ip1> <ip2> --check f.json     # código 0/1

python tools/analog.py <ip1> <ip2>           # resistencia y fuga por línea
```

### App local (`../CableTester32App/`)

```bash
npm start          # bridge + UI en http://localhost:3001
npm run build      # recompilar la UI tras tocar src/
```

Pestañas: **Pruebas**, **Resultados**, **SKUs**, **Socket**, Diagnóstico,
Etiquetas, Logs.

---

## 5. Lo que está hecho en la app

- **Motor de medida portado a Node** y validado: misma huella que el Python
  contra el mismo hardware.
- **Pruebas**: SKU → job → barrido → veredicto → LEDs → registro + lote.
- **Resultados**: tabla desde SQLite, filtros, contadores, CSV.
- **SKUs**: asistente de 5 pasos con barreras bloqueantes, y edición.
- **Socket**: aprendizaje del fixture con pitido (ver §7).
- **Etiquetas**: se generan al cerrar el lote. **El PDF se guarda siempre**, se
  imprima o no, y se puede reimprimir.

### Las tres barreras del golden

Cada una ve lo que las otras no:

| Barrera | Detecta |
|---|---|
| `--modulos` | Canales pegados a masa en las placas |
| Resistencia | Contactos flojos que el mapa da por buenos |
| `--esperadas` | Hilos que **faltan** (una línea ausente no tiene R que medir) |

Caso real: un golden tenía las 46 conexiones pero un contacto a **116 Ω**
intermitente (sus vecinas, 1,0 Ω); el otro tenía contactos perfectos y **le
faltaba un hilo**. La comprobación de resistencia aprobó el segundo.

---

## 6. Pendiente — solo lo puede hacer el usuario

0. **El arnés grande montado ahora mismo da 45 conexiones, no 46.**
   Barrido libre del 2026-09-28 sobre 2898 + 700C + 424C (huella `571f5a4d`):
   45 parejas y **6 canales libres** — `M.J21`, `L1.J24`, `L2.J26`, `L2.J28`,
   `L2.J30`, `L2.J_SP`. Con 46 conexiones deberían quedar 4 libres. Falta una
   línea, y las 45 que hay salen todas *unidas* en el osciloscopio, así que no
   es un contacto flojo: es un hilo que no está.

1. **Los códigos de SKU reales.** Los `TODO-` salen marcados en ámbar en la UI.
   Están en `../CableTester32App/bridge/seed.js`.
2. **Reparar los dos golden del arnés grande**: al 1 le falta rehacer el crimp
   de `M.J11↔L1.J14` (116 Ω); al 2 le falta el hilo `M.J05↔L1.J08`. El arnés
   correcto tiene **46 conexiones** (96 canales − 4 libres = 92 = 46 parejas).
3. **Recorrer el socket con la punta** (pestaña Socket) para que los informes
   digan `CN15.3` en vez de `M.J11`.

---

## 7. El problema de los nombres

El motor habla en coordenadas internas: `M.J11`, `L1.J14`. **Eso no le sirve a
nadie**: no dice a qué conector ir ni a qué pin.

La pestaña **Socket** construye la traducción **midiendo**: se mete 3,3 V con
una punta en cada pin del JST, el tester detecta qué canal se puso alto y pita.
Recorrido el socket, la tabla está completa — y es la del fixture real, no la
del esquemático.

De paso valida el socket: si un conector de 10 posiciones solo da 9 pitidos, hay
un contacto muerto.

El tester **solo escucha**; la tensión la pone el operador. No hizo falta
firmware nuevo.

---

## 8. Reglas que conviene no romper

- **El contrato**: el navegador nunca habla con el ESP32, y la app **no parsea
  el texto de `/cmd`** — solo `/api/*` JSON versionado.
- **La huella tiene que incluir los aislados.** Si solo cubre los nets, un
  barrido con 0 nets produce la huella de la cadena vacía y comparar
  repeticiones deja de comparar nada. Ya pasó.
- **Nunca usar un valor de datos como centinela de error.** Pasó dos veces:
  `0xFFFFFFFF` (32 pines altos es una lectura legítima) y `0x80000000` (máscara
  con `J_SP` excitado, que como `int32_t` es `INT32_MIN`).
- **Medir en los dos sentidos** al calcular resistencia entre unidades. El
  método es diferencial para cancelar el error del ADC, pero eso solo vale si es
  el **mismo** ADC. Entre placas el sesgo (~22 Ω) se cuela como resistencia
  falsa y el tester rechazaría el 100% de las líneas sanas.
- **Doble muestra por fase.** Un pin conectado sigue al driver siempre; uno al
  aire se mueve por acoplamiento y no repite.
- **El RGB se apaga al soltar el bus, no cuando alguien se acuerda.** Va dentro
  de `release()`. Cuando apagarlo era un paso suelto, cuatro de los cinco
  caminos que terminan una medida se lo saltaban y el LED se quedaba encendido
  con la basura del bus.
- **El golden vive en la PC**, no en el ESP32. Si una unidad se borra, se monta
  otra y se le dice qué job correr.
