# Enlace de dos unidades — Diseño

**Estado:** **implementado en firmware v0.5** — verificación parcial (falta 2ª unidad)  
**Fecha:** 2026-08-03  
**Costo en flash:** +17.7 KB (1.10 → 1.12 MB, 85% de la partición)

> El módulo `link.cpp/h` está completo: descubrimiento, emparejamiento cifrado, persistencia
> en NVS y empuje de credenciales. Lo que **no** se ha podido probar es cualquier cosa que
> requiera dos radios. Ver §11 para qué está verificado y qué no.

---

## 1. Decisiones tomadas

| Tema | Decisión | Razón |
|---|---|---|
| Transporte de prueba | **ESP-NOW** | MAC permanente (no depende de SSID), ~10 KB de flash, funciona sin router |
| Transporte de UI/OTA | **WiFi + HTTP** | Ya existe; permite OTA también en el slave |
| BLE | **Descartado** | NimBLE ≈ 100-150 KB; el sketch ya usa 84% (1.10/1.25 MB) |
| Enlace cableado (IO4/IO5) | **Descartado** | LINK_TX/RX tienen header físico pero **no están ruteados** en Rev 1.0/1.1 |
| Roles | **En runtime**, no compilados | Cualquier unidad puede ser master o slave |
| Credenciales WiFi | **El master se las empuja al slave** | Configurar la red una sola vez |
| Modo de líneas | **16P y 32P seleccionables** | Elegido por el usuario |

> IO4 e IO5 quedan **libres** para uso futuro, ya que el enlace cableado se descartó.

---

## 2. Por qué ESP-NOW y no "todo por WiFi"

Con ambas unidades en la misma red, el master podría hablarle al slave por HTTP y no haría
falta ESP-NOW. Se rechazó porque:

- El tester va a vivir en **planta**, donde la red puede no existir o ser inestable.
- ESP-NOW no necesita router, DHCP ni descubrimiento de servicios.
- Latencia ~2-5 ms contra ~10-50 ms de HTTP sobre WiFi.

ESP-NOW y WiFi **no compiten**: ESP-NOW lleva el control de la prueba, WiFi lleva la UI y el OTA.

---

## 3. La restricción del canal

ESP-NOW exige que ambos equipos estén en el **mismo canal WiFi**. Es la única complejidad real.

**En operación normal desaparece:** si ambas unidades tienen credenciales y se conectan al mismo
router, quedan automáticamente en el mismo canal. Ésta es la razón de peso —más allá de la
comodidad— para que el master empuje las credenciales al slave.

**Camino de respaldo (sin router):** el slave salta canales 1→13 emitiendo un paquete de
descubrimiento hasta que el master responde. ~500 ms por canal ⇒ barrido completo en ~7 s.
Esto lo vuelve inmune a que el router cambie de canal por su cuenta.

---

## 4. Máquina de roles (en arranque)

```
Arranque
  │
  ├─ ¿Credenciales WiFi en NVS?
  │    SÍ → conecta
  │         ├─ OK    → MASTER: web UI + responde ESP-NOW en su canal
  │         └─ falla → abre portal captivo (SSID con sufijo de MAC)
  │
  └─ NO → ¿Peer guardado en NVS?
           SÍ → SLAVE: salta canales buscando al master (~10 s)
           │      ├─ lo encuentra → enlazado; espera órdenes y credenciales
           │      └─ no aparece   → sigue escuchando. **NO abre portal.**
           │                         Portal solo si arrancas con BTN_START presionado.
           NO → abre portal captivo (comportamiento actual)
```

### 4.1 Por qué el slave NO debe abrir portal

Si la red WiFi cambia, **ninguna** de las dos unidades conecta. Con la lógica ingenua las dos
levantarían un AP y tendrías dos portales — exactamente el problema que se quiere evitar.

Un equipo que sabe que es slave (tiene peer guardado) debe **esperar credenciales nuevas por
ESP-NOW**, no competir con un AP propio. El usuario configura la red una vez, en el master.

Escape manual: arrancar con `BTN_START` presionado fuerza el portal, para recuperar una unidad
cuyo master se perdió definitivamente.

---

## 5. Emparejamiento

1. En el master (ya en WiFi): botón **Modo emparejamiento** en la web → ventana de 60 s
2. Se enciende la unidad a emparejar (sin credenciales) → emite descubrimiento saltando canales
3. Master responde con su MAC; ambos guardan la MAC del otro en NVS
4. Se establece peer **cifrado** (ver §6)
5. El master envía SSID + contraseña al slave
6. El slave guarda, conecta al router y queda en el mismo canal del master

Persistente: el enlace sobrevive reinicios y cortes. Se deshace con `link_reset`
(espejo de `wifi_reset`).

### 5.1 Cambio de red WiFi

1. Cambia la red → el master no conecta → abre su portal
2. Usuario mete la red nueva **solo en el master**
3. El master conecta y **reenvía las credenciales al peer guardado**
4. El slave —que estaba escuchando, no compitiendo con un AP— las recibe y conecta

El slave debe seguir atendiendo actualizaciones de credenciales **incluso ya conectado**, para
que el cambio se pueda empujar de forma proactiva.

---

## 6. Seguridad — limitación conocida

ESP-NOW va **sin cifrar por defecto**. Mandar la contraseña WiFi en claro por el aire no es
aceptable en planta.

Mitigación: descubrimiento en claro (no lleva secretos) → se establece peer cifrado AES-128 →
recién entonces viajan las credenciales.

> ⚠️ **Límite honesto:** la clave precompartida vive dentro del firmware. Cualquiera con el `.bin`
> la extrae. Esto detiene el *sniffing* casual, **no** a un atacante decidido.
>
> Es proporcionado para una herramienta de banco en red propia. **Si esto entra a la red
> corporativa de un cliente, hay que replantearlo** (clave por dispositivo, emparejamiento con
> PIN mostrado en la UI del master, o transferencia solo por USB).

---

## 7. Modos de línea — 16P y 32P

| Modo | Conectores por unidad | Líneas | Mapeo distribuido |
|---|---|---|---|
| 16P | Solo CN1 | 16 | J(i) master ↔ J(i) slave, i=0..15 |
| 32P | CN1 + CN2 | 32 | J(i) master ↔ J(i) slave, i=0..31 |

**Ojo — el mapeo distribuido NO es el del autotest local.** El autotest local usa un arnés en
bucle CN1↔CN2 y por eso mapea `J(i) ↔ J(i+16)`. En el test distribuido el arnés va de una unidad
a la otra y el índice es **el mismo en ambos extremos**. Son dos tablas distintas; no reutilizar
la lógica sin adaptarla.

En 16P, CN2 queda libre en ambas unidades — se puede dejar el arnés de bucle puesto para correr
el autotest local sin desconectar nada.

Selección: comando `mode 16p` / `mode 32p`, persistido en NVS, visible en la web.

---

## 8. Protocolo de prueba distribuida

Petición/respuesta simple, no sincronización por tiempo. Robustez sobre velocidad:

```
master → slave : SET_INPUTS
master         : maneja línea i
master → slave : READ_PORT
slave  → master: máscara de 32 bits
```

32 líneas × 2 sentidos = 64 viajes redondos × ~3 ms ≈ **200 ms**. De sobra: la prueba completa
queda muy por debajo de un segundo, y no depende de que los relojes vayan sincronizados.

Payload máximo de ESP-NOW: 250 bytes. Una máscara de 32 bits es trivial.

---

## 9. Comandos

| Comando | Descripción |
|---|---|
| `link_info` | Rol, MAC propia, canal, peer y si está en línea |
| `link_pair` | **Master:** abre 60 s de emparejamiento |
| `link_push_wifi` | Reenvía el WiFi actual al peer (tras cambiar de red) |
| `link_ping` | Prueba de vida del peer |
| `link_reset` | Deshace el enlace |

También como botones en la web. El botón amarillo **🔗 EMPAREJAR 2a unidad** es `link_pair`.

## 10. Indicadores RGB en el arranque

| Color | Significado |
|---|---|
| Pulso verde | Conectó a WiFi → MASTER |
| Magenta parpadeando | Unidad virgen buscando master en modo emparejamiento (15 s) |
| Magenta fijo | SLAVE enlazado, esperando credenciales. **No abre AP** |
| Azul fijo | Portal captivo activo |

## 11. Estado de verificación

### Verificado con una sola unidad
- [x] Compila; +17.7 KB, 85% de la partición de app
- [x] SSID de portal con sufijo de MAC (`CableTester32-A1B2`)
- [x] `wifiUnitId()` — identificador corto de unidad

### Escrito pero SIN VERIFICAR — requiere 2ª unidad
- [ ] Emparejamiento real (DISCOVER → OFFER → ACCEPT)
- [ ] Persistencia del peer entre reinicios
- [ ] Empuje de credenciales y reinicio automático del slave
- [ ] Peer cifrado AES-128 (¿acepta `esp_now_add_peer` con LMK en ambos lados?)
- [ ] Salto de canales: ¿encuentra al master dentro de la ventana de 15 s?
- [ ] Modo slave en espera sin levantar AP

> Nada de la segunda lista se ha ejecutado nunca. Al probar con la 2ª unidad, hacerlo por
> partes y en este orden: primero `link_info` en ambas, luego emparejamiento, luego
> credenciales. Depurar los tres a la vez es lo que hay que evitar.

## 12. Pendiente de diseño

- [ ] Protocolo de prueba distribuida (§8) — mensajes definidos, sin implementar
- [ ] Modo 16P/32P seleccionable
- [ ] Decidir si el master proxea la UI del slave o cada uno sirve la suya
