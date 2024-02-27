/*
    Valgrind.cpp - C++ native tester for use w/valgrind

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

FlashInterfaceRAM fi(1 * 1024 * 1024);
SPIFTL ftl(&fi);
int flashLBAs;

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;

    int rv = 12345;
    if (argc == 2) {
        rv = atol(argv[1]);
    }
    printf("Starting FTL, random seed %d\n", rv);

    ftl.start();
    ftl.check();
    flashLBAs = ftl.lbaCount();

    uint8_t lba[512];
    bzero(lba, sizeof(lba));
    for (int i = 0; i < flashLBAs; i++) {
        sprintf((char *)lba, "lba %d", i);
        ftl.write(i, lba);
    }

    for (int i = 0; i < 50000; i++) {
        if ((i % 100) == 0) {
            ftl.trim(rand() % flashLBAs);
        } else {
            int x = rand() % (flashLBAs / 2);
            sprintf((char *)lba, "lba %d rewritten at %i", x, i);
            ftl.write(x, lba);
        }
        if (i % 1000 == 0) {
            printf("Write loop %d\n", i);
            ftl.check();
        }
    }
    ftl.persist();
    return 0;
}
