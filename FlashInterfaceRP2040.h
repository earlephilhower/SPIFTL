/*
    FlashInterfaceRP2040.h - Flash interface for RP2040 arduino-pico

    Copyright (c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 3 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this program. If not, see https://www.gnu.org/licenses/
*/

#pragma once

#include <Arduino.h>
#include <hardware/flash.h>
#include "FlashInterface.h"


class FlashInterfaceRP2040 : public FlashInterface {
public:
    FlashInterfaceRP2040(const uint8_t *start, const uint8_t *end) {
        _flashSize = end - start;
        _flash = start;
    }

    virtual ~FlashInterfaceRP2040() override {
    }

    virtual int size() override {
        return _flashSize;
    }

    virtual int writeBufferSize() override {
        // Limitation of the SDK/HW, writes must be 256b or larger
        return 256;
    }

    virtual const uint8_t *readEB(int eb) override {
        return &_flash[eb * ebBytes];
    }

    virtual bool eraseBlock(int eb) override {
        if (eb < _flashSize / ebBytes) {
            const uint8_t *addr = _flash + (eb * ebBytes);
            noInterrupts();
            rp2040.idleOtherCore();
            flash_range_erase((intptr_t)addr - (intptr_t)XIP_BASE, ebBytes);
            rp2040.resumeOtherCore();
            interrupts();
            return true;
        }
        return false;
    }

    virtual bool program(int eb, int offset, const void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
            const uint8_t *addr = _flash + (eb * ebBytes + offset);
            noInterrupts();
            rp2040.idleOtherCore();
            flash_range_program((intptr_t)addr - (intptr_t)XIP_BASE, (const uint8_t *)data, size);
            rp2040.resumeOtherCore();
            interrupts();
            return true;
        }
        return false;
    }

    virtual bool read(int eb, int offset, void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
            memcpy(data, _flash + (eb * ebBytes + offset), size);
            return true;
        }
        return false;
    }

private:
    const int ebBytes = 4096;
    int _flashSize;
    const uint8_t *_flash;
};
