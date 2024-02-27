/*
    FlashInterface.h - Abstract class utilized by SPIFTL to manipulate flash

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

// Subclass this and implement your own flash (or DRAM for host-based debugging) accessors
class FlashInterface {
public:
    FlashInterface() { }
    virtual ~FlashInterface() { };

    // Need to know how large this flash device is.
    virtual int size() = 0;
    // What's a safe size to write (must be < 512b)
    virtual int writeBufferSize() = 0;

    // For emulation, preserve state between runs.  No-op on real hardware
    virtual void serialize() { }
    virtual void deserialize() { }

    // Returns a pointer to memory-level access to an EB
    virtual const uint8_t *readEB(int eb) = 0;

    virtual bool eraseBlock(int eb) = 0; // Erase an entire EB
    virtual bool program(int eb, int offset, const void *data, int size) = 0; // Program a small region of an EB.  Must support programming at `writeBufferSize()`
    virtual bool read(int eb, int offset, void *data, int size) = 0; // Read flash, guaranteed not to cross an EB
};
