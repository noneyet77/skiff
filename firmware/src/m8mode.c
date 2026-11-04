/* SPDX-License-Identifier: GPL-3.0-only */
/* MENU > MIDI > M8 MODE: the FM-1 as a controller for a Dirtywave M8 (m8emu's MIDI MAPPING, or anything that maps MIDI
 * to the M8's buttons). While it is ON the panel plays no sound of its own and the screen shows the map:
 *
 *   buttons   a note on MIDI channel 16 while held: M8_NOTE0 + the label's index (panel.c B_FX .. B_REC; 36 .. 47).
 *             HOME opens the MENU (to turn M8 MODE off); OCT- / OCT+ move the keys an octave.
 *   encoders  CC M8_CC0 + the role (panel.c EN_SELECT .. EN_K4; 20 .. 26) on channel 16, relative: 1..63 a turn
 *             clockwise, 127..65 anticlockwise (two's complement), one step a detent.
 *   keys      notes on channel 1 (seq.c m8_keys, in the audio ISR as the keys always are), F3 + the key, velocity 100.
 *   PRESET    the keys' velocity, 1..127 (M8_VEL_STEP a detent; 100 at power-on): seq.c m8_vel.
 *   the M8    what it sends back (the app passes its MIDI OUT on): start / stop light PLAY's green, its clock flashes it
 *             on the beat (seq.c m8_play, m8_pulse); nothing else of it reaches the sequencer or the voices here.
 *   SEL       held: OCT- / OCT+ step the keys' MIDI channel (1..16), kept with the settings (seq.c m8_kch); with
 *             M8 KEYS TRACK the selected track instead.
 *   HOME      a tap: the MENU (M8_DTAP_MS after it, no second tap came); twice quickly: the FM-1's own panel (its pages:
 *             a track's ARP, CHORD, SCALE, patterns ..) with M8 MODE still on, and twice quickly there: back.
 *   M8 KEYS   (MENU > MIDI) DIRECT: as above; TRACK: the keys play the selected track as without M8 MODE (its chord,
 *             arp, scale, octave: OCT- / OCT+ move song.octave), heard here, sent to the M8 as its CHANNEL (EXT: only
 *             sent) and NOTES OUT say.
 *   M8 SOUND  (MENU > MIDI) ON: the M8's notes and CCs play the tracks here as MIDI IN does (never sent back out).
 *   the clock the M8's clock and start / stop also reach the clock here: with CLOCK on USB the sequencer follows the M8.
 *   FX        steps KNOB 1..4 through the M8's page, FX A and FX B: there they send absolute CCs on channel 16, FX A
 *             M8_FX_CC0 .. +3, FX B the next four (0..127, M8_FX_STEP a detent, each knob of each page keeping its
 *             value; the FX LED lit on A, breathing on B), for the M8's PROJECT > MIDI > MAPPINGS (or its MIDI learn) to
 *             tie to an effect send, a filter, a mod; m8emu passes CCs it has no mapping for to the M8.
 *
 * M8_MAP is the layout m8emu loads for this unit (its FM-1 preset: hosts/sdl/common/midi_map.c); the screen draws it. */
#define M8_CH_CTL 15u                                   /* channel 16 */
#define M8_NOTE0 36u
#define M8_CC0 20u
#define M8_OCT_MIN (-4)
#define M8_OCT_MAX 4
#define M8_FX_CC0 70u
#define M8_FX_STEP 2
#define M8_FX_PAGES 2u
#define M8_VEL_STEP 4
#define M8_DTAP_MS 300u                                 /* HOME twice within this: the other panel */
static const char *const M8_BTN_MAP[NB] = {"FX PAGE", "KEY CH", "UP", "SHIFT", "EDIT", "OPTION", "MENU", "LEFT",
                                           "DOWN", "RIGHT", "PLAY", "", "OCT-", "OCT+"};
static const char *const M8_ENC_MAP[NE] = {"CURSOR U/D", "CURSOR L/R", "KEY VEL", "VALUE", "VALUE +16", "SCREEN U/D",
                                           "SCREEN L/R"};

static struct {
    uint8_t was_on, dirty;
    uint8_t sel;                                        /* SEL held: OCT- / OCT+ change the keys' channel (or track) */
    uint8_t fm1;                                        /* the FM-1's own panel up, M8 MODE still on */
    uint8_t home_pending, home_held;                    /* HOME tapped, the MENU waiting for no second tap; HOME down */
    uint32_t home_up;                                   /* fm1_ms | 1 of HOME's last release, 0 none */
    uint8_t fx;                                         /* KNOB 1..4 on the FX page */
    uint8_t fx_val[4u * M8_FX_PAGES];                   /* their values there, as last sent, FX A's then FX B's */
    uint8_t fx_init;
    uint32_t prev;                                      /* the matrix buttons held, as last sent */
    char last[24];                                      /* the last control used, for the screen */
} m8;

static int m8_active(void) { return (MIDI_BYTE & MO_M8_ON) != 0u; }
static int m8_track_keys(void) { return (MIDI_BYTE & MO_M8_TRACK) != 0u; }
static int m8_panel(void) { return m8_active() && !m8.fm1; }    /* the panel is the M8's */
static uint32_t m8_since(uint32_t t) { return (fm1_ms | 1u) - t; }   /* ms since a time stored as fm1_ms | 1 */
static void m8_leave(void);
static void m8_to_fm1(void)                             /* HOME twice: the FM-1's own panel, M8 MODE on */
{
    m8_leave();
    m8.fm1 = 1;
    m8.home_pending = 0;
    m8.home_up = 0;
    ui.force = 1;
    ui_message("FM-1 PANEL: HOME TWICE FOR THE M8");
}

static void m8_send(uint32_t pkt)                       /* from the main loop: as cc_knob, behind what the ISR queued */
{
    fm1_irq_off();
    (void)mo_put(pkt, mo.now);
    fm1_irq_on();
}

static void m8_button(uint32_t b, int down)
{
    if (b >= NB)
        return;
    if (b == B_SCL) {                                   /* SEL: no note, a modifier for OCT- / OCT+ */
        m8.sel = (uint8_t)down;
        return;
    }
    if (b == B_OCTDN || b == B_OCTUP) {
        if (down && m8_track_keys()) {                  /* KEYS TRACK: the track's octave, SEL held the track */
            if (m8.sel)
                track_select((song.sel + (b == B_OCTUP ? 1u : NTRK - 1u)) % NTRK);
            else
                song.octave += b == B_OCTDN ? (song.octave > -3 ? -1 : 0) : (song.octave < 3 ? 1 : 0);
            m8.dirty = 1;
            return;
        }
        if (down && m8.sel) {                           /* SEL held: the keys' channel, 1..16 */
            int32_t c = (int32_t)m8_kch() + (b == B_OCTUP ? 1 : -1);
            c = c < 0 ? 0 : c > 15 ? 15 : c;
            favorites.factory[15][29] = (uint8_t)(M8_KCH_SET | (uint32_t)c);
            settings_later();                           /* written once the panel rests or the MENU opens */
            m8.dirty = 1;
            return;
        }
        if (down) {
            int32_t o = m8_oct + (b == B_OCTUP ? 1 : -1);
            m8_oct = (int8_t)(o < M8_OCT_MIN ? M8_OCT_MIN : o > M8_OCT_MAX ? M8_OCT_MAX : o);
            m8.dirty = 1;
        }
        return;
    }
    if (b == B_FX) {                                    /* the knob page: no note */
        if (down) {
            m8.fx = (uint8_t)((m8.fx + 1u) % (M8_FX_PAGES + 1u));
            str_cpy(m8.last, m8.fx == 0u ? "KNOBS: M8" : m8.fx == 1u ? "KNOBS: FX A" : "KNOBS: FX B", sizeof m8.last);
            ui.force = 1;
        }
        return;
    }
    if (b == B_HOME) {                                  /* the MENU (where M8 MODE is), or twice: the FM-1's panel */
        if (down) {
            if (m8.home_up && m8_since(m8.home_up) < M8_DTAP_MS)
                m8_to_fm1();
            else
                m8.home_pending = 1;                    /* (the MENU once it is let go and no second tap comes) */
        } else if (m8.home_pending)
            m8.home_up = fm1_ms | 1u;
        return;
    }
    m8_send(down ? 0x09u | (0x90u | M8_CH_CTL) << 8 | (M8_NOTE0 + b) << 16 | 127u << 24
                 : 0x08u | (0x80u | M8_CH_CTL) << 8 | (M8_NOTE0 + b) << 16);
    if (down) {
        str_cpy(m8.last, M8_BTN_MAP[b][0] ? M8_BTN_MAP[b] : B_NAME[b], sizeof m8.last);
        m8.dirty = 1;
    }
}

static void m8_enc(uint32_t role, int32_t s)
{
    if (role == EN_PRESET) {                            /* the keys' velocity */
        int32_t v = (int32_t)m8_vel + s * M8_VEL_STEP;
        m8_vel = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
        str_cpy(m8.last, "VELOCITY", sizeof m8.last);
        m8.dirty = 1;
        return;
    }
    if (m8.fx && role >= EN_K1 && role <= EN_K4) {     /* an FX page: an absolute value */
        uint32_t k = 4u * (m8.fx - 1u) + (role - EN_K1);
        int32_t v = (int32_t)m8.fx_val[k] + s * M8_FX_STEP;
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if ((uint8_t)v != m8.fx_val[k]) {
            m8.fx_val[k] = (uint8_t)v;
            m8_send(0x0Bu | (0xB0u | M8_CH_CTL) << 8 | (M8_FX_CC0 + k) << 16 | (uint32_t)v << 24);
        }
        m8.last[0] = 'C';
        m8.last[1] = 'C';
        m8.last[2] = (char)('0' + (M8_FX_CC0 + k) / 10u);
        m8.last[3] = (char)('0' + (M8_FX_CC0 + k) % 10u);
        m8.last[4] = ' ';
        m8.last[5] = (char)('0' + v / 100);
        m8.last[6] = (char)('0' + v / 10 % 10);
        m8.last[7] = (char)('0' + v % 10);
        m8.last[8] = 0;
        m8.dirty = 1;
        return;
    }
    while (s) {                                         /* at most 63 steps a message */
        int32_t n = s > 63 ? 63 : s < -63 ? -63 : s;
        m8_send(0x0Bu | (0xB0u | M8_CH_CTL) << 8 | (M8_CC0 + role) << 16 | (uint32_t)(n & 127) << 24);
        s -= n;
    }
    str_cpy(m8.last, M8_ENC_MAP[role][0] ? M8_ENC_MAP[role] : E_NAME[role], sizeof m8.last);
    m8.dirty = 1;
}

static void m8_leave(void);
/* ui_input: the panel while M8 MODE is on and the MENU closed. pressed: this frame's press edges (matrix bits), so a
 * tap shorter than a frame still sends its note on and off */
static void m8_input(uint32_t pressed)
{
    uint32_t cur = fm1_in.buttons, id, r;
    if (!m8.fx_init) {                                  /* the FX page's knobs start in the middle */
        m8.fx_init = 1;
        for (id = 0; id < 4u * M8_FX_PAGES; id++)
            m8.fx_val[id] = 64u;
    }
    if (!m8.was_on) {                                   /* just turned on (or back from the MENU): from what is held */
        m8.was_on = 1;
        m8.prev = cur;
        m8.last[0] = 0;
        ui.force = 1;
        pressed = 0;
    }
    for (id = 0; id < 32u; id++) {
        uint32_t bit = 1u << id, now_on = (cur & bit) != 0u, was = (m8.prev & bit) != 0u, b;
        if (!((pressed | cur | m8.prev) & bit) || ((b = panel_btn_of(id)) >= NB))
            continue;
        if (now_on != was)
            m8_button(b, (int)now_on);
        else if (!now_on && (pressed & bit)) {          /* down and up between two frames */
            m8_button(b, 1);
            m8_button(b, 0);
        }
    }
    m8.prev = cur;
    if (m8.fm1)                                         /* (HOME twice: the FM-1's panel from the next frame) */
        return;
    if (m8.home_pending && m8.home_up && m8_since(m8.home_up) >= M8_DTAP_MS) {   /* one tap: the MENU */
        m8.home_pending = 0;
        m8.home_up = 0;
        ui.msg_t = 0;
        ui.menu = 1;
        ui.confirm = 0;
        ui.force = 1;
    }
    if (ui.menu) {                                      /* HOME opened the MENU: what is held lets go now */
        m8_leave();
        return;
    }
    for (r = 0; r < NE; r++) {
        int32_t s = panel_enc(r);
        if (s)
            m8_enc(r, s);
    }
}

/* the MENU opened or M8 MODE went off: what is held lets go (a note off for each), the next entry starts afresh */
static void m8_leave(void)
{
    uint32_t id, b;
    for (id = 0; id < 32u; id++)
        if (((m8.prev >> id) & 1u) && (b = panel_btn_of(id)) < NB && b != B_HOME && b != B_OCTDN && b != B_OCTUP)
            m8_send(0x08u | (0x80u | M8_CH_CTL) << 8 | (M8_NOTE0 + b) << 16);
    m8.prev = 0;
    m8.was_on = 0;
    m8.sel = 0;
    m8.home_pending = 0;
}

/* ui_input, every frame M8 MODE is on with the FM-1's panel up and the MENU closed: HOME twice quickly, the M8's panel
 * again. 1: done (the press is the M8's, ui_input stops) */
static int m8_fm1_input(uint32_t pressed)
{
    uint32_t hb = 1u << panel.btn[B_HOME], held = (fm1_in.buttons & hb) != 0u;
    if ((pressed & hb) && m8.home_up && m8_since(m8.home_up) < M8_DTAP_MS) {
        m8.fm1 = 0;
        m8.home_up = 0;
        m8.home_held = 1;
        m8.was_on = 0;                                  /* (m8_input starts from what is held: HOME's release, nothing) */
        ui.home_t0 = 0;
        ui.force = 1;
        return 1;
    }
    if (m8.home_held && !held)
        m8.home_up = fm1_ms | 1u;
    m8.home_held = (uint8_t)held;
    return 0;
}

/* ---- the screen: the panel's map, drawn again when something changes ---- */

static void m8_cell(uint32_t x, uint32_t y, uint32_t w, const char *label, const char *what)
{
    lcd_fill(x + 1u, y, w - 2u, 36u, T_BG);
    if (what[0]) {                                      /* a mapped control: outlined */
        lcd_fill(x + 1u, y, w - 2u, 1u, T_DIM);
        lcd_fill(x + 1u, y + 35u, w - 2u, 1u, T_DIM);
        lcd_fill(x + 1u, y, 1u, 36u, T_DIM);
        lcd_fill(x + w - 2u, y, 1u, 36u, T_DIM);
    }
    draw_text_line(x + 2u, y + 3u, w - 4u, &AF_S, label, what[0] ? T_TEXT : T_DIM, T_BG, 1);
    draw_text_line(x + 2u, y + 19u, w - 4u, &AF_S, what[0] ? what : "-", what[0] ? T_THEME : T_DIM, T_BG, 1);
}

static void m8_draw(void)
{
    static const uint8_t ROW1[6] = {B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO};
    static const uint8_t ROW2[6] = {B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC};
    char t[24];
    uint32_t i;
    if (!ui.force && !m8.dirty)
        return;
    if (ui.force) {
        lcd_fill(0, 0, 240, 240, T_BG);
        draw_text_line(0, 6, 240, &AF_M, m8.fx == 0u ? "M8 MODE" : m8.fx == 1u ? "M8 MODE: FX A" : "M8 MODE: FX B", T_THEME, T_BG, 1);
        m8_cell(0, 72u, 120u, E_NAME[EN_SELECT], M8_ENC_MAP[EN_SELECT]);
        m8_cell(120u, 72u, 120u, E_NAME[EN_ALGO], M8_ENC_MAP[EN_ALGO]);
        for (i = 0; i < 6u; i++) {
            m8_cell(i * 40u, 120u, 40u, B_NAME[ROW1[i]], ROW1[i] == B_SCL && m8_track_keys() ? "TRACK" : M8_BTN_MAP[ROW1[i]]);
            m8_cell(i * 40u, 160u, 40u, B_NAME[ROW2[i]], M8_BTN_MAP[ROW2[i]]);
        }
    }
    for (i = 0; i < 4u; i++) {                          /* KNOB 1..4, as on the panel; on the FX page CC and value */
        if (m8.fx) {
            uint32_t k = 4u * (m8.fx - 1u) + i, v = m8.fx_val[k];
            t[0] = 'C'; t[1] = 'C';
            t[2] = (char)('0' + (M8_FX_CC0 + k) / 10u); t[3] = (char)('0' + (M8_FX_CC0 + k) % 10u);
            t[4] = ' ';
            t[5] = (char)('0' + v / 100u); t[6] = (char)('0' + v / 10u % 10u); t[7] = (char)('0' + v % 10u);
            t[8] = 0;
            m8_cell(i * 60u, 30u, 60u, E_NAME[EN_K1 + i], t);
        } else if (ui.force)
            m8_cell(i * 60u, 30u, 60u, E_NAME[EN_K1 + i], M8_ENC_MAP[EN_K1 + i]);
    }
    if (m8_track_keys()) {                              /* KEYS TRACK: T1 CH 1 EXT  OCT +0 */
        uint32_t tr = song.sel, c = trk_midi_ch(tr) + 1u, n = 0;
        int32_t o = song.octave;
        t[n++] = 'T'; t[n++] = (char)('1' + tr); t[n++] = ' ';
        if (trk_moff(tr)) {
            str_cpy(&t[n], "NO MIDI", sizeof t - n);
            n += 7u;
        } else {
            t[n++] = 'C'; t[n++] = 'H'; t[n++] = ' ';
            if (c >= 10u)
                t[n++] = '1';
            t[n++] = (char)('0' + c % 10u);
            if (trk_ext(tr)) {
                str_cpy(&t[n], " EXT", sizeof t - n);
                n += 4u;
            }
        }
        str_cpy(&t[n], "  OCT ", sizeof t - n);
        n += 6u;
        t[n++] = (char)(o < 0 ? '-' : '+');
        t[n++] = (char)('0' + (o < 0 ? -o : o));
        t[n] = 0;
    } else     {                                                   /* CH 16  OCT +0  VEL 100 */
        uint32_t c = m8_kch() + 1u, v = m8_vel, n = 0;
        t[n++] = 'C'; t[n++] = 'H'; t[n++] = ' ';
        if (c >= 10u)
            t[n++] = '1';
        t[n++] = (char)('0' + c % 10u);
        str_cpy(&t[n], "  OCT ", sizeof t - n);
        n += 6u;
        t[n++] = (char)(m8_oct < 0 ? '-' : '+');
        t[n++] = (char)('0' + (m8_oct < 0 ? -m8_oct : m8_oct));
        str_cpy(&t[n], "  VEL ", sizeof t - n);
        n += 6u;
        if (v >= 100u)
            t[n++] = '1';
        if (v >= 10u)
            t[n++] = (char)('0' + v / 10u % 10u);
        t[n++] = (char)('0' + v % 10u);
        t[n] = 0;
    }
    draw_text_line(0, 202, 240, &AF_S, t, T_TEXT, T_BG, 1);
    /* the last control used, or (none yet) the way out */
    draw_text_line(0, 224, 240, &AF_S, m8.last[0] ? m8.last : "HOME: MENU  TWICE: FM-1", m8.last[0] ? T_THEME : T_TEXT,
                   T_BG, 1);
    ui.force = 0;
    m8.dirty = 0;
}
