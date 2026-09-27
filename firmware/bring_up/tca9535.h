#pragma once
#include <Arduino.h>
#include <Wire.h>

// TCA9535 Register map
#define TCA9535_REG_INPUT0   0x00
#define TCA9535_REG_INPUT1   0x01
#define TCA9535_REG_OUTPUT0  0x02
#define TCA9535_REG_OUTPUT1  0x03
#define TCA9535_REG_POL0     0x04
#define TCA9535_REG_POL1     0x05
#define TCA9535_REG_CONFIG0  0x06   // 1=input, 0=output
#define TCA9535_REG_CONFIG1  0x07

class TCA9535 {
public:
    TCA9535(uint8_t addr);

    // Returns true if device ACKs on I2C
    bool begin();

    // Set pin directions. dir: bit=1 → input, bit=0 → output. Default after reset: all inputs.
    bool setDirection(uint16_t dir);

    // Write all 16 output pins at once
    bool writePort(uint16_t value);

    // Read all 16 input pins. Returns -1 on I2C error.
    int32_t readPort();

    // Drive single output pin (0-15)
    bool setPin(uint8_t pin, bool value);

    // Read single input pin (0-15). Returns -1 on error.
    int8_t readPin(uint8_t pin);

    uint8_t  addr;
    uint16_t _out_shadow;   // mirrors what was last written to output register

private:
    bool     _writeReg(uint8_t reg, uint8_t val);
    int16_t  _readReg(uint8_t reg);
};
