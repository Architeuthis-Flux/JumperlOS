/*
    FatFS.cpp - file system wrapper for FatFS
    Copyright (c) 2024 Earle F. Philhower, III. All rights reserved.

    Based on spiffs_api.cpp which is:
    | Copyright (c) 2015 Ivan Grokhotkov. All rights reserved.

    This code was influenced by NodeMCU and Sming libraries, and first version of
    Arduino wrapper written by Hristo Gochkov.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "FatFS.h"
#include <FS.h>
//#define FTL_DEBUG 1
#include "../lib/SPIFTL/FlashInterfaceRP2040.h"
#include "../lib/SPIFTL/SPIFTL.h"

// JumperlOS: enable the SPIFTL delta-journal by default. With it on, persist()
// (called on every CTRL_SYNC / f_close) appends one already-erased flash page
// instead of rewriting the whole ~16 KB metadata snapshot, so a save is
// ~sub-millisecond AND immediately power-loss durable - no lazy deferral
// needed. The journal reserves a small region and changes the partition
// geometry, so the first boot after enabling reformats the FS partition once.
// Define FATFS_SPIFTL_JOURNAL=0 at build time to keep upstream full-snapshot
// persist behavior.
#ifndef FATFS_SPIFTL_JOURNAL
#define FATFS_SPIFTL_JOURNAL 1
#endif

using namespace fs;


#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_FATFS)
extern uint8_t _FS_start;
extern uint8_t _FS_end;

FS FatFS = FS(FSImplPtr(new fatfs::FatFSImpl()));
static FlashInterfaceRP2040 *_fi = new FlashInterfaceRP2040(&_FS_start, &_FS_end);
static SPIFTL *_ftl = nullptr;
uint16_t _sectorSize = 512;

// Set non-zero to log per-CTRL_SYNC FTL persist timing (microseconds) and
// whether each persist was a fast journal append or a full snapshot. Toggled
// from JumperlOS (see fatFsSetTimingDebug / the debug command).
extern "C" volatile int spiftl_timing_debug = 0;

// JumperlOS lazy-persist hooks. These thunk into the file-static _ftl
// instance so callers don't need to deal with C++ namespace lookups
// for a private static. See lib/FatFS/JL_PATCH.md and the SPIFTL fork
// for what setLazyPersist / forceSync mean.
//
// fatFsSetLazyPersist(true) flips persist() (called from
// disk_ioctl(CTRL_SYNC) on every f_close) into a no-op so individual
// saves don't pay the ~750 ms metadata-serialize cost.
// fatFsForceSync() is the explicit "really persist now" call the
// embedder makes during idle / slot switch / pre-USB-MSC-mount /
// shutdown to coalesce many lazy writes into one persist.
//
// Both are no-ops if _ftl is null (i.e. setUseFTL(false) was selected),
// which is correct - without the FTL, FatFS writes go directly to flash
// every f_close and there's no metadata to defer.
extern "C" void fatFsSetLazyPersist(bool enable) {
    if (_ftl) {
        _ftl->setLazyPersist(enable);
    }
}

extern "C" bool fatFsForceSync(void) {
    if (_ftl) {
        return _ftl->forceSync();
    }
    return true;
}

// Toggle the SPIFTL delta-journal at runtime. Can only ENABLE if the _ftl was
// constructed with journaling reserved (FATFS_SPIFTL_JOURNAL != 0); otherwise
// enabling is a no-op (the geometry/dirty-tracking weren't allocated). No-op if
// _ftl is null (setUseFTL(false)). See lib/FatFS/JL_PATCH.md.
extern "C" void fatFsSetJournal(bool enable) {
    if (_ftl) {
        _ftl->setJournal(enable);
    }
}

extern "C" bool fatFsIsJournal(void) {
    return _ftl ? _ftl->isJournal() : false;
}

// Enable/disable per-persist FTL timing logs (see spiftl_timing_debug).
extern "C" void fatFsSetTimingDebug(bool enable) {
    spiftl_timing_debug = enable ? 1 : 0;
}
#endif

namespace fatfs {

// Set once disk_initialize() has run _ftl->start() for the CURRENT _ftl
// instance. Must be cleared whenever a new SPIFTL is constructed, or an
// end()/setUseFTL()/begin() re-toggle would use an un-start()ed FTL
// (uninitialized l2p/peCount).
static bool started = false;

bool FatFSImpl::begin() {
    if (_mounted) {
        return true;
    }

    if (_cfg._useFTL) {
        if (!_ftl) {
            _ftl = new SPIFTL(_fi, FATFS_SPIFTL_JOURNAL);
            started = false; // fresh instance, disk_initialize() must start() it
        }
        _sectorSize = 512;
    } else {
        _sectorSize = 4096;
        if (_ftl) {
            delete _ftl;
            _ftl = nullptr;
        }
    }

    _mounted = (FR_OK == f_mount(&_fatfs, "", 1));
    if (!_mounted && _cfg._autoFormat) {
        format();
        _mounted = (FR_OK == f_mount(&_fatfs, "", 1));
    }
    //FsDateTime::setCallback(dateTimeCB); TODO = callback
    return _mounted;
}

void FatFSImpl::end() {
    if (_mounted) {
        f_unmount("");
    }
    sync();
    _mounted = false;
}


// Required to be global because SDFAT doesn't allow a this pointer in it's own time call
time_t (*__fatfs_timeCallback)(void) = nullptr;

FileImplPtr FatFSImpl::open(const char* path, OpenMode openMode, AccessMode accessMode) {
    if (!_mounted) {
        DEBUGV("FatFSImpl::open() called on unmounted FS\n");
        return FileImplPtr();
    }
    if (!path || !path[0]) {
        DEBUGV("FatFSImpl::open() called with invalid filename\n");
        return FileImplPtr();
    }
    BYTE flags = _getFlags(openMode, accessMode);
    if ((openMode & OM_CREATE) && strchr(path, '/')) {
        // For file creation, silently make subdirs as needed.  If any fail,
        // it will be caught by the real file open later on
        char *pathStr = strdup(path);
        if (pathStr) {
            // Make dirs up to the final fnamepart
            char *ptr = strrchr(pathStr, '/');
            if (ptr && ptr != pathStr) { // Don't try to make root dir!
                *ptr = 0;
                f_mkdir(pathStr);
            }
        }
        free(pathStr);
    }
    auto sharedFd = std::make_shared<FIL>();
    if (FR_OK == f_open(sharedFd.get(), path, flags)) {
        if ((openMode & OM_TRUNCATE) && !(openMode & OM_CREATE)) {
            // No FA_ flag maps to truncate-without-create (see _getFlags), and
            // FA_CREATE_ALWAYS already truncated the OM_CREATE case.
            f_truncate(sharedFd.get());
        }
        return std::make_shared<FatFSFileImpl>(this, sharedFd, path, FA_WRITE & flags ? true : false);
    }
    sharedFd = nullptr;
    DEBUGV("FatFSImpl::openFile: path=`%s` openMode=%d accessMode=%d FAILED", path, openMode, accessMode);
    return FileImplPtr();
}

DirImplPtr FatFSImpl::openDir(const char* path) {
    if (!_mounted) {
        return DirImplPtr();
    }
    char *pathStr = strdup(path); // Allow edits on our scratch copy
    if (!pathStr) {
        // OOM
        return DirImplPtr();
    }
    // Get rid of any trailing slashes
    while (strlen(pathStr) && (pathStr[strlen(pathStr) - 1] == '/')) {
        pathStr[strlen(pathStr) - 1] = 0;
    }
    // At this point we have a name of "/blah/blah/blah" or "blah" or "".
    // If it's an existing dir, open it; otherwise open the containing dir and
    // use the final name part as a prefix filter.
    // The FatFSDirImpl ctor below does the one and only f_opendir(dirPath).
    // (Opening here too would clobber that DIR and, with FF_FS_LOCK, leak a
    // share-table slot per openDir on a subdirectory.)
    FILINFO fno;
    const char *filter = "";
    const char *dirPath = pathStr; // "" opens the root dir
    if (pathStr[0] && !((FR_OK == f_stat(pathStr, &fno)) && (fno.fattrib & AM_DIR))) {
        // A file or a nonexistent name, not a dir
        char *ptr = strrchr(pathStr, '/');
        if (!ptr) {
            // No slashes: filter within the root dir. filter aliases pathStr,
            // so point dirPath at root separately instead of truncating.
            dirPath = "";
            filter = pathStr;
        } else {
            // We've got slashes, open the dir one up
            *ptr = 0; // Remove slash, truncate string
            filter = ptr + 1;
        }
    }
    auto sharedDir = std::make_shared<DIR>(); // value-init: zeroed obj.fs marks it invalid until the ctor opens it
    auto ret = std::make_shared<FatFSDirImpl>(filter, this, sharedDir, dirPath);
    free(pathStr);
    return ret;
}

bool FatFSImpl::format() {
    if (_mounted) {
        return false;
    }
    BYTE *work = new BYTE[4096]; /* Work area (larger is better for processing time) */
    // MKFS_PARM is {fmt, n_fat, align, n_root, au_size}. Upstream's positional
    // initializer was field-scrambled: _sectorSize landed in n_root and
    // _dirEntries in au_size. Values below keep the geometry every shipped
    // device was formatted with: n_root = 512 (what the scrambled code
    // effectively passed; _dirEntries was never honored - au_size=128 *bytes*
    // rounds down to 0 sectors in f_mkfs, i.e. auto cluster size).
    MKFS_PARM opt = { FM_FAT | FM_SFD, _cfg._fatCopies, 1 /*align*/, 512 /*n_root*/, 0 /*au_size: auto*/ };
    auto ret = f_mkfs("", &opt, work, 4096);
    delete[] work;

    return ret == FR_OK;
}

DSTATUS disk_status(BYTE p) {
    (void) p;
    return 0;
}

void disk_format() {
    if (!started && _ftl) {
        _ftl->format();
    }
}

DSTATUS disk_initialize(BYTE p) {
    (void) p;
    if (!started) {
        if (_ftl) {
            _ftl->start();
        }
        started = true;
    }
    return 0;
}

DRESULT disk_read(BYTE p, BYTE *buff, LBA_t sect, UINT count) {
    (void) p;
    for (unsigned int i = 0; i < count; i++) {
        if (_ftl) {
            _ftl->read(sect + i, buff + i * _sectorSize);
        } else {
            _fi->read(sect + i, 0, buff + i * _sectorSize, _sectorSize);
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    (void) pdrv;
    for (unsigned int i = 0; i < count; i++) {
        if (_ftl) {
            _ftl->write(sector + i, buff + i * _sectorSize);
        } else {
            _fi->eraseBlock(sector + i);
            _fi->program(sector + i, 0, buff + i * _sectorSize, _sectorSize);
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    (void) pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        if (_ftl) {
            if (spiftl_timing_debug) {
                uint32_t e0 = _ftl->debugEpoch();
                int r0 = _ftl->debugJournalRecords();
                uint32_t t0 = micros();
                _ftl->persist();
                uint32_t dt = micros() - t0;
                // Classify: epoch bump => full snapshot, record bump => fast
                // journal append, neither => nothing was dirty (no-op).
                const char* kind = (_ftl->debugEpoch() != e0) ? "snapshot"
                                 : (_ftl->debugJournalRecords() != r0) ? "journal-append"
                                 : "noop";
                Serial.printf("[SPIFTL] persist %lu us  %s (journal=%d epoch=%lu recs=%d)\n",
                              (unsigned long)dt, kind, (int)_ftl->isJournal(),
                              (unsigned long)_ftl->debugEpoch(), _ftl->debugJournalRecords());
                Serial.flush();
            } else {
                _ftl->persist();
            }
        }
        return RES_OK;
    case GET_SECTOR_COUNT: {
        LBA_t *p = (LBA_t *)buff;
        if (_ftl) {
            *p = _ftl->lbaCount();
        } else {
            *p = _fi->size() / 4096;
        }
        return RES_OK;
    }
    case GET_SECTOR_SIZE: {
        WORD *w = (WORD *)buff;
        *w = _sectorSize;
        return RES_OK;
    }
    case GET_BLOCK_SIZE: {
        DWORD *dw = (DWORD *)buff;
        *dw = 4096 / _sectorSize; // flash erase block in SECTORS (8 with FTL, 1 without), not bytes; only f_mkfs alignment reads this
        return RES_OK;
    }
    case CTRL_TRIM: {
        LBA_t *lba = (LBA_t *)buff;
        for (unsigned int i = lba[0]; i <= lba[1]; i++) { // lba[1] is inclusive (ff.cpp passes "End of data area to be freed")
            if (_ftl) {
                _ftl->trim(i);
            } else {
                _fi->eraseBlock(i);
            }
        }
        return RES_OK;
    }
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime() {
    time_t now;
    if (fatfs::__fatfs_timeCallback) {
        now = fatfs::__fatfs_timeCallback();
    } else {
        now = time(nullptr);
    }
    struct tm tmbuf;
    struct tm *stm = localtime_r(&now, &tmbuf); // localtime()'s shared static buffer races with other callers
    if (!stm) {
        return 0;
    }
    if (stm->tm_year < 80) {
        // FAT can't report years before 1980
        stm->tm_year = 80;
    }
    return (DWORD)(stm->tm_year - 80) << 25 |
           (DWORD)(stm->tm_mon + 1) << 21 |
           (DWORD)stm->tm_mday << 16 |
           (DWORD)stm->tm_hour << 11 |
           (DWORD)stm->tm_min << 5 |
           (DWORD)stm->tm_sec >> 1;
}

}
