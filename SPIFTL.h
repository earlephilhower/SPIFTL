/*
    SPIFTL.h - Embedded, Static Wear-Leveling FTL Library

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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <bitset>
#include <string.h>
#include <cassert>
#include <list>
#include <map>

#include "FlashInterface.h"

#ifndef FTL_DEBUG
#define FTL_DEBUG 0
#endif


class SPIFTL {
public:
    SPIFTL(FlashInterface *fi) : _fi(fi) {
        flashBytes = fi->size();
        assert(flashBytes <= 16 * 1024 * 1024); // We assume 16MB or less flash space with certain bitfields
        eraseBlocks = flashBytes / ebBytes;
        int theoreticalLBAs = eraseBlocks * ebBytes / lbaBytes;
        metaEBBytes = /* peCount */ eraseBlocks + /* ebState */ (eraseBlocks + 1) / 2 + /* l2p */ (theoreticalLBAs * 2) + /* peCountOffset */ 4;
        metaEBs = 2 * (1 + metaEBBytes / (ebBytes - 64 /* header/footer/checksums */));
        flashLBAs = (eraseBlocks - 3 /* required for GC */ - metaEBs) * (ebBytes / lbaBytes);
        flashWriteBufferSize = fi->writeBufferSize();
        peCount = new uint8_t[eraseBlocks];
        ebState = new uint8_t[(eraseBlocks + 1) / 2];
        metaEBList = new int16_t[metaEBs];
        l2p = new L2P[flashLBAs];
    };

    ~SPIFTL() {
        delete[] l2p;
        delete[] metaEBList;
        delete[] ebState;
        delete[] peCount;
    }

    int lbaCount() {
        return flashLBAs;
    }

    bool format() {
#if FTL_DEBUG
        printf("formatting FTL\n");
#endif
        bzero(l2p, sizeof(L2P) * flashLBAs);
        bzero(peCount, sizeof(uint8_t) * eraseBlocks);
        bzero(ebState, sizeof(uint8_t) * ((eraseBlocks + 1) / 2));
        peCountOffset = 0;
        highestPECount = 0;
        emptyEBs = eraseBlocks;
        for (int i = 0; i < metaEBs; i++) {
            emptyEBs--;
            setEBMeta(i);
            metaEBList[i] = i;
        }
        metadataAge = 0;
        // Blow away anything that looks like old metadata!
        for (int i = 0; i < eraseBlocks; i++) {
            const uint8_t *eb = _fi->readEB(i);
            if (!memcmp(eb, metadataSig, 8)) {
#if FTL_DEBUG
                printf("format erasing eb %d\n", i);
#endif
                _fi->eraseBlock(i);
            }
        }
        return true;
    }

    bool check() {
        int max = 0;
        int min = 65536;
        int c = 0;
        int metas = 0;
        bool ret = true;
        for (int i = 0; i < eraseBlocks; i++) {
            c += !getEBState(i) ? 1 : 0;
            if (peCount[i] > max) {
                max = peCount[i];
            }
            if (min > peCount[i]) {
                min = peCount[i];
            }
            if (ebIsMeta(i)) {
                metas++;
            }
        }
        if (metas > metaEBs) {
            printf("ERROR: metas > metaEBs  %d > %d\n", metas, metaEBs);
            ret = false;
        }
        if (c != emptyEBs) {
            printf("ERROR: emptyEBs mismatch %d != %d\n", c, emptyEBs);
            ret = false;
        }
        if (max != highestPECount) {
            printf("ERROR: highestPECount mismatch %d != %d\n", max, highestPECount);
            ret = false;
        }
        if (max - min > maxPEDiff + 1) {
            printf("ERROR: maxPEDiff mismatch %d - %d    %d != %d\n", max, min, max - min, maxPEDiff);
            ret = false;
        }
        uint8_t val[eraseBlocks];
        bzero(val, sizeof(val));
        for (int i = 0; i < flashLBAs; i++) {
            if (l2p_val(i)) {
                auto eb = l2p_eb(i);
                auto idx = l2p_idx(i);
                if (ebIsMeta(eb)) {
                    printf("ERROR: LBA %d points to metadata\n", i);
                    ret = false;
                }
                if (val[eb] & 1 << idx) {
                    printf("ERROR: LBA %d crosslinked in eb %d idx %d\n", i, eb, idx);
                    ret = false;
                }
                val[eb] |= 1 << idx;
            }
        }
        return ret;
    }


    bool start() {
        _fi->deserialize();
        populateMetadataMap();
        if (loadHighestEpochMetadata()) {
#if FTL_DEBUG
            printf("restored metadata from flash\n");
#endif
            metadataAge = 0;
            return true;
        } else {
            return format();
        }
    }

    bool persist() {
        bool ret = doPersist();
        _fi->serialize();
        return ret;
    }

    bool write(int lba, const uint8_t *data) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false ;
        }
        if (openEB < 0) {
            openEB = selectBestEB();
        }
#if FTL_DEBUG
        printf("wrote %d to eb %d idx %d\n", lba, openEB, openEBNextIndex);
#endif
        if (!l2p_val(lba)) {
            validLBAs++;
        }

        _fi->program(openEB, openEBNextIndex * lbaBytes, data, lbaBytes);
        int oldEB, oldIndex;
        if (findLBA(lba, &oldEB, &oldIndex)) {
            clearLBAValid(oldEB);
            if (!getEBState(oldEB) && (oldEB != openEB)) {
                emptyEBs++;
            }
        }
        setLBAValid(openEB);
        setLBA(lba, openEB, openEBNextIndex);
        openEBNextIndex++;
        if (openEBNextIndex >= ebBytes / lbaBytes) {
            openEB = -1;
            openEBNextIndex = 0;
        }
        ageMetadata();
        return true;
    }

    bool read(int lba, uint8_t *dest) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false;
        }
        int oldEB, oldIndex;
        if (findLBA(lba, &oldEB, &oldIndex)) {
            _fi->read(oldEB, oldIndex * lbaBytes, dest, lbaBytes);
        } else {
            bzero(dest, lbaBytes);
        }
        return true;
    }

    bool trim(int lba) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false;
        }
        if (l2p_val(lba)) {
#if FTL_DEBUG
            printf("trim lba %d eb %d idx %d\n", lba, l2p_eb(lba), l2p_idx(lba));
#endif
            clearLBAValid(l2p_eb(lba));
            validLBAs--;
            if (!getEBState(l2p_eb(lba)) && (l2p_eb(lba) != openEB)) {
                emptyEBs++;
#if FTL_DEBUG
                printf("freeing eb %d\n", l2p_eb(lba));
#endif
            }
            l2p[lba] = 0; // invalid
            ageMetadata();
        }
        return true;
    }

    void dump() {
#if FTL_DEBUG
        printf("Erase Blocks (maxpe=%d, peCountOffset=%d, emptyEBs=%d, validLBAs=%d)\n", highestPECount, peCountOffset, emptyEBs, validLBAs);
        printf("MetaEBList: ");
        for (int i = 0; i < metaEBs; i++) {
            printf("%d ", metaEBList[i]);
        }
        printf("\n");
        for (int i = 0; i < eraseBlocks; i++) {
            printf("  EB%02d: pe=%d ebState=%01X meta=%d gcscore=%d\n", i, peCount[i], getEBState(i), ebIsMeta(i) ? 1 : 0, gcScore(i));
        }
#endif
    }

    const int ebBytes = 4096;
    const int lbaBytes = 512;
    const int maxPEDiff = 64;

private:
    FlashInterface *_fi;

    int flashBytes;
    int eraseBlocks;
    int metaEBBytes;
    int metaEBs;
    int flashLBAs;
    int flashWriteBufferSize;

    typedef struct {
        uint16_t ebBytes;
        uint16_t lbaBytes;
        uint32_t flashBytes;
        uint16_t metaEBBytes;
        uint16_t flashLBAs;
    } FTLInfo;

    uint8_t *peCount; // We'll just track up to 250, and when we hit 251 we will subtract maxPEDiff from them all
    // ebState: 0 = free, 1...8 = # of LBAs valid, 9..0xe = undefined, 0xf = meta
    const unsigned int ebMeta = 0x0f;
    uint8_t *ebState;
    int16_t *metaEBList;

    unsigned int peCountOffset;
    int highestPECount;
    int emptyEBs;
    int validLBAs;
    uint8_t metadataAge;

    // L2P format.  Can't use bitfields since GCC will make every element 32-bits
    //typedef struct {
    //    unsigned eb  : 12;
    //    unsigned idx : 3;
    //    unsigned val : 1;
    //} L2P;
    typedef uint16_t L2P;
    L2P *l2p;

    int openEB = -1; // EB currently being written.  < 0 == none open
    int openEBNextIndex = 0; // Which LBA w/in that EBA should be written next

    // ---- L2P AND ERASE BLOCK MANAGEMENT

    inline void setEBState(int eb, unsigned int state) {
        int idx = eb / 2;
        if (eb & 1) {
            ebState[idx] = (ebState[idx] & 0x0f) | (state << 4);
        } else {
            ebState[idx] = (ebState[idx] & 0xf0) | state;
        }
    }

    inline unsigned int getEBState(int eb) {
        return 0x0f & (ebState[eb / 2] >> ((eb & 1) ? 4 : 0));
    }

    inline bool ebIsMeta(int eb) {
        return getEBState(eb) == ebMeta;
    }

    inline void setEBMeta(int eb) {
        setEBState(eb, ebMeta);
    }

    inline uint16_t l2p_eb(int lba) {
        return l2p[lba] & ((1 << 12) - 1);
    }

    inline uint8_t l2p_idx(int lba) {
        return (l2p[lba] >> 12) & ((1 << 3) - 1);
    }

    inline bool l2p_val(int lba) {
        return l2p[lba] & 1 << 15;
    }

    inline L2P make_l2p(int idx, int eb) {
        L2P t = 1 << 15;
        t |= idx << 12;
        t |= eb;
        return t;
    }

    inline void setLBAValid(int eb) {
        setEBState(eb, getEBState(eb) + 1);
    }

    inline void clearLBAValid(int eb) {
        setEBState(eb, getEBState(eb) - 1);
    }

    bool findLBA(int lba, int *eb, int *idx) {
        if (l2p_val(lba)) {
            *eb = l2p_eb(lba);
            *idx = l2p_idx(lba);
            return true;
        } else {
            return false;
        }
    }

    inline void setLBA(int lba, int eb, int idx) {
        l2p[lba] = make_l2p(idx, eb);
    }


    // ---- METADATA FORMAT AND PERSISTENCE

    // Metadata EB format
    // 8 byte header:   <signature0..7>
    // 3 byte epoch:    <e><e><e> = 2^23 cycles, way beyond flash lifetime
    // 1 byte index:    <i> = Block within this metadata serialization, since more than one EB needed
    // 4080 byte:       <d>...<d> = packed metadata
    // 4 byte checksum: <c><c><c><c> = CRC32 over bytes 0...4091

    // Metadata packed format
    // ftlInfo:peCountArray:l2pArray:peCountOffset:highestPECount:emptyEBs:validLBAs

    class MetadataCRC32 {
    public:
        MetadataCRC32() {
            crc = 0xffffffff;
        }

        ~MetadataCRC32() {
        }

        inline void add(uint8_t x) {
            add(&x, 1);
        }

        void add(const void *d, uint32_t len) {
            const uint8_t *data = (const uint8_t *)d;
            for (uint32_t i = 0; i < len; i++) {
                crc ^= data[i];
                for (int j = 0; j < 8; j++) {
                    if (crc & 1) {
                        crc = (crc >> 1) ^ 0xedb88320;
                    } else {
                        crc >>= 1;
                    }
                }
            }
        }

        uint32_t get() {
            return ~crc;
        }

        void reset() {
            crc = 0xffffffff;
        }

    private:
        uint32_t crc;
    };


    const char metadataSig[8] = {'S', 'P', 'I', 'F', 'T', 'L', '0', '1'};
    std::list<int> metadataEBList;
    int metadataEBoffset;
    uint8_t metadataEBindex;
    MetadataCRC32 metadataCRC;
    uint32_t metadataEpoch = 2; // epoch 0 and 1 are part of formatting on flash, all empty

    void openMetadataStreamForWrite() {
#if FTL_DEBUG
        printf("Serializing metadata epoch %d\n", (int)metadataEpoch + 1);
#endif
        metadataEBList.clear();
        for (int j = 0; j < metaEBs; j++) {
            int i = metaEBList[j];
            if (i < 0) {
                continue;
            }
            const uint8_t *eb = _fi->readEB(i);
            metadataCRC.reset();
            metadataCRC.add(eb, 4096 - 4);
            uint32_t crc = metadataCRC.get();
            bool err = memcmp(&crc, eb + 4096 - 4, 4);
            uint32_t mde = *(uint32_t*)(_fi->readEB(i) + 8) >> 8;
#if FTL_DEBUG
            printf("metaEBList[%d] = %d, epoch %d, err %d\n", j, i, (int)mde, err);
#endif
            if (err || (mde < metadataEpoch)) {
                if (!err) {
                    // Need to erase the MD in this block or we can end up with a large number of old MD blocks, wasting time and memory during FTL bringup
                    _fi->eraseBlock(i);
                }
                setEBState(i, 0);
                metaEBList[j] = -1;
                emptyEBs++;
#if FTL_DEBUG
                printf("Free %d\n", i);
#endif
            }
        }

        for (int i = 0; i < metaEBs; i++) {
            if (metaEBList[i] >= 0) {
                continue;
            }
            int eb = lowestEmptyEB();
            metadataEBList.push_back(eb);
#if FTL_DEBUG
            printf("Allocating %d\n ", eb);
#endif
            setEBMeta(eb);
            metaEBList[i] = eb;
            emptyEBs--;
        }
    
        metadataEpoch++;
        metadataEBindex = 0;
        metadataEBoffset = 0;
        metadataCRC.reset();
    }
    
    inline void writeMetadata8b(uint8_t b, char *wb) {
        if (metadataEBoffset == 4096 - 4) {
            uint32_t crc = metadataCRC.get();
            memcpy(&wb[flashWriteBufferSize - 4], &crc, 4);
            _fi->program(metadataEBList.front(), 4096 - flashWriteBufferSize, wb, flashWriteBufferSize);
            metadataEBList.pop_front();
            metadataCRC.reset();
            metadataEBoffset = 0;
            metadataEBindex++;
        }
        if (metadataEBoffset == 0) {
            bzero(wb, flashWriteBufferSize);
            memcpy(wb, metadataSig, 8);
            metadataCRC.add(metadataSig, 8);
            uint32_t ne = (metadataEpoch << 8) | metadataEBindex;
            memcpy(wb + 8, &ne, 4);
            metadataCRC.add(&ne, 4);
            metadataEBoffset = 12;
        }
        wb[metadataEBoffset % flashWriteBufferSize] = b;
        metadataCRC.add(b);
        metadataEBoffset++;
        if (0 == metadataEBoffset % flashWriteBufferSize) {
            if (metadataEBoffset == flashWriteBufferSize) {
                eraseEB(metadataEBList.front());
                setEBMeta(metadataEBList.front());
            }
            _fi->program(metadataEBList.front(), metadataEBoffset - flashWriteBufferSize, wb, flashWriteBufferSize);
            bzero(wb, flashWriteBufferSize);
        }
    }

    inline void writeMetadata16b(uint16_t t, char *wb) {
        writeMetadata8b((uint8_t)(t >> 8), wb);
        writeMetadata8b((uint8_t)(t & 0xff), wb);
    }

    inline void writeMetadata32b(uint32_t t, char *wb) {
        writeMetadata8b((uint8_t)(t >> 24), wb);
        writeMetadata8b((uint8_t)(t >> 16), wb);
        writeMetadata8b((uint8_t)(t >> 8), wb);
        writeMetadata8b((uint8_t)(t & 0xff), wb);
    }

    void closeMetadataStream(char *wb) {
        // We be lazy, just 0-pad until index loops (taking into account header size)
        while (metadataEBoffset > 13) {
            writeMetadata8b((uint8_t)0, wb);
        }
    }

    bool doPersist() {
        char wb[flashWriteBufferSize]; // Keep on stack to avoid needing to malloc() from inside persist
    
        openMetadataStreamForWrite(); // Will increment epoch, choose oldest MD copy to overwrite

        // Dump FTLInfo
        FTLInfo f = {.ebBytes = (uint16_t)ebBytes, .lbaBytes = (uint16_t)lbaBytes, .flashBytes = (uint32_t)flashBytes, .metaEBBytes = (uint16_t)metaEBBytes, .flashLBAs = (uint16_t)flashLBAs};
        uint8_t *p = (uint8_t*)&f;
        for (size_t i = 0; i < sizeof(f); i++) {
            writeMetadata8b(p[i], wb);
        }

        // Dump peCount
        for (int i = 0; i < eraseBlocks; i++) {
            writeMetadata8b(peCount[i], wb);
        }

        // Dump ebState
        for (int i = 0; i < (eraseBlocks + 1) / 2; i++) {
            writeMetadata8b(ebState[i], wb);
        }

        // Dump L2P
        uint16_t *q = (uint16_t*)(l2p);
        for (int i = 0; i < flashLBAs; i++) {
            writeMetadata16b(q[i], wb);
        }

        // peCountOffset
        writeMetadata32b(peCountOffset, wb);

        closeMetadataStream(wb); // Will 0-fill and add checksum at end

        return true;
    }


    std::map<uint32_t /* epoch */, std::list<int> /* EBs that contain that epoch */> metadataMap;
    const uint8_t *mdOpenEB;

    void populateMetadataMap() {
#if FTL_DEBUG
        printf("populateMetadataMap()\n");
#endif
        metadataMap.clear();
        for (int i = 0; i < eraseBlocks; i++) {
            const uint8_t *eb = _fi->readEB(i);
            if (!memcmp(eb, metadataSig, 8)) {
                metadataCRC.reset();
                metadataCRC.add(eb, 4096 - 4);
                uint32_t crc = metadataCRC.get();
                if (!memcmp(&crc, eb + 4096 - 4, 4)) {
                    uint32_t epoch = *(const uint32_t *)&eb[8];
#if FTL_DEBUG
                    printf("Found MD epoch %d, idx %d at eb %d\n", (int)(epoch >> 8), (int)(epoch & 0xff), i);
#endif
                    auto l = metadataMap.find(epoch >> 8);
                    if (l != metadataMap.end()) {
                        l->second.push_back(i);
                    } else {
                        std::list<int> n;
                        n.push_back(i);
                        metadataMap.insert({epoch >> 8, n});
                    }
                } else {
#if FTL_DEBUG
                    printf("Found header but got CRC mismatch  EB %d\n", i);
#endif
                }
            }
        }
#if FTL_DEBUG
        for (auto x = metadataMap.begin(); x != metadataMap.end(); x++) {
            printf("epoch %d: ", (int)x->first);
            for (auto y = x->second.begin(); y != x->second.end(); y++) {
                printf("%d ", *y);
            }
            printf("\n");
        }
#endif
    }


    void openMetadataStreamForRead() {
        metadataEBoffset = 0;
        mdOpenEB = _fi->readEB(metadataEBList.front());
    }

    inline uint8_t readMetadata8b() {
        if (metadataEBoffset >= 4096 - 4) {
            metadataEBoffset = 0;
            metadataEBList.pop_front();
            mdOpenEB = _fi->readEB(metadataEBList.front());
        }
        if (metadataEBoffset < 12) {
            metadataEBoffset = 12;
        }
        return mdOpenEB[metadataEBoffset++];
    }

    inline uint16_t readMetadata16b() {
        return (readMetadata8b() << 8) | readMetadata8b();
    }

    inline uint32_t readMetadata32b() {
        return  (readMetadata8b() << 24) | (readMetadata8b() << 16) | (readMetadata8b() << 8) | readMetadata8b();
    }

    bool doLoadHighestEpochMetadata() {
        uint32_t epoch = 0; // Should never be higher than anything on flash
        for (auto x = metadataMap.begin(); x != metadataMap.end(); x++) {
            if (x->first > epoch) {
                epoch = x->first;
            }
        }
        if (!epoch) {
            return false;
        }
#if FTL_DEBUG
        printf("Loading epoch %d from ", (int)epoch);
#endif
        metadataEBList.clear();
        auto ebs = metadataMap.find(epoch)->second;
        uint32_t epochidx = epoch << 8;
        for (int i = 0; i < metaEBBytes; i++) {
            for (auto x = ebs.begin(); x != ebs.end(); x++) {
                auto r = _fi->readEB(*x);
                if (*(uint32_t*)&r[8] == epochidx) {
                    metadataEBList.push_back(*x);
#if FTL_DEBUG
                    printf("%d ", *x);
#endif
                    break;
                }
            }
            epochidx++;
        }
#if FTL_DEBUG
        printf("\n");
#endif
        metadataMap.erase(epoch); // If this doesn't pass muster, then don't check it again
        openMetadataStreamForRead();

        // Dump FTLInfo
        FTLInfo f = {.ebBytes = (uint16_t)ebBytes, .lbaBytes = (uint16_t)lbaBytes, .flashBytes = (uint32_t)flashBytes, .metaEBBytes = (uint16_t)metaEBBytes, .flashLBAs = (uint16_t)flashLBAs};
        FTLInfo onFlash;
        uint8_t *p = (uint8_t*)&onFlash;
        for (size_t i = 0; i < sizeof(f); i++) {
            p[i] = readMetadata8b();
        }
        if (memcmp(&f, &onFlash, sizeof(f))) {
#if FTL_DEBUG
            printf("ERROR: FTL info doesn't match, skipping\n");
#endif
            return false;
        }

        // At this point, we blindly pull everything out. CRCs already verified
        highestPECount = 0;
        for (int i = 0; i < eraseBlocks; i++) {
            peCount[i] = readMetadata8b();
            if (peCount[i] > highestPECount) {
                highestPECount = peCount[i];
            }
        }

        // Clear existing meta EB list
        for (int i = 0; i < metaEBs; i++) {
            metaEBList[i] = -1;
        }
        emptyEBs = 0;
        for (int i = 0, j = 0; i < (eraseBlocks + 1) / 2; i++) {
            ebState[i] = readMetadata8b();
            // Restore metaEBList as we read in
            if (ebIsMeta(i * 2)) {
                metaEBList[j++] = i * 2;
            }
            if (ebIsMeta(i * 2 + 1)) {
                metaEBList[j++] = i * 2 + 1;
            }
            if (getEBState(i * 2) == 0) {
                emptyEBs++;
            }
            if (getEBState(i * 2 + 1) == 0) {
                emptyEBs++;
            }
        }

        validLBAs = 0;
        uint16_t *q = (uint16_t*)(l2p);
        for (int i = 0; i < flashLBAs; i++) {
            q[i] = readMetadata16b();
            if (l2p_val(i)) {
                validLBAs++;
            }
        }

        peCountOffset = readMetadata32b();

        // Nothing to close, this is a read operation only
        metadataEpoch = epoch;
        return true;
    }

    bool loadHighestEpochMetadata() {
        while (metadataMap.begin() != metadataMap.end()) {
            if (doLoadHighestEpochMetadata()) {
                metadataMap.clear();
                return true;
            }
        }
        metadataMap.clear();
        return false;
    }

    bool doCheck() {
        int max = 0;
        int min = 65536;
        int c = 0;
        int metas = 0;
        bool pass = true;
        for (int i = 0; i < eraseBlocks; i++) {
            c += !getEBState(i) ? 1 : 0;
            if (peCount[i] > max) {
                max = peCount[i];
            }
            if (min > peCount[i]) {
                min = peCount[i];
            }
            if (ebIsMeta(i)) {
                metas++;
            }
        }
        if (metas > metaEBs) {
#if FTL_DEBUG
            printf("ERROR: metas > metaEBs  %d > %d\n", metas, metaEBs);
#endif
            pass = false;
        }
        if (c != emptyEBs) {
#if FTL_DEBUG
            printf("ERROR: emptyEBs mismatch %d != %d\n", c, emptyEBs);
#endif
            pass = false;
        }
        if (max != highestPECount) {
#if FTL_DEBUG
            printf("ERROR: highestPECount mismatch %d != %d\n", max, highestPECount);
#endif
            pass = false;
        }
        if (max - min > maxPEDiff + 1) {
#if FTL_DEBUG
            printf("ERROR: maxPEDiff mismatch %d - %d    %d != %d\n", max, min, max - min, maxPEDiff);
#endif
            pass = false;
        }
        uint8_t val[eraseBlocks];
        bzero(val, sizeof(val));
        for (int i = 0; i < flashLBAs; i++) {
            if (l2p_val(i)) {
                auto eb = l2p_eb(i);
                auto idx = l2p_idx(i);
                if (ebIsMeta(eb)) {
#if FTL_DEBUG
                    printf("ERROR: LBA %d points to metadata\n", i);
#endif
                    pass = false;
                }
                if (val[eb] & 1 << idx) {
#if FTL_DEBUG
                    printf("ERROR: LBA %d crosslinked in eb %d idx %d\n", i, eb, idx);
#endif
                    pass = false;
                }
                val[eb] |= 1 << idx;
            }
        }
        return pass;
    }

    void eraseEB(int eb) {
#if FTL_DEBUG
        printf("EraseEB(%d)\n", eb);
#endif
        _fi->eraseBlock(eb);
        if (peCount[eb] > 250) {
            for (int i = 0; i < eraseBlocks; i++) {
                if (peCount[i] > maxPEDiff) {
                    peCount[i] -= maxPEDiff;
                } else {
#if FTL_DEBUG
                    printf("ERROR: underflow pecount reset eb %d - maxPEDiff %d\n", peCount[i], maxPEDiff);
#endif
                    peCount[i] = 0;
                }
            }
            highestPECount -= maxPEDiff;
            peCountOffset += maxPEDiff;
        }
        peCount[eb]++;
        if (peCount[eb] > highestPECount) {
            highestPECount = peCount[eb];
        }

        setEBState(eb, 0);
    }


    // ----- GARBAGE COLLECTION AND WEAR LEVELING
    inline int highestEmptyEB() {
        int highestEmptyPE = -1;
        int highestEmptyIdx = 0;
        for (int i = 0; i < eraseBlocks; i++) {
            if ((peCount[i] > highestEmptyPE) && (getEBState(i) == 0)) {
                highestEmptyPE = peCount[i];
                highestEmptyIdx = i;
            }
        }
        return highestEmptyIdx;
    }

    inline int lowestEmptyEB() {
        int lowestEmptyPE = 1 << 16; // 1 more than highest possible PECOUNT
        int lowestEmptyIdx = -1;
        for (int i = 0; i < eraseBlocks; i++) {
            if ((peCount[i] <= lowestEmptyPE) && (getEBState(i) == 0)) {
                lowestEmptyPE = peCount[i];
                lowestEmptyIdx = i;
            }
        }
        return lowestEmptyIdx;
    }

    void dumpMetadataEBs() {
#if FTL_DEBUG
        for (int i = 0; i < eraseBlocks; i++) {
            if (ebIsMeta(i)) {
                printf("MDEB %d: ", i);
                auto z = _fi->readEB(i);
                for (int j = 0; j < ebBytes; j++) {
                    printf("%02X ", z[j]);
                }
                printf("\n");
            }
        }
#endif
    }

    void ageMetadata() {
        if (++metadataAge == 0) {
            // Every 256 writes we
            persist();
            metaAgeRewrite();
        }
    }

    // Assumes the destEB is available to write date and has no gaps in its valid bits
    // Really ugly but w/o a reverse P2L map not sure how to get this otherwise
    int collectValidLBAs(int srcEB, int destEB, int destIdx) {
        int curIdx = destIdx;
        const uint8_t *readAddr = _fi->readEB(srcEB);
        for (int i = 0; (i < flashLBAs) && (curIdx < 8); i++) {
            if ((l2p_eb(i) == srcEB) && l2p_val(i)) {
#if FTL_DEBUG
                printf("moving lba%02d to eb%d idx%d\n", i, destEB, curIdx);
#endif
                uint8_t buff[flashWriteBufferSize];
                for (int j = 0; j < lbaBytes; j += sizeof(buff)) {
                    memcpy(buff, readAddr + 512 * l2p_idx(i) + j, sizeof(buff));
                    _fi->program(destEB, 512 * curIdx + j, buff, sizeof(buff));
                }
                clearLBAValid(srcEB);
                if (getEBState(srcEB) == 0) {
                    emptyEBs++;
                }
                l2p[i] = make_l2p(curIdx, destEB);
                setEBState(destEB, getEBState(destEB) + 1);
                curIdx++;
            }
        }
        return curIdx;
    }
    
    inline int gcScore(int eb) {
        unsigned int state = getEBState(eb);
        if ((state == ebMeta) || !state) {
            return 0;
        }
        int delta = highestPECount - peCount[eb];
        if (delta >= maxPEDiff) {
            return 10 + delta - maxPEDiff; // Aged out, choose oldest
        }
        if (delta > ((maxPEDiff * 7 ) / 8)) {
            return 9; // Getting old, try to move before timeout
        }
        return 8 - state;
    }

    int garbageCollect() {
        int ebScore = 0;
        int destEB = lowestEmptyEB(); // We'll write data into the youngest flash
        assert(destEB >= 0);
        eraseEB(destEB);
        emptyEBs--;
        for (int cnt = 0; (getEBState(destEB) < 8 ) && (cnt < 8); cnt++) {  // Loop until full or at most 8 times since we should have at least 1 move per cycle
            static int eb = 0; // The current EB to GC, we'll start at the last eb checked and loop around
            // Find first non-meta EB
            while (ebIsMeta(eb) || (eb == destEB)) {
                eb = (eb + 1) % eraseBlocks;
            }
            ebScore = gcScore(eb);
            for (int i = 1; (i < eraseBlocks) && (ebScore < 8); i++) {
                int ebMod = (eb + i) % eraseBlocks;
                if ((ebScore < gcScore(ebMod)) && (ebMod != destEB)) {
                    eb = ebMod;
                    ebScore = gcScore(eb);
                }
            }
            assert(ebScore > 0); // ERROR, couldn't find anything...we're toast
            assert(eb != destEB);
            setEBState(destEB, collectValidLBAs(eb, destEB, getEBState(destEB)));
        }
        return ebScore;
    }

    // Check all metadata EBs for age-out and rewrite if necessary
    void metaAgeRewrite() {
        for (int i = 0; i < metaEBs; i++) {
            int eb = metaEBList[i];
            if (eb < 0) {
                continue;
            }
            if (highestPECount - peCount[eb] >= maxPEDiff) {
                int destEB = lowestEmptyEB(); // We'll write data into the youngest flash
#if FTL_DEBUG
                printf("Aged-out metadata %d to %d\n", eb, destEB);
#endif
                assert(destEB >= 0);
                assert(destEB != eb);
                eraseEB(destEB);
                const uint8_t *readAddr = _fi->readEB(eb);
                uint8_t buff[flashWriteBufferSize];
                for (int i = 0; i < ebBytes; i += sizeof(buff)) {
                    memcpy(buff, readAddr + i, sizeof(buff));
                    _fi->program(destEB, i, buff, sizeof(buff));
                }
                setEBState(eb, 0);
                setEBMeta(destEB);
                metaEBList[i] = destEB;
            }
        }
    }

    int selectBestEB() {
        int ebScore = 0;
        // We need 3 EBs minimum to be free, and any score > 10 means we need to move for PE count wear leveling
        while ((emptyEBs < 3) || (ebScore > 10)) {
            ebScore = garbageCollect();
            metaAgeRewrite();
        }
        emptyEBs--;
        int eb = lowestEmptyEB();
#if FTL_DEBUG
        printf("selectBestEB() = %d\n", eb);
#endif
        eraseEB(eb);
        return eb;
    }


};
