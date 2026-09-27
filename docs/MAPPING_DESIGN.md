# Motor de mapeo — diseño

**Estado:** ✅ IMPLEMENTADO en firmware v0.7 — local y distribuido  
**Sustituye a:** la lógica de loopback fija `J(i)↔J(i+16)` y a la distinción de modos 16P/32P

---

## 1. Idea central

El firmware **deja de saber** cómo está cableado el arnés. Lo descubre.

1. Se conecta un arnés **golden** (bueno conocido)
2. Comando `map` → recorre canal por canal y construye la lista de conexiones
3. Ese mapa se guarda como referencia
4. Cada arnés siguiente se mapea igual y se **compara** contra el golden

Consecuencia: da exactamente igual si una conexión es U1↔U2, U1↔U1 o U2↔U2, ni en qué
unidad física está cada extremo. **Mapear es el primer paso para todos los arneses.**

Esto vuelve innecesarias dos cosas que antes parecían obligatorias:
- Conocer la topología del arnés de antemano
- Los modos 16P / 32P

---

## 2. Modelo de canal

Se abandona el modelo «U1 = lado A, U2 = lado B». Pasa a ser un espacio plano:

```
CHANNEL[0 .. N-1]      N = 32 por unidad × unidades enlazadas
```

Cada canal tiene tres estados posibles: `INPUT` · `OUTPUT LOW` · `OUTPUT HIGH`.

Esto es viable porque el **TCA9535 configura la dirección por pin** (registros CONFIG 0x06/0x07),
a diferencia del 74HC595, que es solo salida.

---

## 3. ⚠️ El problema: el TCA9535 NO tiene pull-ups internos

> TI, hoja de datos: *«El TCA9535 no incluye la resistencia de pull-up interna, lo que exige
> pull-ups o pull-downs en los pines configurados como entrada que queden sin excitar.»*
> Es igual al PCA9555 **salvo por la eliminación del pull-up interno**.

Por eso el algoritmo ingenuo **no funciona**:

```
❌  i → OUTPUT,  los otros 31 → INPUT,  leer
```

Un canal no conectado queda **flotando** y lee basura. No se puede distinguir
«no conectado» de «conectado», y el mismo arnés daría mapas distintos en cada pasada.

---

## 4. Algoritmo correcto: lectura complementaria con precarga

```
Para cada canal fuente i en 0..N-1:

  Fase ALTA
    1. TODOS los canales → OUTPUT LOW        (fuerza 0 V; borra carga residual)
    2. Todos → INPUT excepto i
    3. i → OUTPUT HIGH
    4. esperar settle
    5. leer todos los canales               → mask_H

  Fase BAJA
    6. TODOS los canales → OUTPUT HIGH       (precarga inversa)
    7. Todos → INPUT excepto i
    8. i → OUTPUT LOW
    9. esperar settle
   10. leer todos los canales               → mask_L

  Conectado(i, j)  ⟺  mask_H[j] == 1  Y  mask_L[j] == 0
```

**Por qué funciona:** un pin flotante **retiene** el estado con el que se precargó — lo sostiene
la capacidad de entrada CMOS. Un pin realmente conectado **sigue** al driver. Exigir que siga
*ambas* transiciones descarta los flotantes.

**La precarga no es opcional.** Sin ella, un pin que quedó cargado alto de la iteración anterior
produce un falso positivo en la fase ALTA.

### 4.1 Seguridad

En todo momento hay **como máximo un canal en modo salida**. Nunca se enfrentan dos drivers
deliberadamente. Los 680 Ω serie cubren el caso accidental (3.3 V / 1360 Ω = 2.4 mA), pero la
rutina no debe depender de esa protección.

### 4.2 Casos ambiguos

Si un canal da lecturas inconsistentes entre pasadas, se puede medir su **nivel analógico real**
con el MUX + ADC (`scope_read`) en vez del umbral digital. Es lento, así que solo para
diagnóstico puntual, no para el barrido completo.

---

## 5. Representación del mapa: NETS, no pares

Guardar **clases de equivalencia**, no una lista de parejas:

```json
{
  "nets": [
    ["J00", "J19", "J41"],
    ["J01", "J27"],
    ["J02", "J03"]
  ]
}
```

Con pares, un empalme de tres puntos obliga a listar J00-J19, J00-J41 y J19-J41 y a mantener
la coherencia entre ellos. Con nets sale gratis.

### 5.1 Comparación contra el golden

Comparar dos **particiones** del conjunto de canales:

| Diferencia | Diagnóstico | Reporte |
|---|---|---|
| Un net del golden se **parte** | Línea abierta | Qué canal se quedó aislado |
| Dos nets del golden se **fusionan** | Corto | Entre qué canales |
| Un canal **cambia** de net | Mal ruteado | De dónde a dónde |
| Canal aislado en el golden aparece en un net | Corto nuevo | Contra qué |

Los tres fallos que hay que reportar —abierto, corto, ruteo distinto— salen de comparar
particiones. No hace falta lógica dedicada para cada uno.

---

## 6. Presupuesto de tiempo

Por canal fuente: ~20 operaciones de registro I²C ≈ **7 ms** a 100 kHz.

| Escenario | Canales | Tiempo estimado |
|---|---|---|
| 1 unidad | 32 | ~0.25 s |
| 3 unidades enlazadas | 96 | ~1.4 s (incluye ida y vuelta ESP-NOW) |

Subir el bus I²C de 100 kHz a **400 kHz** lo bajaría a menos de la mitad. Los pull-ups de
4.7 kΩ (R36/R37) lo permiten sin problema en pistas cortas.

---

## 7. Mapeo distribuido — protocolo

> **La app de PC NO puede orquestar esto.** Dos motivos: los hijos solo son alcanzables por
> ESP-NOW desde el master (no tienen IP), y aunque la tuvieran, cada paso por HTTP costaría
> 10-50 ms — la precarga se disiparía y el rechazo de flotantes dejaría de funcionar.
> Tiene que vivir en el firmware.

### 7.1 El problema de los tiempos

Cuando la unidad A excita un canal, **todas** las unidades deben leer, y sus canales también
flotan. La precarga hay que aplicarla **en cada unidad**. Si el ciclo
`precarga → alta-Z → lectura` se reparte en varios mensajes de radio, el retardo acumulado
deja que la carga se disipe y la lectura se vuelve basura.

### 7.2 Diseño que lo elimina

El ciclo completo se ejecuta **dentro de cada unidad, de forma atómica**. Por la radio solo
viaja *cuándo empezar*, y eso es insensible al retardo: el que maneja **sostiene** el nivel
durante todo el muestreo, así que un desfase de milisegundos entre unidades no cambia nada.

```
Para cada canal fuente global G, y para cada nivel (ALTO, BAJO):

  1. master → unidad dueña de G :  DRIVE(G_local, nivel)     excita y MANTIENE
  2. master → broadcast         :  SAMPLE
        cada unidad, por su cuenta, sin radio de por medio:
          · todos sus canales → OUTPUT (nivel de precarga opuesto)
          · todos → INPUT   (salvo G si es suya, que sigue excitando)
          · settle
          · lee sus 32 canales → guarda en un buffer local
  3. master → cada unidad       :  COLLECT  → devuelve máscara de 32 bits
  4. master → todas             :  RELEASE  (estado de reposo seguro)

  Conectado(G, j) ⟺ máscara_ALTA[j] == 1  Y  máscara_BAJA[j] == 0
```

### 7.3 Mensajes nuevos

| Mensaje | Dirección | Payload |
|---|---|---|
| `LMSG_DRIVE`    | master → dueña | canal local (u8), nivel (u8) |
| `LMSG_SAMPLE`   | master → todas | fase (u8: 0=alta, 1=baja) |
| `LMSG_COLLECT`  | master → unidad | — |
| `LMSG_MASK`     | unidad → master | máscara de 32 bits (u32) |
| `LMSG_RELEASE`  | master → todas | — |
| `LMSG_LEDS`     | master → unidad | máscara de LEDs (u16), veredicto por rango |

### 7.4 Numeración global

```
canal_global = indice_unidad × 32 + canal_local
   indice 0 = master ·  1..N = hijos en el orden de peers[]
```

El master traduce a (unidad, canal local) para el `DRIVE` y al revés para el reporte.

> ⚠️ Cambiar el orden de los peers invalida un golden ya capturado. El software de PC debe
> guardar las MAC junto al golden y verificarlas, no fiarse de la posición (ver `API.md` §6).

### 7.5 Cambios de código — ✅ hechos en v0.7

| Qué | Dónde |
|---|---|
| `MAP_CH_LOCAL` 32 → `MAP_CH_MAX` 96 | `mapping.h` — `net_of[]` pasa a 96 bytes; union-find y comparación O(n²) = 9216 pares, trivial |
| Ciclo de muestreo local reutilizable | `mapping.cpp` — extraer de `mapRun()` lo que ya funciona |
| Manejadores de hijo | `link.cpp` — DRIVE/SAMPLE/COLLECT/RELEASE sobre sus propios U1/U2 |
| Orquestación | nuevo `mapdist.cpp` |

### 7.6 Riesgo y cómo verificarlo

El ciclo dentro de cada unidad es el mismo que ya funciona en local, así que el riesgo es bajo.
Aun así, la forma de comprobarlo es directa: **correr el mapeo distribuido y comparar los 32
canales del master contra lo que da `map` local** sobre el mismo arnés. Si coinciden, la parte
distribuida no introdujo error.

---

## 8. Pendiente de decidir

- [ ] Dónde vive el golden: NVS del master, o JSON subido por la web
- [ ] Varios goldens guardados (un arnés distinto por número de parte)
- [ ] El fixture soldado y el fixture de PCB dan mapas **distintos** — cada uno necesita su
      propio golden. Etiquetarlos para no compararlos cruzados
- [ ] Tiempo de settle: empezar en 500 µs y ajustar con medidas reales
