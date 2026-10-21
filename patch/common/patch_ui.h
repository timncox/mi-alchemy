/*
 * patch_ui.h -- the Daisy Patch panel conventions shared by the Clouds and
 * Beads ports (patch/clouds, patch/beads). Header-only, so patch-sim needs
 * nothing but an -I for it.
 *
 *   Paged knobs with PICKUP. The Patch has four knob+CV channels (the CV is
 *   summed into the knob before the ADC), and both engines have more than
 *   four continuous parameters, so the knobs are paged. A knob is inert on a
 *   new page until it crosses the stored value or comes within kPickupSlack
 *   of it (Belt's rule, belt_patch.cpp, PICKUP_SLACK 3 on a 0..127 scale:
 *   real Patch knobs stop short of full scale). CV is summed into whatever
 *   parameter its knob currently addresses, and a CV swing can trip pickup.
 *
 *   End law. A real Patch knob reads about 0.01..0.98, so the knob is mapped
 *   (raw - kLo) / (kHi - kLo), clamped: fully CCW is 0 and fully CW is 1.
 *
 *   Click to select, turn to change, click to let go (Tim, 2026-10-03: push-
 *   and-turn never worked on his Patch). A click is RisingEdge then
 *   FallingEdge with no detent between. A click on "Mods" opens the picker.
 */
#pragma once
#include "daisy_patch.h"
#include <stdint.h>
#include <stdlib.h>

namespace pui
{
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
inline int   clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Knob end law: 0.01 -> 0, 0.97 and above -> 1. */
inline float knob_law(float raw)
{
    constexpr float kLo = 0.01f, kHi = 0.97f;
    return clampf((raw - kLo) / (kHi - kLo), 0.0f, 1.0f);
}

/* Pickup on the 0..127 scale Belt uses, so the slack means the same here. */
constexpr int kPickupSlack = 3;

template <int PAGES>
struct PagedKnobs
{
    float value[PAGES][4]; /* the parameters, 0..1 */
    bool  used[PAGES][4];  /* false = no parameter on this knob */
    bool  live[4];
    int   at[4];           /* last raw knob reading, 0..127 */
    int   page = 0;

    static int to127(float v) { return (int)(v * 127.0f + 0.5f); }

    /* Re-arm pickup: call on a page change, at boot, and after the picker. */
    void Arm(daisy::DaisyPatch &hw)
    {
        for(int k = 0; k < 4; k++)
        {
            live[k] = false;
            at[k]   = to127(knob_law(hw.GetKnobValue((daisy::DaisyPatch::Ctrl)k)));
        }
    }
    void SetPage(daisy::DaisyPatch &hw, int p)
    {
        page = clampi(p, 0, PAGES - 1);
        Arm(hw);
    }
    /* Once per control pass. Returns a bitmask of knobs whose value moved. */
    unsigned Update(daisy::DaisyPatch &hw)
    {
        unsigned moved = 0;
        for(int k = 0; k < 4; k++)
        {
            if(!used[page][k]) continue;
            const float v   = knob_law(hw.GetKnobValue((daisy::DaisyPatch::Ctrl)k));
            const int   raw = to127(v);
            if(!live[k])
            {
                const int cur = to127(value[page][k]);
                if((at[k] <= cur && raw >= cur) || (at[k] >= cur && raw <= cur)
                   || abs(raw - cur) <= kPickupSlack)
                    live[k] = true;
                at[k] = raw;
                if(!live[k]) continue;
            }
            at[k] = raw;
            if(v != value[page][k])
            {
                value[page][k] = v;
                moved |= 1u << k;
            }
        }
        return moved;
    }
    /* '*' = waiting for pickup. */
    char Mark(int k) const { return used[page][k] && !live[k] ? '*' : ' '; }
};

/* Click-select menu. Rows are app-defined; the app supplies an edit hook. */
struct Menu
{
    int  sel     = 0;
    int  count   = 0;
    int  top     = 0;  /* first visible row */
    bool editing = false;
    bool down = false, turned = false;

    /* Returns +-n detents to apply to the selected row (only while editing),
     * and sets *click_mods when a click lands on the row index mods_row. */
    int Poll(daisy::DaisyPatch &hw, int mods_row, bool *click_mods)
    {
        *click_mods = false;
        int edit    = 0;
        if(hw.encoder.RisingEdge())
        {
            down   = true;
            turned = false;
        }
        const int inc = hw.encoder.Increment();
        if(inc)
        {
            if(down) turned = true; /* a turn during a press is not a click */
            if(editing)
                edit = inc;
            else
                sel = clampi(sel + inc, 0, count - 1);
        }
        if(hw.encoder.FallingEdge())
        {
            const bool click = down && !turned;
            down             = false;
            if(click)
            {
                if(sel == mods_row)
                    *click_mods = true;
                else
                    editing = !editing;
            }
        }
        return edit;
    }
    /* Keep the cursor inside a window of `rows` visible rows. */
    void Scroll(int rows)
    {
        if(sel < top) top = sel;
        if(sel >= top + rows) top = sel - rows + 1;
    }
    char Cursor(int row) const { return row == sel ? (editing ? '*' : '>') : ' '; }
};

/* Peak audio-block load, Meld's method: the callback brackets its body with
 * the tick counter and keeps the worst block; the main loop takes it and
 * holds the worst of each half second for the screen. libDaisy's own sample
 * conversion around the callback is not counted, so the true figure is a
 * little higher. */
struct LoadMeter
{
    float          inv = 0.0f;
    volatile float peak = 0.0f;
    float          shown = 0.0f, window = 0.0f;
    uint32_t       window_start = 0;

    void Init(float sr, size_t block)
    {
        inv = 1.0f / ((float)daisy::System::GetTickFreq() * ((float)block / sr));
    }
    void Block(uint32_t t0)
    {
        const float l = (float)(daisy::System::GetTick() - t0) * inv;
        if(l > peak) peak = l;
    }
    void Take(uint32_t now)
    {
        const float pk = peak;
        peak           = 0.0f;
        if(pk > window) window = pk;
        if(now - window_start >= 500u)
        {
            shown        = window;
            window       = 0.0f;
            window_start = now;
        }
    }
};

/* One-pole envelope follower for the CV outs. */
struct Env
{
    float a = 0.0f, r = 0.0f, e = 0.0f;
    void  Init(float attack, float release) { a = attack, r = release; }
    void  Process(float x)
    {
        x = x < 0 ? -x : x;
        e += (x > e ? a : r) * (x - e);
    }
};
} // namespace pui
