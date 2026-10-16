// Arduino Wire implementation of the Melexis MLX90640 I2C driver interface
// (see MLX90640_I2C_Driver.h). The caller sets up Wire pins before use.
//
// The MLX90640 uses 16-bit register addresses and 16-bit big-endian words.
// The ESP32 Wire buffer is 128 bytes, so long reads (832 words for the
// EEPROM and frame RAM) are split into chunks.

#include <Arduino.h>
#include <Wire.h>
#include "MLX90640_I2C_Driver.h"

static const uint16_t CHUNK_WORDS = 32;  // 64 bytes per transfer

void MLX90640_I2CInit(void)
{
}

void MLX90640_I2CFreqSet(int freq)
{
    Wire.setClock(1000UL * freq);  // freq is in kHz, as in the Melexis drivers
}

int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data)
{
    while (nMemAddressRead > 0)
    {
        uint16_t words = nMemAddressRead < CHUNK_WORDS ? nMemAddressRead : CHUNK_WORDS;

        Wire.beginTransmission(slaveAddr);
        Wire.write(startAddress >> 8);
        Wire.write(startAddress & 0xFF);
        if (Wire.endTransmission(false) != 0)  // repeated start
        {
            return -1;
        }

        size_t bytes = words * 2;
        if (Wire.requestFrom(slaveAddr, bytes) != bytes)
        {
            return -1;
        }
        for (uint16_t i = 0; i < words; i++)
        {
            uint16_t hi = Wire.read();
            uint16_t lo = Wire.read();
            *data++ = (hi << 8) | lo;
        }

        startAddress += words;
        nMemAddressRead -= words;
    }
    return 0;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    Wire.beginTransmission(slaveAddr);
    Wire.write(writeAddress >> 8);
    Wire.write(writeAddress & 0xFF);
    Wire.write(data >> 8);
    Wire.write(data & 0xFF);
    if (Wire.endTransmission() != 0)
    {
        return -1;
    }

    // Read back like the reference drivers do. Status register writes
    // legitimately read back different, and the API ignores -2 there.
    uint16_t check;
    MLX90640_I2CRead(slaveAddr, writeAddress, 1, &check);
    return check == data ? 0 : -2;
}
