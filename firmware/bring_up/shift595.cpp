#include "shift595.h"

Shift595::Shift595(uint8_t ser, uint8_t srclk, uint8_t rclk)
    : _ser(ser), _srclk(srclk), _rclk(rclk), _shadow(0) {}

void Shift595::begin() {
    pinMode(_ser,   OUTPUT);
    pinMode(_srclk, OUTPUT);
    pinMode(_rclk,  OUTPUT);
    digitalWrite(_ser,   LOW);
    digitalWrite(_srclk, LOW);
    digitalWrite(_rclk,  LOW);
    // OE# soldado a GND en PCB — no hay pin de habilitacion.
    clear();
}

void Shift595::write(uint16_t value) {
    _shadow = value;
    // Shiftear MSB primero (bit 15 → bit 0)
    // bit 15 termina en U4-QH, bit 0 termina en U3-QA
    for (int8_t i = 15; i >= 0; i--) {
        digitalWrite(_ser, (value >> i) & 1);
        digitalWrite(_srclk, HIGH);
        delayMicroseconds(1);
        digitalWrite(_srclk, LOW);
        delayMicroseconds(1);
    }
    // Latch al registro de salida
    digitalWrite(_rclk, HIGH);
    delayMicroseconds(1);
    digitalWrite(_rclk, LOW);
}

void Shift595::clear() {
    write(MUX_A_EN_MASK | MUX_B_EN_MASK);  // MUX_EN# HIGH = deshabilitados, LEDs off
}

void Shift595::setLeds(uint16_t mask) {
    write(_shadow | mask);
}

void Shift595::clearLeds(uint16_t mask) {
    write(_shadow & ~mask);
}

void Shift595::setAllLeds(bool on) {
    // Máscara de todos los LEDs (bits 0-9)
    const uint16_t ALL_LEDS = SR_LED1_RED | SR_LED2_RED | SR_LED3_RED |
                               SR_LED4_RED | SR_LED5_RED | SR_LED1_GREEN |
                               SR_LED2_GREEN | SR_LED3_GREEN | SR_LED4_GREEN |
                               SR_LED5_GREEN;
    if (on)
        write(_shadow | ALL_LEDS);
    else
        write(_shadow & ~ALL_LEDS);
}

void Shift595::selectMux(uint8_t ch, int mux_unit) {
    // Limpiar bits de MUX en shadow
    uint16_t base = _shadow;
    base &= ~(MUX_BITS(0xF) | MUX_A_EN_MASK | MUX_B_EN_MASK);

    if (mux_unit < 0) {
        // Deshabilitar ambos
        base |= MUX_A_EN_MASK | MUX_B_EN_MASK;
    } else {
        base |= MUX_BITS(ch & 0xF);
        if (mux_unit == 0) {
            base |= MUX_B_EN_MASK;  // U6 deshabilitado
            // U5 habilitado: MUX_A_EN# = 0
        } else {
            base |= MUX_A_EN_MASK;  // U5 deshabilitado
            // U6 habilitado: MUX_B_EN# = 0
        }
    }
    write(base);
}
