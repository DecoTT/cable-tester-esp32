# Loopback Adapter — Cableado

## Variante A: Cross-connect (recomendada para bring-up)

Conectar CN1 ↔ CN2 de la siguiente forma:

```
CN1 Pin 1  (J00) ──── CN2 Pin 1  (J16)
CN1 Pin 2  (J01) ──── CN2 Pin 2  (J17)
CN1 Pin 3  (J02) ──── CN2 Pin 3  (J18)
CN1 Pin 4  (J03) ──── CN2 Pin 4  (J19)
CN1 Pin 5  (J04) ──── CN2 Pin 5  (J20)
CN1 Pin 6  (J05) ──── CN2 Pin 6  (J21)
CN1 Pin 7  (J06) ──── CN2 Pin 7  (J22)
CN1 Pin 8  (J07) ──── CN2 Pin 8  (J23)
CN1 Pin 9  (J08) ──── CN2 Pin 9  (J24)
CN1 Pin 10 (J09) ──── CN2 Pin 10 (J25)
CN1 Pin 11 (J10) ──── CN2 Pin 11 (J26)
CN1 Pin 12 (J11) ──── CN2 Pin 12 (J27)
CN1 Pin 13 (J12) ──── CN2 Pin 13 (J28)
CN1 Pin 14 (J13) ──── CN2 Pin 14 (J29)
CN1 Pin 15 (J14) ──── CN2 Pin 15 (J30)
CN1 Pin 16 (J15) ──── CN2 Pin 16 (J_SPARE)
```

**Pines 17-20 de CN1 y CN2:** NO conectar (auxiliares, pendiente definición).

## Cómo funciona con el firmware

- U1 (TCA9535 @ 0x20) maneja CN1 como **OUTPUT** (driver)
- U2 (TCA9535 @ 0x21) maneja CN2 como **INPUT** (reader)
- `loopback16` activa J00-J15 uno por uno y verifica que aparece en J16-J31

## Variante B: Same-pin (para arneses DUT reales)

```
CN1 J00 ──── CN2 J00
CN1 J01 ──── CN2 J01
...
```
Esta variante requiere que ambas mitades del conector estén asignadas al mismo número de línea. Depende de la definición del arnés bajo prueba.

## Materiales sugeridos para construir el adaptador

- 2× Conector JST B20B-PHDSS-B hembra (compatible con B20B-PHDSS-B(LF)(SN))
- Cable con 16 conductores
- Largo sugerido: 15-30 cm para facilitar manejo
