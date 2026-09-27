#pragma once
#include <Arduino.h>
#include "config.h"

// Driver para 2x 74HC595 en daisy-chain (16 bits total).
// U3 (primer chip, recibe LED_DATA) = bits 0-7
// U4 (segundo chip, recibe de U3 QH') = bits 8-15
//
// OE# de U3/U4 está soldado a GND en la PCB → salidas siempre activas.
// El shadow mantiene el estado completo (LEDs + MUX select) entre escrituras,
// evitando que un setLed() borre la selección de MUX activa y viceversa.
class Shift595 {
public:
    Shift595(uint8_t ser, uint8_t srclk, uint8_t rclk);

    void begin();

    // Escribe los 16 bits completos con latch
    void write(uint16_t value);

    // Fuerza todo a 0 (LEDs off, MUX disabled)
    void clear();

    // --- LED helpers ---
    // mask: uno o más bits SR_LEDx_RED / SR_LEDx_GREEN (OR de los que quieras)
    void setLeds(uint16_t mask);          // enciende esos LEDs (preserva MUX)
    void clearLeds(uint16_t mask);        // apaga esos LEDs (preserva MUX)
    void setAllLeds(bool on);             // todos los LEDs ON u OFF (preserva MUX)

    // --- MUX helpers ---
    // Selecciona canal ch (0-15) en U5 (mux_unit=0) o U6 (mux_unit=1)
    // Preserva el estado actual de LEDs.
    // Para deshabilitar ambos MUX: selectMux(0, -1)
    void selectMux(uint8_t ch, int mux_unit);  // mux_unit: 0=U5, 1=U6, -1=disable ambos

    // Estado actual del registro
    uint16_t shadow() const { return _shadow; }

private:
    uint8_t  _ser, _srclk, _rclk;
    uint16_t _shadow;
};
