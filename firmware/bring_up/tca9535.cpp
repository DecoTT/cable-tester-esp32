#include "tca9535.h"

TCA9535::TCA9535(uint8_t addr) : addr(addr), _out_shadow(0) {}

bool TCA9535::_writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

int16_t TCA9535::_readReg(uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    Wire.requestFrom(addr, (uint8_t)1);
    if (!Wire.available()) return -1;
    return (int16_t)(uint8_t)Wire.read();
}

bool TCA9535::begin() {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool TCA9535::setDirection(uint16_t dir) {
    bool ok = _writeReg(TCA9535_REG_CONFIG0, dir & 0xFF);
    ok     &= _writeReg(TCA9535_REG_CONFIG1, (dir >> 8) & 0xFF);
    return ok;
}

bool TCA9535::writePort(uint16_t value) {
    _out_shadow = value;
    bool ok = _writeReg(TCA9535_REG_OUTPUT0, value & 0xFF);
    ok      &= _writeReg(TCA9535_REG_OUTPUT1, (value >> 8) & 0xFF);
    return ok;
}

int32_t TCA9535::readPort() {
    int16_t lo = _readReg(TCA9535_REG_INPUT0);
    if (lo < 0) return -1;
    int16_t hi = _readReg(TCA9535_REG_INPUT1);
    if (hi < 0) return -1;
    return ((uint32_t)(uint8_t)hi << 8) | (uint8_t)lo;
}

bool TCA9535::setPin(uint8_t pin, bool value) {
    if (pin > 15) return false;
    if (value) _out_shadow |=  (1u << pin);
    else       _out_shadow &= ~(1u << pin);
    return writePort(_out_shadow);
}

int8_t TCA9535::readPin(uint8_t pin) {
    if (pin > 15) return -1;
    int32_t val = readPort();
    if (val < 0) return -1;
    return (val >> pin) & 1;
}
