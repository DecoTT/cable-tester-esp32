# Datos para el video del Cable Tester — respuesta del 2026-11-10

Respuesta a las preguntas de la sesión «Cable tester video production».
Marco con **[MEDIDO]** lo que tengo comprobado, con **[DOC]** lo que viene de la
documentación anterior y no he vuelto a verificar, y con **[NO SÉ]** lo que no
puedo responder.

---

## 1. Las cifras de la tienda contra ESTADO: no se contradicen

No hay conflicto. Son **montajes distintos**, y la tienda cita bien
[CARACTERIZACION §1](CARACTERIZACION.md).

| cifra | de dónde sale | estado |
|---|---|---|
| 11–14 s para **64 canales** | 2 equipos | **[MEDIDO]** §1 |
| ~22 s para 96 | era una **estimación** en §1 | ahora **[MEDIDO]: 24–26 s** |
| resolución ≈ 1 Ω | §1 | **[MEDIDO]**, y coherente con la repetibilidad de abajo |
| fuga a GND por debajo de ~600 kΩ | §1 | **[DOC]** — no la he vuelto a medir |
| 8/8 corridas idénticas | §1, sobre **32/32 líneas en 64 canales** | **[DOC]** |

El tiempo escala con **canales × unidades**, así que los 26 s de ESTADO y los
11–14 s de la ficha son la misma máquina con distinto montaje. En el barrido,
casi todo el tiempo es **latencia de red**, no medida: unas 8 peticiones HTTP
por canal excitado.

**Repetibilidad que sí medí yo** (2026-09-30, 46 líneas / 96 canales / 3
equipos):

- Mapa: **la misma huella** en pasadas consecutivas, y también barriendo en
  sentido inverso (95→0) y con 1, 10 y 20 ms de asentamiento. **[MEDIDO]**
- Resistencia, 3 pasadas sin tocar nada: rango mediano **0,45 Ω**, p90
  **1,54 Ω**. **[MEDIDO]** — de ahí que «≈1 Ω» sea honesto.

### ⚠️ «Monitoreo de contacto intermitente» — esta hay que matizarla en el video

Es la única afirmación de la ficha que puede inducir a error, y está **medido
que la prueba normal NO lo hace**:

> **Un contacto intermitente no se puede detectar con una sola pasada, ni
> haciéndola más lenta.** (CARACTERIZACION §16.4 y §16.5)

Lo que sí es cierto, y es mejor historia:

- **La vista de osciloscopio sí lo ve**: sacudiendo el arnés mientras muestrea,
  un crimp flojo aparece en la traza. **[DOC §14]**
- **Desde el 2026-11-05, los SKU marcados como «subensamble» miden resistencia**
  y avisan de un contacto que va justo. **[MEDIDO el camino, NO contra hardware]**
- El caso real: `CN12.5 ↔ CN16.4` dio **39, 6 y 69 Ω en tres medidas** y **pasó
  el test digital las tres veces**. **[MEDIDO]**

**Cómo lo dice el usuario (10-nov), y es la forma correcta:** no hay vibración
automática ni stress test — **eso lo hace el operador**. Si el operador mueve el
arnés mientras se muestrea, **sí se detecta, y ya pasó en planta**.

Es decir: la frase honesta es *«con el arnés en la mano, moviéndolo, el tester
lo ve»*, no *«el tester los detecta solo»*. Si el video dice lo segundo, un
cliente va a esperar que la corrida PASS/FAIL los cace, y no los caza.

---

## 2. Qué cambió desde el 19-oct

**Firmware: sigue en 1.8.0**, sin cambios. **[MEDIDO]** (`config.h:227`, y el
`.bin` publicado es `bring_up-1.8.0.bin`).

En la app sí hay bastante, todo en git:

| cambio | fecha | por qué existe |
|---|---|---|
| **Unidades por MAC, no por IP** | 19-oct | En Guadalajara el DHCP dio otras IPs y la prueba fallaba tras **143 s de timeouts** contra una IP que ya no existía, con el mDNS viendo los tres equipos perfectamente |
| **Identificar equipo con SW2** | 30-sep | Tres cajas iguales sin etiqueta: se presiona el reset y la app dice cuál es |
| **Identificar un pin con la punta** | 30-sep | El recorrido de socket al revés: los números de pin son el **orden en que se mapeó**, no una serigrafía |
| **`intermitente` como tipo propio** | 30-sep | Media alta + desviación alta no es acoplamiento |
| **Subensambles** | 5-nov | Solo se excitan los canales de su golden: de ~24 s a ~5 |
| **Límites de resistencia por SKU y por línea** | 5-nov | Editables en planta; los 5/50 Ω de partida son del firmware, no una medida del producto |
| **Exportar por partes / importar sin duplicar** | 5-nov | Varias PC: el job de una pisaba el de otra |
| **Aviso de versión nueva** | 5-nov | Enterarse de un arreglo dependía de un correo |

**Lo que más vale un video, en mi opinión: el de las IPs.** Es un fallo que
parecía del tester y era de la red, el mDNS lo desmentía, y la solución —atar
por MAC— se explica en una frase.

---

## 3. Hardware físico

| pregunta | respuesta |
|---|---|
| Placa | **[DOC]** `ARCHITECTURE.md`: **ESP32-C3-MINI-1-N4, «SBC Rev1.1»**, esquemático EasyEDA Rev 1.0 (2026-03-02). No «Dev 06 rev2» |
| Botones del módulo | **[MEDIDO contra el esquemático]** **SW1 → IO9** (BOOT, y **es SDA del I²C**) y **SW2 → EN** (reset). Los dos con pull-up de 10 k |
| Cómo van montados | **[USUARIO, 10-nov]** El módulo ESP32 va **encima** del módulo tester |
| Carcasa negra | **[USUARIO, 10-nov]** Existe, y **la diseñaron para esta aplicación** |
| Color de la máscara | **[USUARIO, 10-nov]** ESP32 **azul**, tester **verde**, módulos de conexión de arneses **azules** |

Esto lo contestó el usuario el 10-nov y ya se puede usar en el render.

Un dato útil para el guion, y sale de medir: **SW1 no se puede usar como
botón** aunque sea el único que trae la placa. Cuelga de IO9, que es la línea de
datos del I²C: presionarlo pone SDA a tierra y puede colgar el bus. Por eso el botón
de disparo va soldado a IO4.

---

## 4. Qué se puede grabar hoy: **nada aquí**

Los cuatro equipos (**2898, 700C, 424C, 8744**) se reprovisionaron el **30-sep**
a la WiFi de planta `INFINITUM2774` y **están en Guadalajara**. No están en esta
red. **[MEDIDO]** — lo comprobé tras el reinicio: ninguno respondía.

El usuario comentó el 5-nov que tiene **un arnés armado y otro por ensamblar**,
pero **el tester no**. Para grabar aquí haría falta que vuelva un equipo y
volver a meterle las credenciales de esta red por el portal (dos minutos desde
el móvil, AP `CableTester32-XXXX` → `http://192.168.4.1/`).

### ⚠️ Lo que no debería salir en cámara

1. ~~**El fixture y el golden son del cliente.**~~ **Resuelto: el usuario
   autorizó mostrar el mapa del fixture en el video (10-nov).** Siguen fuera de
   git, pero eso es por no mezclar datos de cliente con el producto, no por
   confidencialidad.
2. **La contraseña de la WiFi de planta.** Esta sigue en pie. Aparece en una foto del módem que el
   usuario me mandó. Si esa foto o una captura de la app con ese SSID acaba en
   un video, se publica la red de un cliente.

Los arneses son **rechazos de cliente** —hilo sólido en vez de trenzado, defecto
ya corregido en su producción—, así que mostrarlos como «mira qué mal venían»
también es decisión del usuario.

---

## 5. Las historias con datos medidos

Ordenadas por lo bien que se sostienen. Las de **software** las medí yo; las de
firmware y diseño son anteriores y solo puedo señalarlas.

**Las tres mejores, todas [MEDIDO]:**

1. **El barrido rápido que mentía.** Leer las tres unidades en paralelo bajó el
   barrido de 24 s a 12 s… y dio **8, 23 y 14 aperturas** en tres pasadas sobre
   la misma pieza, que un momento antes daba **una sola, dos veces seguidas**.
   El dueño del canal reconfigura sus pines dentro de su propia lectura y en
   paralelo las otras leen durante esa ventana. Moraleja: *un tester rápido que
   miente es peor que no tener tester.* (§16.7)

2. **Las dos poblaciones de defecto.** Tres contactos **muertos** (128–181 mV,
   4/4 en barridos repetidos) frente a cinco que **chisporrotean** (3 V de media
   con 600–940 mV de desviación). Los segundos el barrido los aprueba o los
   rechaza **según el instante**, y por eso los fallos «se movían» entre
   acoplamientos. Y el multímetro del usuario confirmó que el hilo conduce:
   **0,1 Ω**. El fallo está en el acoplamiento, no en el cable. (§16.4)

3. **La hipótesis que se descartó midiendo.** «El barrido va demasiado rápido»
   era razonable. Cuatro pasadas con 1, 10, 1 y 20 ms de asentamiento —con el
   control intercalado a propósito— dieron **la misma huella exacta**. (§16.3)

**Otras dos que se cuentan en treinta segundos, [MEDIDO]:**

- **`localhost` contra `::1`.** El navegador decía «localhost rechazó la
  conexión» con el bridge perfectamente arriba: en Windows `localhost` resuelve
  primero a IPv6 y el bridge solo escucha en IPv4. Medido: `127.0.0.1` responde
  en 0,03 s, `[::1]` no conecta.
- **El sesgo entre ADCs.** Cada ESP32 tiene su propio error de ganancia, y el
  desacuerdo entre sentidos es **sistemático por par de cajas**: M↔L1 +38,
  L1↔L2 −28, M↔L2 +12 Ω. Cuadran entre sí (+38−28 = +10 contra +12 medido). Por
  eso la resistencia se mide **en los dos sentidos y se promedia**: una línea de
  0,6 Ω mediría 39 Ω en un sentido y −37 en el otro. (§16.2)

**[NO SÉ] si tienen datos publicables:** las I²C invertidas, los LED, el
abandono de ESP-NOW, la vía huérfana que se tragó el pour de GND, F1 y el LED en
el reloj del I²C. Son anteriores a mí; están en ESTADO §3 y en los documentos de
diseño, pero no los he verificado.

---

## 6. §14, el margen de 750 mV: **tenías razón, estaba mal**

La resta es correcta: `3300 − 2900 = 400`.

El umbral de 2900 mV cae en el hueco, pero **el margen no es simétrico**:

- **767 mV por debajo**, hasta el vecino acoplado (2133 mV).
- **400 mV por encima**, hasta la punta (3300 mV). **El lado estrecho es este.**

Ya está corregido en `CARACTERIZACION.md §14`, con nota de quién lo encontró.
Gracias: estaba publicado como si fueran 750 a los dos lados.

---

## 7. Terminología: aplicada el 10-nov

Corrección del usuario: *«en español pone masa en lugar de tierra (GND)..
voltios.. que ponga volts.. no "españolice" la traducción, los términos se
quedan»*. Hecho en los dos repos:

| antes | ahora |
|---|---|
| masa | **tierra**, y **tierra (GND)** en lo que ve el operador |
| voltios / milivoltios | **volts / milivolts** |
| pulsar, pulse, pulsando | **presionar, presione, presionando** |
| fichero | archivo |
| ordenador | computadora |

Dos excepciones, a propósito:

- **`pulsación` / `pulsaciones` se queda.** No es término de España, y
  `pulsaciones` es además el nombre del campo que devuelve el firmware en
  `/api/status`: cambiarlo rompería el contrato con los equipos que ya están en
  Guadalajara.
- **Del firmware solo se tocaron comentarios** (`raw.cpp`, `raw.h`), que no
  entran en el binario. Las cadenas en español que quedan —comentarios de
  `bring_up.ino` y una línea de la consola serie, `«Pulsar = ejecutar
  prueba»`— **se dejaron como están**: editarlas dejaría el fuente distinto del
  `bring_up-1.8.0.bin` que está publicado y cargado en los cuatro equipos de
  Guadalajara. Si el video va a enseñar la consola serie, dímelo y se hace un
  1.8.1.

**Para el video: usa esta terminología también en los rótulos y la locución**,
porque si el rótulo dice «masa» y la pantalla dice «tierra (GND)», se nota.
