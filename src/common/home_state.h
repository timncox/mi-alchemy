/*
 * HomeFile: a firmware's autosaved panel as its own file on the SD card,
 * 0:/alchemy/state/<name>.st, instead of a QSPI preset slot.
 *
 * Why: the SDK's QSPI store is 16 slots for every firmware on the module
 * (kPresetNumSlots, the top 640 KB of the chip, ending at its last byte),
 * and each firmware taking one as its "home slot" used them all up by
 * 2026-10-13 (Hypjolin and Beads both wanted the last one). A file named
 * after the firmware cannot collide with another firmware's, needs no slot
 * map, and leaves the QSPI slots to presets saved by hand.
 *
 * Same contract as mi::HomeSlot (mi_family.h): Boot() once after
 * presets.Init(), Touch() on every change, Service() every control frame;
 * it writes 5 s after the last change, never while a button is held,
 * Settings is open, the picker is writing or the card is busy.
 *
 * Migration: give the slot the firmware used to autosave to as
 * @p legacy_slot. With no file on the card, Boot() loads that slot instead
 * and the first Service() writes the file; the slot itself is left alone.
 *
 * The file is written as <name>.tmp then renamed, so a power cut mid-write
 * leaves the previous file (or, between unlink and rename, the .tmp, which
 * Boot() also tries). The format is state_file.h.
 */
#pragma once

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/presets.h"

#include "state_file.h"

namespace mi {

struct HomeFile
{
    /* Room for the managed components' bytes: a pager's stored pots are
     * pages x 6 floats, Settings and the CPU extras a few dozen bytes. */
    static constexpr size_t kCap = 1024u;

    const char* name;
    int         legacy_slot;
    bool        dirty    = false;
    uint32_t    since    = 0;
    uint32_t    retry_at = 0;

    explicit HomeFile(const char* n, int legacy = -1) : name(n), legacy_slot(legacy) {}

    /* Returns true when there was saved state to restore (a file, the
     * legacy slot, or the SDK's slot 0), as HomeSlot::Boot did. */
    bool Boot(alchemy::Presets& presets, alchemy::SdCard& card, uint32_t now)
    {
        sd_ = &card;
        const bool had_boot = presets.HasValid(0);
        presets.BootLoad();
        if (Read(presets, now, "st") || Read(presets, now, "tmp")) return true;
        if (legacy_slot >= 0 && presets.HasValid((uint8_t)legacy_slot)
            && presets.Load((uint8_t)legacy_slot))
        {
            dirty = true;   /* carry it onto the card at the first Service() */
            since = now - 5000u;
            return true;
        }
        return had_boot;
    }

    void Touch(uint32_t now)
    {
        dirty = true;
        since = now;
    }

    /* Every control frame. `busy` = Settings open or the picker writing.
     * Returns true when it saved. A failed write (no card, full card)
     * stays dirty and retries every 30 s. */
    bool Service(alchemy::Presets& presets, alchemy::AlchemyLab& hw, uint32_t now, bool busy)
    {
        if (!dirty || busy || !sd_ || now - since < 5000u) return false;
        if ((int32_t)(now - retry_at) < 0) return false;
        for (uint8_t b = 0; b < alchemy::kNumButtons; b++)
            if (hw.buttons[b].Pressed()) return false;
        if (sd_->Busy()) return false;
        if (!Write(presets, now))
        {
            retry_at = now + 30000u;
            return false;
        }
        dirty = false;
        return true;
    }

  private:
    alchemy::SdCard* sd_ = nullptr;

    /* SDMMC1's IDMA cannot reach DTCM (where .bss lives): the buffer and
     * the FIL go in .axi_bss, as the picker's do. */
    static uint8_t* Buf()
    {
        alignas(32) static uint8_t ALCHEMY_SDMMC_BSS b[state_file::kHeaderLen + kCap];
        return b;
    }
    static FIL& File()
    {
        alignas(32) static FIL ALCHEMY_SDMMC_BSS f;
        return f;
    }

    void PathFor(char* out, size_t cap, const char* ext) const
    {
        snprintf(out, cap, "0:/alchemy/state/%s.%s", name, ext);
    }

    bool Read(alchemy::Presets& presets, uint32_t now, const char* ext)
    {
        alchemy::SdCard::BusyGuard guard(*sd_);
        if (!sd_->EnsureMounted(now)) return false;
        char path[48];
        PathFor(path, sizeof path, ext);
        FIL& f = File();
        memset(&f, 0, sizeof f);
        if (f_open(&f, path, FA_READ) != FR_OK) return false;
        uint8_t* b = Buf();
        UINT     n = 0;
        const FRESULT r = f_read(&f, b, (UINT)(state_file::kHeaderLen + kCap), &n);
        f_close(&f);
        if (r != FR_OK) return false;
        size_t len = 0;
        if (!state_file::Open(b, n, presets.LiveSchemaHash(), presets.LiveSize(), len))
            return false;
        return presets.DeserializeLive(b + state_file::kHeaderLen, len);
    }

    bool Write(alchemy::Presets& presets, uint32_t now)
    {
        alchemy::SdCard::BusyGuard guard(*sd_);
        if (!sd_->EnsureMounted(now)) return false;
        uint8_t*     b   = Buf();
        const size_t len = presets.SerializeLive(b + state_file::kHeaderLen, kCap);
        if (len == 0u) return false;   /* too big for kCap, or nothing managed */
        const size_t total = state_file::Seal(b, presets.LiveSchemaHash(), len);

        f_mkdir("0:/alchemy");          /* FR_EXIST is the usual answer */
        f_mkdir("0:/alchemy/state");

        char tmp[48], path[48];
        PathFor(tmp, sizeof tmp, "tmp");
        PathFor(path, sizeof path, "st");
        FIL& f = File();
        memset(&f, 0, sizeof f);
        if (f_open(&f, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
        UINT          n  = 0;
        const FRESULT wr = f_write(&f, b, (UINT)total, &n);
        const FRESULT cl = f_close(&f);
        if (wr != FR_OK || cl != FR_OK || n != total)
        {
            sd_->OnIoError();
            return false;
        }
        f_unlink(path);                 /* FR_NO_FILE the first time */
        return f_rename(tmp, path) == FR_OK;
    }
};

}  // namespace mi
