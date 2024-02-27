# SPIFTL - Embedded, Static Wear-Leveling FTL Library
(c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

This library implements a static wear-leveling FTL algorithm suitable for
use on embedded systems with SPI flash.  Using static wear leveling should
help extend the useful life of flash memory, especially when combined with
the FAT filesystem which has certain high-write usage areas such as the FAT
tables and directory entries.

There were three design goals:
* Preserve data
* Keep the flash array within a limited PE (program/erase) cycle range
* Miimize the memory at the cost of write speed

While the process used here is similar in concept to what a modern SSD does,
this is definitely not a general purpose SSD FTL layer.  It is missing things
like bad block handling, parallelism, short-circuit paths, data retention
scans and rewrite, and much more.  It is also limited to 16MB of flash and
erase pages of 4KB for memory and expediency considerations.

An implementation for the Arduino-Pico RP2040 core as well as a NBD
(Network Block Device) plugin is included.  Porting to other architectures
should only require developing a small FlashInterface subclass.

This software is provided on an AS-IS basis and no comes with no warranties.
See LICENSE.md for the full GNU LESSER GENERAL PUBLIC LICENSE.
