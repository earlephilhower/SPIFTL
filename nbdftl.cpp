/*
    NBDFTL - NBD driver for FTL testing on the host

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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <list>
#include <map>

#include "SPIFTL.h"
#include "FlashInterfaceRAM.h"

#define NBDKIT_API_VERSION 2
#include <nbdkit-plugin.h>
#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_ALL_REQUESTS

FlashInterfaceRAM fi(1 * 1024 * 1024);
SPIFTL ftl(&fi);

int flashLBAs;
uint8_t *lbaCopy;

static void ftl_load(void) {
    ftl.start();
    ftl.check();
    flashLBAs = ftl.lbaCount();
    lbaCopy = (uint8_t *)calloc(flashLBAs, 512);
    FILE *f = fopen("lba.bin", "rb");
    if (f) {
        fread(lbaCopy, 512, flashLBAs, f);
        fclose(f);
    }
    ftl.check();
}

static void ftl_close(void *handle) {
    ftl.persist();
    FILE *f = fopen("lba.bin", "wb");
    if (f) {
        fwrite(lbaCopy, 512, flashLBAs, f);
        fclose(f);
    }
}

static void *ftl_open(int readonly) {
    return NBDKIT_HANDLE_NOT_NEEDED;
}

static int64_t ftl_get_size(void *handle) {
    return (int64_t)flashLBAs * 512;
}

static int ftl_pwrite(void *handle, const void *buf, uint32_t count, uint64_t offset, uint32_t flags) {
    uint8_t *b = (uint8_t*)buf;
    while (count) {
        int lba = offset / 512;
        ftl.write(lba, b);
        memcpy(lbaCopy + lba * 512, b, 512);
        for (int i = 0; i < flashLBAs; i++) {
            uint8_t tmp[512];
            ftl.read(i, tmp);
            if (memcmp(tmp, lbaCopy + i * 512, 512)) {
                fprintf(stderr, "ERROR, lba mismatch %d\n", i);
            }
        }
        b += 512;
        offset += 512;
        count -= 512;
    }
    return 0;
}

static int ftl_pread(void *handle, void *buf, uint32_t count, uint64_t offset, uint32_t flags) {
    uint8_t *b = (uint8_t*)buf;
    while (count) {
        int lba = offset / 512;
        ftl.read(lba, b);
        offset += 512;
        count -= 512;
    }
    return 0;
}

static int ftl_block_size(void *handle, uint32_t *minimum, uint32_t *preferred, uint32_t *maximum) {
    *minimum = 512;
    *preferred = 512;
    *maximum = 512;
    return 0;
}

static int ftl_can_trim(void *handle) {
    return 1;
}

static int ftl_trim(void *handle, uint32_t count, uint64_t offset, uint32_t flags) {
    while (count) {
        int lba = offset / 512;
        ftl.trim(lba);
        offset += 512;
        count -= 512;
    }
    return 1;
}

static struct nbdkit_plugin plugin = {
    .name              = "spiftl",
    .version           = "1.0",
    .load              = ftl_load,
    .open              = ftl_open,
    .close             = ftl_close,
    .get_size          = ftl_get_size,

    .can_trim          = ftl_can_trim,

    .pread             = ftl_pread,
    .pwrite            = ftl_pwrite,
    .trim              = ftl_trim,
    .block_size        = ftl_block_size
};

NBDKIT_REGISTER_PLUGIN(plugin)
