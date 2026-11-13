// Minimal GT911 capacitive touch reader over I2C: one finger, polled. The panels in this family
// wire the controller's INT pin either to a GPIO or not at all, so nothing here needs it.
#pragma once

#include <Arduino.h>
#include <Wire.h>

class Gt911 {
public:
    bool begin(int sda, int scl, int intPin, int rstPin) {
        _addr = 0;
        if (rstPin >= 0) {
            // Reset with INT low selects address 0x5D; probe both afterwards anyway.
            pinMode(rstPin, OUTPUT);
            if (intPin >= 0) {
                pinMode(intPin, OUTPUT);
                digitalWrite(intPin, LOW);
            }
            digitalWrite(rstPin, LOW);
            delay(10);
            digitalWrite(rstPin, HIGH);
            delay(10);
            if (intPin >= 0) pinMode(intPin, INPUT);
            delay(60);
        }
        Wire.begin(sda, scl, 400000);
        for (uint8_t a : {0x5D, 0x14}) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                _addr = a;
                break;
            }
        }
        return _addr != 0;
    }

    bool ok() const { return _addr != 0; }

    /// True while a finger is down; `x`/`y` are the controller's raw coordinates.
    bool read(int16_t &x, int16_t &y) {
        if (!_addr) return false;
        uint8_t status;
        if (!readRegs(0x814E, &status, 1)) return _down;
        if (!(status & 0x80)) return _down; // nothing new since the last read
        uint8_t n = status & 0x0F;
        if (n > 0) {
            uint8_t p[6];
            if (readRegs(0x8150, p, sizeof p)) {
                _x = p[0] | (p[1] << 8);
                _y = p[2] | (p[3] << 8);
                _down = true;
            }
        } else {
            _down = false;
        }
        uint8_t zero = 0;
        writeRegs(0x814E, &zero, 1);
        x = _x;
        y = _y;
        return _down;
    }

private:
    bool readRegs(uint16_t reg, uint8_t *buf, size_t n) {
        Wire.beginTransmission(_addr);
        Wire.write(reg >> 8);
        Wire.write(reg & 0xFF);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom((int)_addr, (int)n) != (int)n) return false;
        for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
        return true;
    }

    void writeRegs(uint16_t reg, const uint8_t *buf, size_t n) {
        Wire.beginTransmission(_addr);
        Wire.write(reg >> 8);
        Wire.write(reg & 0xFF);
        Wire.write(buf, n);
        Wire.endTransmission();
    }

    uint8_t _addr = 0;
    bool _down = false;
    int16_t _x = 0, _y = 0;
};
