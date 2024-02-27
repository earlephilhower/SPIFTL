/*
    FlashInterfaceRAM.h - Host flash emulation for SPIFTL

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

#include "FlashInterface.h"

// DRAM simulation for host-based testing, NBD, etc.
class FlashInterfaceRAM : public FlashInterface {
public:
    FlashInterfaceRAM(int size) {
        _flashSize = size;
        _flash = new uint8_t[_flashSize];
        _isErased = new uint8_t[_flashSize / ebBytes];
        bzero(_isErased, _flashSize / ebBytes);
    }

    virtual ~FlashInterfaceRAM() override {
        delete[] _isErased;
        delete[] _flash;
    }

    virtual int size() override {
        return _flashSize;
    }

    virtual int writeBufferSize() override {
        return 128;
    }

    virtual const uint8_t *readEB(int eb) override {
        return &_flash[eb * ebBytes];
    }

    virtual void serialize() override {
        FILE *f = fopen("flash.bin", "wb");
        if (f) {
            fwrite(_flash, 1, _flashSize, f);
            fclose(f);
        }
    }

    virtual void deserialize() override {
        FILE *f = fopen("flash.bin", "rb");
        if (f) {
            if (fread(_flash, 1, _flashSize, f) != _flashSize) {
                bzero(_flash, _flashSize);
            }
            fclose(f);
        }
    }

    virtual bool eraseBlock(int eb) override {
        if (_isErased[eb]) {
            // Commenting out this check because the MD operations do an erase when changing epochs
            // If they don't erase, they could leave stale MD to be found on startup, wasting RAM and time
            // Testing w/FIO doesn't show any re-erases outside of the MD operations.
            //printf("ERROR: Erasing already erased block eb %d\n", eb);
        }
        _isErased[eb] = 1;
        if (eb < _flashSize / ebBytes) {
            bzero(&_flash[eb * ebBytes], ebBytes);
            return true;
        }
        return false;
    }

    virtual bool program(int eb, int offset, const void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
            _isErased[eb] = 0;
            memcpy(&_flash[eb * ebBytes + offset], data, size);
            return true;
        }
        return false;
    }

    virtual bool read(int eb, int offset, void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
            memcpy(data, &_flash[eb * ebBytes + offset], size);
            return true;
        }
        return false;
    }

private:
    const int ebBytes = 4096;
    int _flashSize;
    uint8_t *_flash;
    uint8_t *_isErased;
};
