#pragma once

// =============================================================
// config.h — Cable Tester 32CH — Bring-up v0.2
// Target: ESP32-C3-MINI-1-N4 en módulo SBC Rev1.1
// Pinout CONFIRMADO desde esquemático EasyEDA Rev 1.0 (2026-03-02)
// =============================================================

// -------------------------------------------------------------
// I2C Bus — confirmado desde esquemático (R36/R37 = 4.7kΩ pull-ups a 3V3)
// NOTA: IO8=SCL comparte con LED RGB WS2812B del módulo ESP.
//       IO9=SDA comparte con BOOT button (pull-up 10k en módulo).
//       Ambos son strapping pins pero con pull-ups correctos no hay problema.
// -------------------------------------------------------------
#define PIN_SDA         9   // I2C_SDA — R37 4.7kΩ pull-up a 3V3
#define PIN_SCL         8   // I2C_SCL — R36 4.7kΩ pull-up a 3V3

// -------------------------------------------------------------
// 74HC595 — U3+U4 daisy-chain (16 bits)
// U3 G# (OE#) está soldado a GND en PCB — salidas siempre activas.
// NO hay pin GPIO para OE#.
// -------------------------------------------------------------
#define PIN_595_SER     0   // LED_DATA  → U3 SI
#define PIN_595_SRCLK   1   // LED_CLK   → U3 SCK
#define PIN_595_RCLK    2   // LED_LATCH → U3 RCK (y U4 RCK)
// OE# = 255 indica "no conectado" — está fijo a GND en la PCB
#define PIN_595_OE      255

// -------------------------------------------------------------
// ADC Scope — IO3 a través de R47 (1kΩ) + C10 (22pF filtro)
// La señal viene de ADC_MUX_OUT (salida común de U5/U6 CD74HC4067)
// IO3 = ADC1_CH3, libre de strapping.
// -------------------------------------------------------------
#define PIN_ADC_SCOPE   3

// -------------------------------------------------------------
// UART inter-módulo (Fase 3 — reservado, no implementado)
// -------------------------------------------------------------
#define PIN_LINK_TX     5   // LINK_TX
#define PIN_LINK_RX     4   // LINK_RX

// -------------------------------------------------------------
// Botón de inicio
// -------------------------------------------------------------
#define PIN_BTN_START   10  // BTN_START — pull-up externo TBD, leer con INPUT_PULLUP

// -------------------------------------------------------------
// LED RGB WS2812B en módulo ESP32 SBC Rev1.1
// IO8 comparte con PIN_SCL — usar ANTES de Wire.begin()
// -------------------------------------------------------------
#define PIN_RGB_LED     8
#define CT_RGB_BRIGHTNESS  8   // 0-255 — muy suave, no ciega

// -------------------------------------------------------------
// TCA9535 I2C Addresses — CORREGIDO 2026-09-26
//
// Del esquematico: la direccion es 0100 A2 A1 A0.
//   U1: A2=GND A1=GND A0=GND -> 0x20, y lleva L00-L15 (CN1)
//   U2: A2=GND A1=GND A0=3V3 -> 0x21, y lleva L16-L30 + SPARE (CN2)
//
// Estaba AL REVES. El comentario anterior decia "VERIFICADO en PCB fisica",
// pero lo unico que se habia comprobado es que scan_i2c encuentra 0x20 y 0x21
// — no CUAL es cual. El firmware llamaba J00 a lo que fisicamente es J16.
//
// POR QUE NO LO DETECTO NINGUNA PRUEBA: es perfectamente simetrico. Con el
// bucle CN1<->CN2, "J00<->J16" y "J16<->J00" son el MISMO net. Con dos placas
// iguales el cruce tambien sale coherente. El golden funcionaba y las piezas
// malas se rechazaban bien; lo unico erroneo era a que pin fisico apuntaba el
// informe — justo lo que el operador necesita.
//
// COMPROBADO CON MULTIMETRO (2026-09-26): excitando el canal 0 del firmware,
// CN1 pin 1 = 0.18 V y CN2 pin 1 = 3.30 V. Inequivoco.
//
// Y esto explica los enables del MUX: no estaban intercambiados en la placa.
// Se "corrigieron" empiricamente para compensar ESTE error, y los dos fallos
// se cancelaban. Al arreglar las direcciones hay que devolver los enables a lo
// que dice el esquematico.
// -------------------------------------------------------------
#define TCA9535_U1_ADDR  0x20   // L00-L15  -> J00-J15  (CN1)
#define TCA9535_U2_ADDR  0x21   // L16-L30 + SPARE -> J16-J_SP (CN2)

// La cadena es 1 a 1 y sin sorpresas, confirmado en el esquematico:
//   TCA9535 Pxy  ->  Lnn  ->  680 ohm  ->  Jnn  ->  pin del conector
//   U1: P00..P07 = L00..L07 ; P10..P17 = L08..L15
//   U2: P00..P07 = L16..L23 ; P10..P16 = L24..L30 ; P17 = SPARE
// -------------------------------------------------------------

#define CH_COUNT_U1   16
#define CH_COUNT_U2   16
#define CH_COUNT_ALL  32

// Canal de J_SPARE dentro de U2
#define J_SPARE_BIT   15  // bit 15 de U2 = P17 = J_SPARE

// -------------------------------------------------------------
// Shift Register 16-bit — bit layout
//
// CORREGIDO contra el esquematico (2026-09-26).
//
// El mapa anterior decia "bits 0-4 los cinco rojos, 5-9 los cinco verdes" y
// estaba MAL: los LEDs van INTERCALADOS verde/rojo por canal, y el canal 5 vive
// en el segundo chip. Pedir "todos los rojos" encendia C1_Green, C1_Red,
// C2_Green, C2_Red, C3_Green — una mezcla, que es justo lo que se observo.
//
// El comentario viejo decia "VERIFICADO visualmente con led_test": esa
// verificacion no valia, porque encendiendolos uno a uno en secuencia no se
// distingue el orden real.
//
// U3 (bits 0-7, QA=bit0 ... QH=bit7):
//   bit 0  = QA = C1_Green
//   bit 1  = QB = C1_Red
//   bit 2  = QC = C2_Green
//   bit 3  = QD = C2_Red
//   bit 4  = QE = C3_Green
//   bit 5  = QF = C3_Red
//   bit 6  = QG = C4_Green
//   bit 7  = QH = C4_Red
//
// U4 (bits 8-15, QA=bit8 ... QH=bit15):
//   bit 8  = QA = C5_Green
//   bit 9  = QB = C5_Red
//   bit 10 = QC = MUX_S0
//   bit 11 = QD = MUX_S1
//   bit 12 = QE = MUX_S2
//   bit 13 = QF = MUX_S3
//   bit 14 = QG = MUX_A_EN#
//   bit 15 = QH = MUX_B_EN#
//
// Los bits de MUX SI eran correctos: las medidas analogicas dependen de ellos y
// salieron bien, lo que confirma que el error estaba solo en los LEDs.
// -------------------------------------------------------------

// LEDs individuales (bit en el registro de 16 bits)
#define SR_LED1_GREEN   (1u << 0)
#define SR_LED1_RED     (1u << 1)
#define SR_LED2_GREEN   (1u << 2)
#define SR_LED2_RED     (1u << 3)
#define SR_LED3_GREEN   (1u << 4)
#define SR_LED3_RED     (1u << 5)
#define SR_LED4_GREEN   (1u << 6)
#define SR_LED4_RED     (1u << 7)
#define SR_LED5_GREEN   (1u << 8)
#define SR_LED5_RED     (1u << 9)

// Mascaras utiles: todos verdes = pieza buena, todos rojos = error grave.
#define SR_ALL_GREEN    (SR_LED1_GREEN | SR_LED2_GREEN | SR_LED3_GREEN | SR_LED4_GREEN | SR_LED5_GREEN)
#define SR_ALL_RED      (SR_LED1_RED | SR_LED2_RED | SR_LED3_RED | SR_LED4_RED | SR_LED5_RED)

// Rojo/verde del rango r (0-4), para construir la mascara por zonas.
#define SR_RED_OF(r)    (1u << (2 * (r) + 1))
#define SR_GREEN_OF(r)  (1u << (2 * (r)))

#define LED_COUNT       10   // 5 rojo + 5 verde

// Posiciones de bits MUX en shift register
#define SR_MUX_S0_BIT   10
#define SR_MUX_S1_BIT   11
#define SR_MUX_S2_BIT   12
#define SR_MUX_S3_BIT   13

// Enables del MUX — segun esquematico (U4 QG = bit 14, QH = bit 15).
//
// Estuvieron intercambiados en el firmware desde 2026-09-21, "verificado" con
// rtest. Aquella observacion era real, pero la causa no: lo que estaba mal eran
// las DIRECCIONES de los expansores (ver arriba). Al invertir los enables se
// compensaba el error y el conjunto quedaba auto-consistente, con todos los
// nombres de canal desplazados 16 posiciones.
//
// Corregida la causa, los enables vuelven a su sitio.
#define SR_MUX_A_EN_BIT 14   // MUX_A_EN# -> habilita U5 (CN1, J00-J15)
#define SR_MUX_B_EN_BIT 15   // MUX_B_EN# -> habilita U6 (CN2, J16-J_SP)

// Helpers para construir valor con MUX seleccionado
// ch: canal 0-15, unit: 0=U5 (CN1 J00-J15), 1=U6 (CN2 J16-J31)
// Resultado: valor 16-bit para escribir en shift register (preservando LEDs)
#define MUX_BITS(ch) ( \
    (((ch) & 1) ? (1u << SR_MUX_S0_BIT) : 0) | \
    (((ch) & 2) ? (1u << SR_MUX_S1_BIT) : 0) | \
    (((ch) & 4) ? (1u << SR_MUX_S2_BIT) : 0) | \
    (((ch) & 8) ? (1u << SR_MUX_S3_BIT) : 0) )

#define MUX_A_EN_MASK   (1u << SR_MUX_A_EN_BIT)  // 1 = disable (active LOW)
#define MUX_B_EN_MASK   (1u << SR_MUX_B_EN_BIT)  // 1 = disable (active LOW)

// -------------------------------------------------------------
// Medición analógica / resistencia de contacto
// R4-R35 = 680Ω en serie por línea, en AMBOS extremos (CN1 y CN2).
// Divisor: 3V3 ─[680]─ J(i) ─[cable]─ J(i+16) ─[680]─ GND
// Nominal con cable ideal: 1.65 V en ambos lados.
// Sensibilidad ≈ 1.2 mV por ohm (≈1.5 LSB) → resolución de pocos ohms.
// -------------------------------------------------------------
#define R_SERIES_OHM      680.0f
#define ADC_VREF          3.3f
#define ADC_FULL_SCALE    4095.0f
#define ADC_AVG_SAMPLES   64

// Umbrales de diagnóstico de resistencia de contacto
#define RC_OK_OHM         5.0f    // conexión sana
#define RC_WARN_OHM       50.0f   // sospechosa: crimp flojo / soldadura fría

// Error de ganancia del ADC del ESP32-C3 (sin esp_adc_cal).
// Medido 2026-08-03: el divisor 680/680 da 1.650 V teóricos y el ADC reportó
// 1.747 V de forma consistente en las 16 líneas → +5.88%.
// Solo afecta medidas ABSOLUTAS; rtest es diferencial y se cancela solo.
#define ADC_GAIN_ERR      1.0588f

// Por encima de este voltaje (ya corregido) la línea se considera sin fuga.
// Una línea sana en alta-Z llega al riel; el ADC satura en 3.3 V → 3.12 V
// tras corregir ganancia, así que 3.0 V deja margen cómodo.
#define LEAK_OK_VOLT      3.00f

// -------------------------------------------------------------
// Enlace ESP-NOW entre dos unidades (ver docs/LINK_DESIGN.md)
// -------------------------------------------------------------
// Cuánto escucha una unidad virgen buscando un master en modo emparejamiento
// antes de rendirse y abrir su propio portal captivo.
// Un barrido completo son 13 × 600 ms ≈ 8 s, así que esto da ~3 barridos:
// tres oportunidades de coincidir con el canal del master, no una.
#define LINK_DISCOVERY_MS   25000

// Duración de la ventana de emparejamiento que abre el master
#define LINK_PAIR_WINDOW_MS 60000

// -------------------------------------------------------------
// Versión de firmware y de la API HTTP
// -------------------------------------------------------------
#define FW_VERSION   "1.7.1"

// Versión del contrato JSON de /api/*. Se incrementa SOLO cuando cambia de
// forma incompatible: el software de PC debe comprobarla al conectarse.
// Añadir campos nuevos NO la incrementa.
#define API_VERSION  1

// Serial
#define SERIAL_BAUD  115200
