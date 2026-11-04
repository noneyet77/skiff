/* SPDX-License-Identifier: GPL-3.0-only */
/* MENU > MIDI > M8 MODE (firmware/src/m8mode.c, seq.c m8_keys): the panel as a controller for an M8. Buttons go out as
 * notes on channel 16 while held (a tap shorter than a frame too), encoders as relative CCs on channel 16, the keys as
 * notes on channel 1 that do not sound here; OCT- / OCT+ move the keys; HOME opens the MENU and lets go of what is held;
 * M8 MODE OFF gives the panel back. The ui_test.c prelude, the MIDI OUT ring read as a host would (midi_out_test.c).
 * M8_SHOT=path writes the M8 MODE screen as a PPM. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

typedef struct { uint8_t st, d1, d2; } m8ev_t;
static m8ev_t ev[4096];
static uint32_t evn;

static void drain(void)
{
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQO];
        if (evn < 4096u)
            ev[evn++] = (m8ev_t){(uint8_t)(p >> 8), (uint8_t)((p >> 16) & 127u), (uint8_t)((p >> 24) & 127u)};
        mo_r++;
    }
}
static void blk(void) { events_block(CTL); drain(); }
static void fr(void) { frame(); blk(); }
static void clear(void) { drain(); evn = 0; }
static int has(uint8_t st, uint8_t d1, uint8_t d2)
{
    for (uint32_t i = 0; i < evn; i++)
        if (ev[i].st == st && ev[i].d1 == d1 && ev[i].d2 == d2)
            return 1;
    return 0;
}
static int fails;
static void m8check(const char *what, int ok)
{
    printf("m8: %-90s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        fails++;
        for (uint32_t i = 0; i < evn; i++)
            printf("      %02X %3u %3u\n", ev[i].st, ev[i].d1, ev[i].d2);
    }
}
static int sounding(void)
{
    for (uint32_t t = 0; t < NTRK; t++)
        for (uint32_t i = 0; i < NVOICE; i++)
            if (trk[t].v[i].active && trk[t].v[i].gate)
                return 1;
    return 0;
}
static void shot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6 240 240 255\n");
    for (uint32_t i = 0; i < 240u * 240u; i++) {
        uint16_t c = host_screen[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63u) << 2), (uint8_t)((c & 31u) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void)
{
    ui_power_on();
    memset(&mn, 0, sizeof mn);
    memset(&mo, 0, sizeof mo);
    mo_w = mo_r = 0;
    usb.config = 1;
    MIDI_BYTE = MO_CLOCK_OFF | NO_OFF;               /* (no clock, no notes of the tracks: only M8 MODE's) */
    fm1_in.notes = kb_prev = 0;
    blk();
    menu_put(MI_M8, 1);
    m8check("MENU > MIDI > M8 MODE ON sets its bit", (MIDI_BYTE & MO_M8_ON) != 0u && menu_get(MI_M8) == 1);
    fr();
    clear();

    press(B_ENV);
    blk();
    m8check("a tap of ENV: note 38 on and off, channel 16", has(0x9F, 38, 127) && has(0x8F, 38, 0) && evn == 2u);
    clear();

    btn_down(B_LFO);
    fr();
    m8check("LFO held: note 39 on, no off yet", has(0x9F, 39, 127) && !has(0x8F, 39, 0));
    turn(EN_K1, 3);
    blk();
    m8check("KNOB 1 three detents clockwise: CC 23 = 3 on channel 16", has(0xBF, 23, 3));
    turn(EN_SELECT, -2);
    blk();
    m8check("SELECT two detents back: CC 20 = 126 (two's complement)", has(0xBF, 20, 126));
    btn_up(B_LFO);
    fr();
    m8check("LFO let go: note 39 off", has(0x8F, 39, 0));
    clear();

    host_pressed |= 1u << panel.btn[B_PLAY];          /* down and up again before the frame looked */
    fr();
    m8check("a tap between two frames still sends PLAY's on and off (46)", has(0x9F, 46, 127) && has(0x8F, 46, 0));
    clear();

    fm1_in.notes = 1u;                                /* the lowest key */
    blk();
    m8check("the lowest key: note 53 (F3) on channel 1, velocity 100", has(0x90, 53, 100));
    m8check(".. and nothing sounds on the FM-1", !sounding());
    press(B_OCTUP);
    blk();
    m8check("OCT+ sends nothing", evn == 1u);
    fm1_in.notes = 0;
    blk();
    m8check("the key let go after OCT+: the note it sent (53) ends", has(0x80, 53, 0));
    fm1_in.notes = 1u;
    blk();
    m8check("pressed again: an octave up (65)", has(0x90, 65, 100));
    fm1_in.notes = 0;
    blk();
    press(B_OCTDN);
    clear();

    press(B_SCL);
    blk();
    m8check("a tap of SEL sends nothing", evn == 0u);
    btn_down(B_SCL);
    fr();
    press(B_OCTUP);
    blk();
    press(B_OCTUP);
    blk();
    btn_up(B_SCL);
    fr();
    m8check("SEL + OCT+ twice: the keys on channel 3, kept with the settings, the octave as it was",
            m8_kch() == 2u && favorites.factory[15][29] == (0x80u | 2u) && m8_oct == 0 && evn == 0u);
    fm1_in.notes = 1u;
    blk();
    m8check(".. the lowest key: note 53 on channel 3", has(0x92, 53, 100));
    btn_down(B_SCL);
    fr();
    for (int i = 0; i < 20; i++) {
        press(B_OCTUP);
        blk();
    }
    btn_up(B_SCL);
    fr();
    fm1_in.notes = 0;
    blk();
    m8check(".. let go after the channel moved: its note off still on channel 3", has(0x82, 53, 0));
    m8check(".. and the channel stops at 16", m8_kch() == 15u);
    btn_down(B_SCL);
    fr();
    for (int i = 0; i < 20; i++) {
        press(B_OCTDN);
        blk();
    }
    btn_up(B_SCL);
    fr();
    m8check("SEL + OCT- back down: channel 1 again", m8_kch() == 0u);
    favorites.factory[15][29] = 30u;
    m8check("an older STYLE value in that byte reads as channel 1", m8_kch() == 0u);
    favorites.factory[15][29] = 0x80u;
    clear();

    turn(EN_K2, 100);
    blk();
    m8check("a fast turn of 100: split in messages of 63 at most (CC 24 = 63, then 37)", has(0xBF, 24, 63) && has(0xBF, 24, 37));
    clear();

    press(B_FX);
    blk();
    m8check("FX: the FX page, and no note for it", m8.fx && evn == 0u);
    turn(EN_K1, 3);
    blk();
    m8check("FX page, KNOB 1 three detents: CC 70 = 64 + 6, absolute, channel 16", has(0xBF, 70, 70) && evn == 1u);
    clear();
    turn(EN_K4, 100);
    blk();
    turn(EN_K4, 5);
    blk();
    m8check(".. KNOB 4 far past the end: CC 73 = 127 once, nothing more", has(0xBF, 73, 127) && evn == 1u);
    clear();
    turn(EN_SELECT, 1);
    blk();
    m8check(".. SELECT still moves the M8's cursor (CC 20 = 1)", has(0xBF, 20, 1));
    clear();
    if (getenv("M8_SHOT_FX")) {
        ui.force = 1;
        fr();
        shot(getenv("M8_SHOT_FX"));
    }
    press(B_FX);
    blk();
    turn(EN_K1, 1);
    blk();
    m8check("FX again: FX B, KNOB 1 is CC 74 (its own value, 64 + 2)", m8.fx == 2u && has(0xBF, 74, 66));

    clear();
    press(B_FX);
    blk();
    turn(EN_K1, 1);
    blk();
    m8check("FX a third time: the M8's page, KNOB 1 is VALUE again (CC 23 = 1)", !m8.fx && has(0xBF, 23, 1));
    clear();
    press(B_FX);
    blk();
    turn(EN_K1, -1);
    blk();
    m8check(".. and FX A kept its knob (CC 70: 70 - 2 = 68)", m8.fx == 1u && has(0xBF, 70, 68));
    press(B_FX);
    blk();
    press(B_FX);
    blk();
    clear();

    turn(EN_PRESET, -10);
    blk();
    fm1_in.notes = 1u;
    blk();
    m8check("PRESET ten detents back: the keys' velocity 100 - 40 = 60, and no CC for it", has(0x90, 53, 60) && evn == 1u);
    fm1_in.notes = 0;
    blk();
    turn(EN_PRESET, 100);
    blk();
    m8check(".. turned far up: 127 at most", m8_vel == 127u);
    turn(EN_PRESET, -7);
    blk();
    m8check(".. and back to 99", m8_vel == 99u);
    clear();

    midi_enqueue(0x0Fu | 0xFAu << 8, 1u);
    blk();
    m8check("the M8's start: PLAY's green on", m8_play == 1u);
    ui_leds();
    m8check(".. and lit (PLAY's green LED, no clock yet)",
            (fm1_led[LED_PLAY_GREEN >> 3] >> (LED_PLAY_GREEN & 7u)) & 1u);
    for (int i = 0; i < 7; i++)
        midi_enqueue(0x0Fu | 0xF8u << 8, 1u);
    blk();
    m8check(".. its clock counted (7 clocks: the 7th 24th of the beat)", m8_pulse == 6u);
    ui_leds();
    m8check(".. past the beat's first quarter: the green dark until the next beat",
            !((fm1_led[LED_PLAY_GREEN >> 3] >> (LED_PLAY_GREEN & 7u)) & 1u));
    midi_enqueue(0x09u | 0x90u << 8 | 60u << 16 | 100u << 24, 1u);
    blk();
    m8check(".. a note from the M8 does not sound here", !sounding());
    m8check(".. nor does its clock or start run the sequencer here", !song.playing);
    midi_enqueue(0x0Fu | 0xFCu << 8, 1u);
    blk();
    m8check("the M8's stop: PLAY's green off", m8_play == 0u);
    clear();
    turn(EN_K1, 0);
    clear();

    if (getenv("M8_SHOT")) {                          /* the screen, drawn afresh */
        ui.force = 1;
        fr();
        shot(getenv("M8_SHOT"));
    }

    /* HOME twice quickly: the FM-1's own panel, M8 MODE still on; twice again: back */
    press(B_HOME);
    fr();
    press(B_HOME);
    fr();
    m8check("HOME twice quickly: the FM-1's panel, M8 MODE still on, no MENU", m8.fm1 && m8_active() && !m8_panel() && !ui.menu);
    clear();
    fm1_in.notes = 1u;
    blk();
    m8check(".. there the keys still go to the M8 (KEYS DIRECT)", has(0x90, 53, 99) && !sounding());
    fm1_in.notes = 0;
    blk();
    frames(400);
    press(B_HOME);
    fr();
    press(B_HOME);
    fr();
    frames(400);
    m8check(".. and HOME twice there: the M8's panel again, no MENU", !m8.fm1 && m8_panel() && !ui.menu);
    clear();

    btn_down(B_EDIT);
    fr();
    btn_down(B_HOME);
    fr();
    m8check("HOME pressed: no MENU yet (a second tap may come), nothing sent for it", !ui.menu && !has(0x9F, 36 + B_HOME, 127));
    btn_up(B_HOME);
    fr();
    frames(400);
    m8check(".. let go and no second tap: the MENU", ui.menu);
    blk();
    m8check(".. EDIT held when the MENU opened: its note ends", has(0x8F, 40, 0));
    btn_up(B_EDIT);
    fr();
    clear();
    turn(EN_K1, 2);
    blk();
    m8check("in the MENU the knobs are the MENU's: no CC", evn == 0u);

    /* M8 KEYS TRACK: the keys play the selected track (its chord, arp, scale); an EXT channel sends it to the M8 only */
    ui.menu = 0;
    ui.force = 1;
    menu_put(MI_M8KEYS, 1);
    fr();
    clear();
    fm1_in.notes = 1u;
    blk();
    m8check("KEYS TRACK, T1 a sound: the key sounds here, no direct note to the M8", sounding() && !has(0x90, 53, 99));
    fm1_in.notes = 0;
    for (int i = 0; i < 200; i++)
        blk();
    menu_put(MI_TCH1, 17u);                           /* T1 CHANNEL: CH 1 EXT */
    clear();
    fm1_in.notes = 1u;
    blk();
    {
        int on = 0;
        for (uint32_t i = 0; i < evn; i++)
            on |= ev[i].st == 0x90;
        m8check(".. T1 on CH 1 EXT: the key goes to the M8 on channel 1, nothing sounds here", on && !sounding());
    }
    fm1_in.notes = 0;
    blk();
    {
        int off = 0;
        for (uint32_t i = 0; i < evn; i++)
            off |= ev[i].st == 0x80;
        m8check(".. and its note off when let go", off);
    }
    menu_put(MI_TCH1, 1u);                            /* (T1 back to CH 1, a sound) */
    clear();
    {
        int32_t o = song.octave;
        press(B_OCTUP);
        blk();
        m8check("KEYS TRACK: OCT+ moves the track's octave (song.octave), no channel or m8_oct change", song.octave == o + 1 && m8_oct == 0);
        press(B_OCTDN);
        blk();
    }
    btn_down(B_SCL);
    fr();
    press(B_OCTUP);
    blk();
    btn_up(B_SCL);
    fr();
    m8check(".. SEL + OCT+: the next track", song.sel == 1u);
    btn_down(B_SCL);
    fr();
    press(B_OCTDN);
    blk();
    btn_up(B_SCL);
    fr();
    btn_down(B_LFO);                                  /* (SHIFT on the M8's panel: LFO opens no layer on the keys) */
    fr();
    clear();
    fm1_in.notes = 1u;
    blk();
    m8check(".. a key with LFO (the M8's SHIFT) held still plays, no layer", song.sel == 0u && sounding());
    fm1_in.notes = 0;
    blk();
    btn_up(B_LFO);
    fr();
    for (int i = 0; i < 200; i++)
        blk();
    menu_put(MI_M8KEYS, 0);
    fr();
    clear();

    /* M8 SOUND: the M8's notes play the tracks here, never sent back out */
    midi_enqueue(0x09u | 0x90u << 8 | 60u << 16 | 100u << 24, 1u);
    blk();
    m8check("M8 SOUND OFF: a note from the M8 does not sound", !sounding());
    midi_enqueue(0x08u | 0x80u << 8 | 60u << 16, 1u);
    blk();
    menu_put(MI_M8SOUND, 1);
    MIDI_BYTE = (uint8_t)((MIDI_BYTE & ~3u) | NO_ALL);   /* (NOTES OUT ALL: still not sent back) */
    clear();
    midi_enqueue(0x09u | 0x90u << 8 | 60u << 16 | 100u << 24, 1u);
    blk();
    m8check("M8 SOUND ON: a note from the M8 on channel 1 plays T1", sounding());
    m8check(".. and is not sent back to the M8 (NOTES OUT ALL)", !has(0x90, 60, 100));
    midi_enqueue(0x08u | 0x80u << 8 | 60u << 16, 1u);
    blk();
    menu_put(MI_M8SOUND, 0);
    MIDI_BYTE = (uint8_t)((MIDI_BYTE & ~3u) | NO_OFF);
    for (int i = 0; i < 200; i++)
        blk();
    clear();

    /* the sequencer follows the M8 (CLOCK USB): the M8's start runs it, its stop stops it */
    menu_put(MI_CLOCK, 1u);                           /* (USB) */
    clear();
    midi_enqueue(0x0Fu | 0xFAu << 8, 1u);
    for (int i = 0; i < 6; i++) {
        midi_enqueue(0x0Fu | 0xF8u << 8, 1u);
        blk();
    }
    m8check("CLOCK USB: the M8's start runs the sequencer here too", song.playing && m8_play);
    midi_enqueue(0x0Fu | 0xFCu << 8, 1u);
    blk();
    blk();
    m8check(".. and its stop stops it", !song.playing && !m8_play);
    menu_put(MI_CLOCK, 0u);
    clear();

    menu_put(MI_M8, 0);
    ui.menu = 0;
    ui.force = 1;
    fr();
    clear();
    press(B_ENV);
    blk();
    fm1_in.notes = 1u;
    blk();
    m8check("M8 MODE OFF: buttons send nothing, the keys sound again", evn == 0u && sounding());
    fm1_in.notes = 0;
    blk();

    MIDI_BYTE = (uint8_t)((MIDI_BYTE & ~MO_CLOCK_OFF) | MO_M8_ON);   /* CLOCK OUT ON, M8 MODE ON, playing */
    clear();
    transport_req = 1;
    for (uint32_t i = 0; i < 2000u; i++)
        blk();
    transport_req = 2;
    blk();
    {
        int clocks = 0;
        for (uint32_t i = 0; i < evn; i++)
            clocks += ev[i].st == 0xF8;
        m8check("M8 MODE: no MIDI clock goes out, even playing with CLOCK OUT ON", clocks == 0);
    }
    MIDI_BYTE = (uint8_t)(MIDI_BYTE & ~MO_M8_ON);
    clear();
    transport_req = 1;
    for (uint32_t i = 0; i < 2000u; i++)
        blk();
    transport_req = 2;
    blk();
    {
        int clocks = 0;
        for (uint32_t i = 0; i < evn; i++)
            clocks += ev[i].st == 0xF8;
        m8check(".. and with M8 MODE OFF the clock is back", clocks > 0);
    }

    printf(fails ? "m8 mode test FAILED (%d)\n" : "m8 mode test passed\n", fails);
    return fails != 0;
}
