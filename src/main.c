/*
 * SOPWITH GB - a Game Boy (DMG) homage to the 1984 PC game "Sopwith"
 * by David L. Clark / BMB Compuscience.  Written from scratch for GBDK-2020.
 *
 * Controls
 *   UP / DOWN      pitch (relative to the pilot - like the original, flying
 *                  upside down reverses them)
 *   LEFT / RIGHT   throttle: the direction you face = faster, opposite = slower.
 *                  Stopped on the runway: opposite direction turns the plane around.
 *   A              machine gun (hold)
 *   B              drop bomb
 *   SELECT         flip the plane over (roll 180 degrees)
 *   START          pause
 *
 * Land slowly and level on your home runway (far left) to refuel and re-arm.
 * Destroy every ground target to complete the mission.
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "assets.h"

/* ------------------------------------------------------------ constants */
#define WORLD_W      (WORLD_COLS * 8)       /* world width in pixels (1536) */
#define FIX          4                      /* positions are 1/16 pixel     */
#define MAX_THR      10
#define MAX_SPEED    52
#define STALL_SPEED  14
#define TAKEOFF_SPD  18
#define LAND_MAX_SPD 30
#define FUEL_MAX     4096u
#define AMMO_MAX     96
#define BOMBS_MAX    5
#define START_LIVES  4

#define P_GROUND 0
#define P_FLY    1
#define P_FALL   2
#define P_DEAD   3

#define N_PBUL   5
#define N_EBUL   4
#define N_BOMB   3
#define N_ENEMY  3
#define N_DEB    8
#define N_BIRD   5

/* OAM slots */
#define OAM_PLAYER 0
#define OAM_ENEMY  2
#define OAM_PBUL   8
#define OAM_EBUL   13
#define OAM_BOMB   18
#define OAM_DEB    21
#define OAM_BIRD   29

#define MSG_ROW 4

/* |d| <= r, cheap unsigned trick */
#define NEAR(d, r) ((uint16_t)((d) + (r)) <= (uint16_t)(2 * (r)))

#define BIRD_X0 660
#define BIRD_X1 780

#ifdef PROFILE
uint8_t fl_ly0, fl_ly1, fl_bad, fl_bad0;
uint8_t prof[16], profsum[16], prof_prev;
uint16_t profacc[16];
#define PROF(n) { uint8_t ly_ = LY_REG; ly_ = (ly_ >= 144) ? ly_ - 144 : ly_ + 10; \
  if (n == 9) prof_prev = 0; \
  { uint8_t d_ = ly_ - prof_prev; if (d_ > prof[n]) prof[n] = d_; profacc[n] += d_; } prof_prev = ly_; }
#else
#define PROF(n)
#endif

/* ------------------------------------------------------------ types */
typedef struct {
    int16_t x, y;        /* 1/16 px                */
    int16_t xp, yp;      /* cached whole pixels    */
    int8_t  vx, vy;      /* 1/16 px per frame      */
    uint8_t life;
    uint8_t type;        /* sprite tile            */
} obj_t;

typedef struct {
    int16_t x, y, xp, yp;
    int8_t  vx, vy;
    uint8_t dir, vdir, spd, state, cd;
} enemy_t;

typedef struct {
    int16_t x0;          /* left pixel             */
    int16_t y0;          /* top pixel of hitbox    */
    uint8_t alive, hp;
} target_t;

typedef struct {
    int16_t x, y;        /* pixels */
    uint8_t alive;
} bird_t;

/* ------------------------------------------------------------ state */
uint8_t world[WORLD_COLS * WORLD_ROWS];   /* column-major tile map */
static uint8_t mm[16 * 12];                      /* minimap tile buffer   */
uint8_t frame;
uint16_t loops;
static uint8_t joy, joy_prev, joy_new;
static uint8_t rnd_s = 0xA5;

int16_t camx;
static int16_t loaded_c;

uint8_t level, lives, targets_left;
uint16_t score, hiscore;

/* player */
int16_t px, py, pxp, pyp;
static int8_t  pvx, pvy;
uint8_t pdir, pinv, pspeed, pthr, pstate, ptimer;
uint16_t pfuel;
uint8_t pammo, pbombs;
static uint8_t pitch_t, thr_t, fire_cd, bomb_cd, pv_dir = 0xFF, pv_spd;

enemy_t enemies[N_ENEMY];
static uint16_t espawn_t;
static uint8_t n_enemy;

static obj_t pbul[N_PBUL], ebul[N_EBUL], bombs[N_BOMB], debris[N_DEB];
/* number of live objects per pool (upper bound, recounted each update) and
   what was drawn last frame - lets us skip whole loops when a pool is idle */
static uint8_t n_pbul, n_ebul, n_bomb, n_deb, r_pbul, r_ebul, r_bomb, r_deb;
static uint8_t bird_rr;
static uint8_t deb_next;
static bird_t birds[N_BIRD];
static uint8_t birds_alive, birds_drawn;

target_t targets[NUM_TARGETS];

/* ------------------------------------------------------------ utils */
static uint8_t rnd(void) {
    uint8_t lsb = rnd_s & 1;
    rnd_s >>= 1;
    if (lsb) rnd_s ^= 0xB8;
    return rnd_s;
}

static uint8_t ground_y(int16_t x) {
    uint8_t c, f, a, b, y;
    if (x < 0) x = 0;
    else if (x >= WORLD_W) x = WORLD_W - 1;
    c = (uint8_t)((uint16_t)x >> 3);
    f = (uint8_t)x & 7;
    a = ground_vtx[c];
    b = ground_vtx[c + 1];
    y = a << 3;
    if (b < a) return y - f;
    if (b > a) return y + f;
    return y;
}

/* direction (0..15, 0 = right, 4 = up) from a vector, y down */
static uint8_t dir_to(int16_t dx, int16_t dy) {
    uint16_t ax, ay;
    uint8_t k;
    if (dx > 1000) dx = 1000; else if (dx < -1000) dx = -1000;
    if (dy > 1000) dy = 1000; else if (dy < -1000) dy = -1000;
    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    if ((ay << 2) + ay < ax) k = 0;
    else if ((ay << 1) + ay < (ax << 1)) k = 1;
    else if ((ay << 1) < (ax << 1) + ax) k = 2;
    else if (ay < (ax << 2) + ax) k = 3;
    else k = 4;
    if (dx >= 0) return (dy <= 0) ? k : (uint8_t)((16 - k) & 15);
    return (dy <= 0) ? (uint8_t)(8 - k) : (uint8_t)(8 + k);
}

/* rotate d one step towards target along the shortest way */
static uint8_t turn_toward(uint8_t d, uint8_t t) {
    uint8_t diff = (t - d) & 15;
    if (diff == 0) return d;
    if (diff < 8) return (d + 1) & 15;
    return (d - 1) & 15;
}

static uint8_t dir_diff(uint8_t a, uint8_t b) {
    uint8_t d = (a - b) & 15;
    return d > 8 ? 16 - d : d;
}

/* ------------------------------------------------------------ sound */
static uint8_t engine_on, engine_f;
static void snd_init(void) {
    NR52_REG = 0x80;
    NR50_REG = 0x77;
    NR51_REG = 0xFF;
}
static void snd_gun(void) {
    NR41_REG = 0x30; NR42_REG = 0x61; NR43_REG = 0x23; NR44_REG = 0xC0;
}
static void snd_explode(void) {
    NR41_REG = 0x00; NR42_REG = 0xF5; NR43_REG = 0x76; NR44_REG = 0x80;
}
static void snd_bomb(void) {
    NR10_REG = 0x7E; NR11_REG = 0x80; NR12_REG = 0xA6; NR13_REG = 0x00; NR14_REG = 0x87;
}
static void snd_hit(void) {
    NR10_REG = 0x1B; NR11_REG = 0x40; NR12_REG = 0xF3; NR13_REG = 0x80; NR14_REG = 0x86;
}
static void snd_blip(void) {
    NR10_REG = 0x00; NR11_REG = 0x80; NR12_REG = 0x81; NR13_REG = 0xC0; NR14_REG = 0x87;
}
static void engine_silence(void) {
    NR22_REG = 0x00; NR24_REG = 0x80; engine_on = 0;
}
static void engine_update(void) {
    uint8_t want = (pstate <= P_FLY) && (pthr > 0 || pspeed > 0);
    uint16_t f;
    if (!want) {
        if (engine_on) engine_silence();
        return;
    }
    if (engine_on && engine_f == pspeed + pthr) return;
    engine_f = pspeed + pthr;
    f = 180 + ((uint16_t)pspeed << 4) + ((uint16_t)pthr << 3);
    if (!engine_on) {
        NR21_REG = 0x00; NR22_REG = 0x30;
        NR23_REG = (uint8_t)f; NR24_REG = 0x80 | (uint8_t)(f >> 8);
        engine_on = 1;
    } else {
        NR23_REG = (uint8_t)f; NR24_REG = (uint8_t)(f >> 8);
    }
}
static void engine_off(void) { pthr = 0; pspeed = 0; engine_silence(); }

/* ------------------------------------------------------------ background */
static void load_col(int16_t c) {
    if (c < 0 || c >= WORLD_COLS) return;
    set_bkg_tiles((uint8_t)c & 31, 0, 1, WORLD_ROWS, &world[c * WORLD_ROWS]);
}

static void cam_full_load(void) {
    int16_t c;
    loaded_c = camx >> 3;
    for (c = loaded_c; c <= loaded_c + 20; c++) load_col(c);
    SCX_REG = (uint8_t)camx;
}

/* columns that must be written to VRAM during the next VBlank */
static uint8_t pend_n;
static int16_t pend_c[4];

static void cam_stream(void) {
    int16_t nc = camx >> 3;
    while (loaded_c < nc && pend_n < 4) { loaded_c++; pend_c[pend_n++] = loaded_c + 20; }
    while (loaded_c > nc && pend_n < 4) { loaded_c--; pend_c[pend_n++] = loaded_c; }
}

/* VBlank only: raw write of pending columns into the BG map */
static void flush_cols(void) {
    uint8_t i, n;
    int16_t c;
    uint8_t *d;
    const uint8_t *src;
    for (i = 0; i < pend_n; i++) {
        c = pend_c[i];
        if (c < 0 || c >= WORLD_COLS) continue;
        d = (uint8_t *)0x9800 + ((uint8_t)c & 31);
        src = &world[c * WORLD_ROWS];
        n = WORLD_ROWS;
        do { *d = *src++; d += 32; } while (--n);
    }
    pend_n = 0;
}

static void cam_update(void) {
    int16_t t = pxp - 80, d, s;
    if (pstate <= P_FLY) t += (cos64[pdir] >> 1);   /* look ahead */
    if (t < 0) t = 0;
    if (t > WORLD_W - 160) t = WORLD_W - 160;
    d = t - camx;
    if (d == 0) return;
    s = d >> 3;
    if (s == 0) s = d > 0 ? 1 : -1;
    else if (s > 5) s = 5;
    else if (s < -5) s = -5;
    camx += s;
    cam_stream();
}

static void refresh_world_cols(uint8_t col, uint8_t row, uint8_t n) {
    uint8_t i;
    for (i = 0; i < n; i++) {
        int16_t c = col + i;
        if (c >= loaded_c && c <= loaded_c + 20)
            set_bkg_tiles((uint8_t)c & 31, row, 1, 2, &world[c * WORLD_ROWS + row]);
    }
}

static void bg_text(const char *s, uint8_t row) {
    uint8_t len = (uint8_t)strlen(s), i;
    int16_t c0 = loaded_c + ((20 - len) >> 1) + ((camx & 7) ? 1 : 0);
    for (i = 0; i < len; i++)
        set_bkg_tile_xy((uint8_t)(c0 + i) & 31, row, charmap[s[i] - 32]);
}

static void bg_restore_row(uint8_t row) {
    int16_t c;
    for (c = loaded_c; c <= loaded_c + 20; c++)
        if (c >= 0 && c < WORLD_COLS)
            set_bkg_tile_xy((uint8_t)c & 31, row, world[c * WORLD_ROWS + row]);
}

/* ------------------------------------------------------------ HUD (window) */
static uint8_t hudbuf[20];
static uint16_t hud_score;
static uint8_t hud_f, hud_a, hud_b, hud_l, hud_lv;

static void num_to_tiles(uint8_t *dst, uint16_t v, uint8_t digits) {
    while (digits) {
        digits--;
        dst[digits] = charmap['0' - 32 + (uint8_t)(v % 10)];
        v /= 10;
    }
}

static void hud_bar(uint8_t *dst, uint8_t n, uint8_t val) {
    uint8_t i, v;
    for (i = 0; i < n; i++) {
        v = val > 8 ? 8 : val;
        dst[i] = BGT_BAR + v;
        val -= v;
    }
}

/* force = redraw everything; otherwise only what changed (and at most the
   expensive score update per call, to keep frame time low) */
static void hud_draw(uint8_t force) {
    uint8_t v;
    if (force) {
        for (v = 0; v < 12; v++) hudbuf[v] = BGT_MINIMAP + v;
        set_win_tiles(0, 0, 12, 1, hudbuf);
        hudbuf[0] = charmap['F' - 32]; set_win_tiles(0, 1, 1, 1, hudbuf);
        hudbuf[0] = charmap['A' - 32]; set_win_tiles(5, 1, 1, 1, hudbuf);
        hudbuf[0] = BGT_HUD_BOMB;      set_win_tiles(10, 1, 1, 1, hudbuf);
        hudbuf[0] = BGT_HUD_PLANE;     set_win_tiles(13, 1, 1, 1, hudbuf);
        hudbuf[0] = charmap['L' - 32]; set_win_tiles(16, 1, 1, 1, hudbuf);
    }
    if (force || hud_score != score) {
        hud_score = score;
        num_to_tiles(hudbuf, score, 5);
        set_win_tiles(15, 0, 5, 1, hudbuf);
        if (!force) return;
    }
    v = (uint8_t)(pfuel >> 7);
    if (force || v != hud_f) { hud_f = v; hud_bar(hudbuf, 4, v); set_win_tiles(1, 1, 4, 1, hudbuf); }
    v = pammo >> 2;
    if (force || v != hud_a) { hud_a = v; hud_bar(hudbuf, 3, v); set_win_tiles(6, 1, 3, 1, hudbuf); }
    if (force || pbombs != hud_b) { hud_b = pbombs; hudbuf[0] = charmap['0' - 32 + pbombs]; set_win_tiles(11, 1, 1, 1, hudbuf); }
    if (force || lives != hud_l) { hud_l = lives; hudbuf[0] = charmap['0' - 32 + lives]; set_win_tiles(14, 1, 1, 1, hudbuf); }
    if (force || level != hud_lv) { hud_lv = level; num_to_tiles(hudbuf, level, 2); set_win_tiles(17, 1, 2, 1, hudbuf); }
}

static void mm_plot(int16_t x, int16_t y, uint8_t col) {
    uint8_t mx, my, bit, *p;
    if (x < 0 || y < 0) return;
    mx = (uint8_t)((uint16_t)x >> 4); my = (uint8_t)((uint16_t)y >> 4);
    if (mx >= 96 || my >= 8) return;
    p = &mm[((mx >> 3) << 4) + (my << 1)];
    bit = 0x80 >> (mx & 7);
    if (col & 1) p[0] |= bit; else p[0] &= ~bit;
    if (col & 2) p[1] |= bit; else p[1] &= ~bit;
}

static void minimap_plot_targets(void) {
    uint8_t i;
    target_t *t = targets;
    for (i = 0; i < NUM_TARGETS; i++, t++)
        if (t->alive) mm_plot(t->x0 + 8, t->y0 + 2, 3);
}

static void minimap_plot_planes(void) {
    uint8_t i;
    enemy_t *e = enemies;
    if (frame & 16)
        for (i = 0; i < N_ENEMY; i++, e++)
            if (e->state) mm_plot(e->xp, e->yp, 3);
    if (pstate != P_DEAD) mm_plot(pxp, pyp, 3);
}

static void minimap_draw(void) {
    memcpy(mm, minimap_base, sizeof(mm));
    minimap_plot_targets();
    minimap_plot_planes();
    set_bkg_data(BGT_MINIMAP, 12, mm);
}

/* ------------------------------------------------------------ sprites */
static void hide_all(void) {
    uint8_t i;
    for (i = 0; i < 40; i++) shadow_OAM[i].y = 0;
}

/* draws a 16x16 plane centred on world pixel (xp,yp) into two OAM entries */
static void draw_plane(volatile OAM_item_t *s, int16_t xp, int16_t yp, uint8_t dir, uint8_t inv, uint8_t pal) {
    uint8_t img, flags, t;
    int16_t sx = xp - camx;
    if ((uint16_t)(sx - 1) >= 167 || yp < -8 || yp >= 128) { s[0].y = 0; s[1].y = 0; return; }
    if (!inv) {
        if (dir < 8) { img = dir; flags = 0; }
        else { img = dir - 8; flags = S_FLIPX | S_FLIPY; }
    } else {
        uint8_t e = (16 - dir) & 15;
        if (e < 8) { img = e; flags = S_FLIPY; }
        else { img = e - 8; flags = S_FLIPX; }
    }
    flags |= pal;
    t = SPR_PLANE + (img << 2);
    s[0].y = s[1].y = (uint8_t)(yp + 8);
    s[0].x = (uint8_t)sx;
    s[1].x = (uint8_t)sx + 8;
    s[0].prop = s[1].prop = flags;
    if (flags & S_FLIPX) { s[0].tile = t + 2; s[1].tile = t; }
    else { s[0].tile = t; s[1].tile = t + 2; }
}

/* 8x16 sprites whose "hot" pixel is at (ox,oy) inside the sprite.
   type bit 7 = draw X-flipped.  Hand-written in assembly: this loop runs for
   up to 20 objects every frame and SDCC's version was ~4x slower. */
uint8_t *d_s;
obj_t *d_o;
int16_t d_cx;
uint8_t d_oy, d_n, d_t;

static void draw_objs_asm(void) __naked {
    __asm
    ld  hl, #_d_s
    ld  a, (hl+)
    ld  e, a
    ld  d, (hl)
    ld  hl, #_d_o
    ld  a, (hl+)
    ld  h, (hl)
    ld  l, a
101$:
    push hl
    ld  bc, #10
    add hl, bc
    ld  a, (hl+)
    or  a, a
    jr  z, 110$
    ld  a, (hl)
    ld  (_d_t), a
    ld  bc, #0xFFF9
    add hl, bc
    ld  a, (_d_cx)
    ld  b, a
    ld  a, (hl+)
    sub a, b
    ld  c, a
    ld  a, (_d_cx+1)
    ld  b, a
    ld  a, (hl+)
    sbc a, b
    jr  nz, 110$
    ld  a, c
    dec a
    cp  a, #167
    jr  nc, 110$
    ld  a, (hl+)
    ld  b, a
    ld  a, (hl)
    or  a, a
    jr  nz, 110$
    ld  a, b
    cp  a, #128
    jr  nc, 110$
    ld  a, (_d_oy)
    add a, b
    ld  (de), a
    inc de
    ld  a, c
    ld  (de), a
    inc de
    ld  a, (_d_t)
    ld  b, a
    and a, #0x7F
    ld  (de), a
    inc de
    ld  a, b
    and a, #0x80
    rra
    rra
    ld  (de), a
    inc de
    jr  120$
110$:
    xor a, a
    ld  (de), a
    inc de
    inc de
    inc de
    inc de
120$:
    pop hl
    ld  bc, #12
    add hl, bc
    ld  a, (_d_n)
    dec a
    ld  (_d_n), a
    jr  nz, 101$
    ret
    __endasm;
}


/* moves every live object of a pool: life--, x += vx, y += vy, refresh
   xp/yp, kill when leaving the world (x) or the top of the screen.
   Objects whose type == d_gt also get gravity (vy++ up to 50). */
uint8_t d_gt;
static void move_objs_asm(void) __naked {
    __asm
    ld  hl, #_d_o
    ld  a, (hl+)
    ld  d, (hl)
    ld  e, a
201$:
    ld  hl, #10
    add hl, de
    ld  a, (hl)
    or  a, a
    jp  z, 290$
    dec a
    ld  (hl+), a
    jp  z, 290$
    ld  a, (_d_gt)
    cp  a, (hl)
    jr  nz, 205$
    dec hl
    dec hl
    ld  a, (hl)
    bit 7, a
    jr  nz, 204$
    cp  a, #50
    jr  nc, 205$
204$:
    inc (hl)
205$:
    ld  hl, #8
    add hl, de
    ld  a, (hl+)
    ld  c, a
    rlca
    sbc a, a
    ld  b, a
    ld  a, (hl)
    push af
    ld  h, d
    ld  l, e
    ld  a, (hl)
    add a, c
    ld  (hl+), a
    ld  a, (hl)
    adc a, b
    ld  (hl+), a
    pop af
    ld  c, a
    rlca
    sbc a, a
    ld  b, a
    ld  a, (hl)
    add a, c
    ld  (hl+), a
    ld  a, (hl)
    adc a, b
    ld  (hl), a
    ld  h, d
    ld  l, e
    ld  a, (hl+)
    ld  c, a
    ld  b, (hl)
    sra b
    rr  c
    sra b
    rr  c
    sra b
    rr  c
    sra b
    rr  c
    ld  a, b
    cp  a, #0x06
    jr  nc, 280$
    ld  hl, #4
    add hl, de
    ld  a, c
    ld  (hl+), a
    ld  a, b
    ld  (hl), a
    ld  hl, #2
    add hl, de
    ld  a, (hl+)
    ld  c, a
    ld  b, (hl)
    sra b
    rr  c
    sra b
    rr  c
    sra b
    rr  c
    sra b
    rr  c
    bit 7, b
    jr  nz, 280$
    ld  hl, #6
    add hl, de
    ld  a, c
    ld  (hl+), a
    ld  a, b
    ld  (hl), a
    jr  290$
280$:
    ld  hl, #10
    add hl, de
    xor a, a
    ld  (hl), a
290$:
    ld  hl, #12
    add hl, de
    ld  e, l
    ld  d, h
    ld  a, (_d_n)
    dec a
    ld  (_d_n), a
    jp  nz, 201$
    ret
    __endasm;
}

static void move_objs(obj_t *o, uint8_t n, uint8_t gt) {
    d_o = o; d_n = n; d_gt = gt;
    move_objs_asm();
}

static void draw_objs(volatile OAM_item_t *s, obj_t *o, uint8_t n, uint8_t ox, uint8_t oy) {
    d_s = (uint8_t *)s;
    d_o = o;
    d_n = n;
    d_cx = camx - 8 + ox;
    d_oy = 16 - oy;
    draw_objs_asm();
}

/* ------------------------------------------------------------ effects */
#define DEB_PARTICLE SPR_DEBRIS
#define DEB_EXPL     SPR_EXPL_A
#define DEB_SMOKE    SPR_SMOKE

static void spawn_debris(int16_t x, int16_t y, int8_t vx, int8_t vy, uint8_t type, uint8_t life) {
    obj_t *o = &debris[deb_next];
    deb_next = (deb_next + 1) & (N_DEB - 1);
    o->x = x; o->y = y; o->xp = x >> FIX; o->yp = y >> FIX;
    o->vx = vx; o->vy = vy; o->type = type; o->life = life;
    if (n_deb < N_DEB) n_deb++;
}

static void explosion(int16_t x, int16_t y, uint8_t pieces) {
    spawn_debris(x, y, 0, 0, DEB_EXPL, 20);
    while (pieces--)
        spawn_debris(x, y, (int8_t)((rnd() & 31) - 16), (int8_t)(-(int8_t)(rnd() & 31) - 8), DEB_PARTICLE, 50);
    snd_explode();
}

/* ------------------------------------------------------------ targets */
static void destroy_target(uint8_t i) {
    const uint8_t *ti = &target_init[i * 3];
    uint8_t col = ti[0], row = ti[1], t = ti[2];
    uint8_t *w = &world[col * WORLD_ROWS + row];
    targets[i].alive = 0;
    w[0] = BGT_RUBBLE; w[1] = BGT_RUBBLE + 2;
    w[WORLD_ROWS] = BGT_RUBBLE + 1; w[WORLD_ROWS + 1] = BGT_RUBBLE + 3;
    refresh_world_cols(col, row, 2);
    explosion((targets[i].x0 + 8) << FIX, (targets[i].y0 + 4) << FIX, t == T_FUEL ? 6 : 4);
    score += (t == T_FUEL) ? 150 : (t == T_HANGAR ? 200 : 100);
    targets_left--;
}

#define TARGET_MIN_Y (7 * 8 + 6)

/* returns target index hit by world pixel point, or 0xFF */
static uint8_t target_at(int16_t x, int16_t y) {
    uint8_t i;
    target_t *t;
    if (y < TARGET_MIN_Y) return 0xFF;
    t = targets;
    for (i = 0; i < NUM_TARGETS; i++, t++) {
        if (x < t->x0) break;          /* sorted by x */
        if ((uint16_t)(x - t->x0) < 16 && t->alive && (uint16_t)(y - t->y0) < 10) return i;
    }
    return 0xFF;
}

/* ------------------------------------------------------------ player */
static void spawn_player(void) {
    int16_t x = RUNWAY_HOME_X0 + 12;
    pxp = x;
    pyp = (int16_t)ground_y(x) - 6;
    px = pxp << FIX;
    py = pyp << FIX;
    pdir = 0; pinv = 0; pspeed = 0; pthr = 0; pstate = P_GROUND;
    pfuel = FUEL_MAX; pammo = AMMO_MAX; pbombs = BOMBS_MAX;
    pitch_t = thr_t = fire_cd = bomb_cd = 0;
    pvx = pvy = 0;
    pv_dir = 0xFF;
    camx = 0;
    pend_n = 0;
    /* big camera jump: reload the whole view with the LCD briefly off */
    DISPLAY_OFF;
    cam_full_load();
    DISPLAY_ON;
}

static void player_crash(void) {
    explosion(px, py, 6);
    pstate = P_DEAD;
    ptimer = 100;
    engine_off();
}

static void player_shot(void) {
    if (pstate == P_FLY) {
        pstate = P_FALL;
        snd_hit();
        engine_off();
    } else if (pstate == P_GROUND) {
        player_crash();
    }
}

static uint8_t fire_bullet(obj_t *o, uint8_t n, uint8_t *cnt, int16_t x, int16_t y, uint8_t dir, int8_t vx, int8_t vy) {
    int8_t c = cos64[dir], s = sin64[dir];
    for (; n; n--, o++) {
        if (!o->life) {
            o->x = x + ((int16_t)c << 1);
            o->y = y - ((int16_t)s << 1);
            o->xp = o->x >> FIX;
            o->yp = o->y >> FIX;
            o->vx = vx + c;
            o->vy = vy - s;
            o->life = 26;
            o->type = SPR_BULLET;
            (*cnt)++;
            snd_gun();
            return 1;
        }
    }
    return 0;
}

static void update_player(void) {
    uint8_t facing_right, gy;
    int8_t c;

    if (pstate == P_DEAD) return;

    if (pstate == P_FALL) {
        if ((frame & 3) == 0) pdir = (pdir + 1) & 15;
        if (pvy < 40) pvy++;
        px += pvx; py += pvy;
        pxp = px >> FIX; pyp = py >> FIX;
        if ((frame & 7) == 0) spawn_debris(px, py, 0, -4, DEB_SMOKE, 24);
        if (pxp < 4 || pxp > WORLD_W - 4) pvx = -pvx;
        if (pyp + 4 >= ground_y(pxp)) player_crash();
        return;
    }

    c = cos64[pdir];
    facing_right = (c > 0) || (c == 0 && !pinv);

    /* throttle */
    if (joy & (J_LEFT | J_RIGHT)) {
        if (thr_t == 0) {
            uint8_t fwd = (facing_right && (joy & J_RIGHT)) || (!facing_right && (joy & J_LEFT));
            if (fwd) {
                if (pthr < MAX_THR && pfuel) pthr++;
            } else if (pthr) {
                pthr--;
            } else if (pstate == P_GROUND && pspeed == 0) {
                /* turn around on the ground */
                pdir ^= 8; pinv ^= 1;
                thr_t = 20;
            }
            if (!thr_t) thr_t = 6;
        } else thr_t--;
    } else thr_t = 0;

    /* flip */
    if ((joy_new & J_SELECT) && pstate == P_FLY) pinv ^= 1;

    /* pitch */
    if (joy & (J_UP | J_DOWN)) {
        if (pitch_t == 0) {
            int8_t d = (joy & J_UP) ? 1 : -1;
            if (pinv) d = -d;
            if (pstate == P_FLY) {
                pdir = (pdir + d) & 15;
            } else if ((joy & J_UP) && pspeed >= TAKEOFF_SPD) {
                pdir = (pdir + d) & 15;
                pstate = P_FLY;
            }
            pitch_t = (pspeed > 36) ? 3 : 4;
        } else pitch_t--;
    } else pitch_t = 0;

    /* fuel */
    if (pthr && pfuel) {
        pfuel--;
        if (pfuel == 0) pthr = 0;
    }

    /* speed */
    if ((frame & 7) == 0) {
        uint8_t tgt = pthr << 2;
        if (pspeed < tgt) pspeed += (pstate == P_GROUND) ? 2 : 1;
        else if (pspeed > tgt) {
            if (pstate == P_GROUND) pspeed = (pspeed > 2) ? pspeed - 2 : 0;
            else pspeed--;
        }
    } else if (pstate == P_FLY && (frame & 7) == 4) {
        int8_t g = sin64[pdir] >> 5;
        int16_t ns = (int16_t)pspeed - g;
        if (ns < 0) ns = 0;
        if (ns > MAX_SPEED) ns = MAX_SPEED;
        pspeed = (uint8_t)ns;
    }

    /* stall: nose drops */
    if (pstate == P_FLY && pspeed < STALL_SPEED) {
        if ((frame & 7) == 0) pdir = turn_toward(pdir, 12);
        py += 10;
    }

    if (pdir != pv_dir || pspeed != pv_spd) {
        pv_dir = pdir; pv_spd = pspeed;
        pvx = (int8_t)(((int16_t)pspeed * cos64[pdir]) >> 6);
        pvy = (int8_t)(-(((int16_t)pspeed * sin64[pdir]) >> 6));
    }
    px += pvx;
    py += pvy;

    /* world edges: the plane is turned around (mirrored) */
    if (px < (8 << FIX) || px > ((WORLD_W - 8) << FIX)) {
        px = (px < (8 << FIX)) ? (8 << FIX) : ((WORLD_W - 8) << FIX);
        if (pstate == P_FLY) { pdir = (8 - pdir) & 15; pinv ^= 1; }
    }
    if (py < (6 << FIX)) { py = 6 << FIX; if (pspeed > 2) pspeed -= 2; }

    pxp = px >> FIX;
    pyp = py >> FIX;
    gy = ground_y(pxp);

    if (pstate == P_GROUND) {
        if (pxp < RUNWAY_HOME_X0 + 4) { pxp = RUNWAY_HOME_X0 + 4; px = pxp << FIX; pspeed = 0; pthr = 0; }
        pyp = (int16_t)gy - 6;
        py = pyp << FIX;
        if (pspeed && (ground_y(pxp + 8) != gy || ground_y(pxp - 8) != gy)) { player_crash(); return; }
        if (pspeed == 0 && pxp >= RUNWAY_HOME_X0 && pxp < RUNWAY_HOME_X1) {
            /* refuel & re-arm */
            if (pfuel < FUEL_MAX) { pfuel += 24; if (pfuel > FUEL_MAX) pfuel = FUEL_MAX; }
            if (pammo < AMMO_MAX && (frame & 1)) pammo++;
            if (pbombs < BOMBS_MAX && (frame & 31) == 0) { pbombs++; snd_blip(); }
        }
    } else if (pyp + 5 >= (int16_t)gy) {
        uint8_t level_ok = (!pinv && (pdir == 0 || pdir == 1 || pdir == 15)) ||
                           (pinv && (pdir == 7 || pdir == 8 || pdir == 9));
        uint8_t flat = ground_y(pxp + 8) == gy && ground_y(pxp - 8) == gy;
        if (level_ok && flat && pspeed <= LAND_MAX_SPD && pxp >= RUNWAY_HOME_X0 && pxp < RUNWAY_HOME_X1) {
            pstate = P_GROUND;
            pdir = pinv ? 8 : 0;
            pyp = (int16_t)gy - 6;
            py = pyp << FIX;
            if (pthr > 4) pthr = 4;
        } else {
            player_crash();
            return;
        }
    }

    if (pstate == P_FLY && target_at(pxp, pyp + 3) != 0xFF) { player_crash(); return; }

    /* weapons */
    if (fire_cd) fire_cd--;
    if (bomb_cd) bomb_cd--;
    if (pstate != P_FLY) return;
    if ((joy & J_A) && !fire_cd && pammo) {
        if (fire_bullet(pbul, N_PBUL, &n_pbul, px, py, pdir, pvx, pvy)) pammo--;
        fire_cd = 6;
    }
    if ((joy_new & J_B) && !bomb_cd && pbombs) {
        uint8_t i;
        obj_t *o = bombs;
        for (i = 0; i < N_BOMB; i++, o++) {
            if (!o->life) {
                o->life = 1;
                o->x = px; o->y = py + (5 << FIX);
                o->xp = pxp; o->yp = pyp + 5;
                o->vx = pvx; o->vy = pvy > 0 ? pvy : 0;
                o->type = SPR_BOMB_V;
                n_bomb++;
                pbombs--;
                bomb_cd = 15;
                snd_bomb();
                break;
            }
        }
    }
}

/* ------------------------------------------------------------ enemies */
static void spawn_enemy(void) {
    uint8_t i;
    enemy_t *e = enemies;
    for (i = 0; i < N_ENEMY; i++, e++) {
        if (!e->state) {
            e->state = 1;
            e->xp = ENEMY_BASE_X;
            e->yp = (int16_t)ground_y(ENEMY_BASE_X) - 24;
            e->x = e->xp << FIX;
            e->y = e->yp << FIX;
            e->dir = 7;
            e->vdir = 0xFF;
            e->spd = 24 + (level << 1);
            if (e->spd > 40) e->spd = 40;
            e->cd = 60;
            n_enemy++;
            return;
        }
    }
}

static void enemy_ai(enemy_t *e) {
    int16_t dx = pxp - e->xp, dy = pyp - e->yp, alt;
    uint8_t want;
    alt = (int16_t)ground_y(e->xp) - e->yp;
    if (alt < 30 || (alt < 50 && e->dir > 8)) want = 4;
    else if (e->yp < 14) want = (e->dir < 4 || e->dir > 12) ? 0 : 8;
    else if (pstate >= P_FALL) want = (e->xp > WORLD_W / 2) ? 7 : 1;
    else if (e->xp < 24) want = 0;
    else if (e->xp > WORLD_W - 24) want = 8;
    else if (!NEAR(dx, 180)) want = dir_to(dx, dy >> 2);
    else want = dir_to(dx, dy);
    e->dir = turn_toward(e->dir, want);
    if (level > 2) e->dir = turn_toward(e->dir, want);
}

static void update_enemies(void) {
    uint8_t i, active = 0, maxe;
    enemy_t *e = enemies;
    for (i = 0; i < N_ENEMY; i++, e++) {
        if (!e->state) continue;
        active++;
        if (e->state == 2) {   /* falling */
            if ((frame & 3) == 0) e->dir = (e->dir + 15) & 15;
            if (e->vy < 40) e->vy++;
            e->x += e->vx; e->y += e->vy;
            e->xp = e->x >> FIX; e->yp = e->y >> FIX;
            if ((frame & 7) == 0) spawn_debris(e->x, e->y, 0, -4, DEB_SMOKE, 24);
            if (e->yp + 4 >= ground_y(e->xp)) {
                uint8_t t;
                explosion(e->x, e->y, 4);
                t = target_at(e->xp, e->yp);
                if (t != 0xFF) destroy_target(t);
                e->state = 0;
            }
            continue;
        }
        if (((frame + (i << 2)) & 7) == 0) enemy_ai(e);
        if (e->cd) e->cd--;
        else if (((frame + i) & 3) == 0 && pstate <= P_FLY) {
            int16_t dx = pxp - e->xp, dy = pyp - e->yp;
            if (NEAR(dx, 100) && NEAR(dy, 64) && dir_diff(e->dir, dir_to(dx, dy)) <= 1) {
                fire_bullet(ebul, N_EBUL, &n_ebul, e->x, e->y, e->dir, e->vx, e->vy);
                e->cd = (level > 4) ? 14 : 30 - level * 3;
            }
        }
        if (e->dir != e->vdir) {
            e->vdir = e->dir;
            e->vx = (int8_t)(((int16_t)e->spd * cos64[e->dir]) >> 6);
            e->vy = (int8_t)(-(((int16_t)e->spd * sin64[e->dir]) >> 6));
        }
        e->x += e->vx;
        e->y += e->vy;
        e->xp = e->x >> FIX;
        e->yp = e->y >> FIX;
        if (e->yp + 5 >= ground_y(e->xp) || target_at(e->xp, e->yp + 3) != 0xFF) {
            explosion(e->x, e->y, 4);
            e->state = 0;
            continue;
        }
        /* mid-air collision */
        if (pstate == P_FLY && NEAR(pxp - e->xp, 9) && NEAR(pyp - e->yp, 7)) {
            explosion(e->x, e->y, 4);
            e->state = 0;
            player_crash();
        }
    }
    /* spawning */
    n_enemy = active;
    maxe = level < 3 ? level + 1 : 3;
    if (espawn_t) espawn_t--;
    else if (active < maxe && pstate <= P_FLY) {
        spawn_enemy();
        espawn_t = (level < 6) ? 420 - level * 50 : 120;
    }
}

/* ------------------------------------------------------------ projectiles */
static void update_bullets(void) {
    uint8_t i, j, t;
    int16_t x, y;
    obj_t *o = pbul;
    enemy_t *e;
    bird_t *b;
    uint8_t n;
    if (n_pbul) {
    move_objs(pbul, N_PBUL, 0xFF);
    n = 0;
    for (i = 0; i < N_PBUL; i++, o++) {
        if (!o->life) continue;
        x = o->xp; y = o->yp;
        if (y >= GROUND_MIN_Y && y >= (int16_t)ground_y(x)) { o->life = 0; continue; }
        n++;
        e = enemies;
        if (n_enemy) for (j = 0; j < N_ENEMY; j++, e++) {
            if (e->state == 1 && NEAR(e->xp - x, 6) && NEAR(e->yp - y, 5)) {
                e->state = 2;
                snd_hit();
                score += 50;
                o->life = 0;
                break;
            }
        }
        if (!o->life) continue;
        /* static things (birds, buildings) are checked on alternate frames */
        if ((i ^ frame) & 1) continue;
        if (birds_alive && x >= BIRD_X0 && x < BIRD_X1) {
            b = birds;
            for (j = 0; j < N_BIRD; j++, b++) {
                if (b->alive && NEAR(b->x - x, 3) && NEAR(b->y - y, 3)) {
                    b->alive = 0;
                    birds_alive--;
                    spawn_debris(o->x, o->y, 4, -12, DEB_PARTICLE, 40);
                    o->life = 0;
                    break;
                }
            }
            if (!o->life) continue;
        }
        t = target_at(x, y);
        if (t != 0xFF) {
            o->life = 0;
            if (--targets[t].hp == 0) destroy_target(t);
        }
    }
    n_pbul = n;
    }
    if (n_ebul) {
    n = 0;
    move_objs(ebul, N_EBUL, 0xFF);
    o = ebul;
    for (i = 0; i < N_EBUL; i++, o++) {
        if (!o->life) continue;
        if (o->yp >= GROUND_MIN_Y && o->yp >= (int16_t)ground_y(o->xp)) { o->life = 0; continue; }
        n++;
        if (pstate <= P_FLY && NEAR(pxp - o->xp, 5) && NEAR(pyp - o->yp, 4)) {
            o->life = 0;
            player_shot();
        }
    }
    n_ebul = n;
    }
}

static void update_bombs(void) {
    uint8_t i, j, t;
    int16_t x, y;
    int8_t ax;
    obj_t *o = bombs;
    target_t *tg;
    uint8_t n = 0;
    if (!n_bomb) return;
    for (i = 0; i < N_BOMB; i++, o++) {
        if (!o->life) continue;
        n++;
        if (o->vy < 60) o->vy++;
        if ((frame & 15) == 0) { if (o->vx > 0) o->vx--; else if (o->vx < 0) o->vx++; }
        o->x += o->vx;
        o->y += o->vy;
        x = o->xp = o->x >> FIX;
        y = o->yp = o->y >> FIX;
        if ((uint16_t)x >= WORLD_W) { o->life = 0; continue; }
        ax = o->vx < 0 ? -o->vx : o->vx;
        o->type = (ax * 2 > o->vy) ? (o->vx < 0 ? (SPR_BOMB_D | 0x80) : SPR_BOMB_D) : SPR_BOMB_V;
        t = target_at(x, y);
        if (t != 0xFF || y >= (int16_t)ground_y(x)) {
            o->life = 0;
            explosion(o->x, o->y, 3);
            /* blast radius */
            tg = targets;
            for (j = 0; j < NUM_TARGETS; j++, tg++) {
                if (tg->alive && NEAR(tg->x0 + 8 - x, 13) && NEAR(tg->y0 + 6 - y, 15)) destroy_target(j);
            }
        }
    }
    n_bomb = n;
}

static void update_debris(void) {
    uint8_t i;
    obj_t *o = debris;
    uint8_t n = 0;
    if (!n_deb) return;
    move_objs(debris, N_DEB, DEB_PARTICLE);
    for (i = 0; i < N_DEB; i++, o++) {
        if (!o->life) continue;
        n++;
        if (o->type == DEB_PARTICLE) {
            if (((i ^ frame) & 1) == 0 && o->yp >= GROUND_MIN_Y && o->yp >= (int16_t)ground_y(o->xp)) o->life = 0;
        } else if (o->type == DEB_EXPL && o->life == 10) {
            o->type = SPR_EXPL_B;
        }
    }
    n_deb = n;
}

static void init_birds(void) {
    uint8_t i;
    bird_t *b = birds;
    for (i = 0; i < N_BIRD; i++, b++) {
        b->alive = 1;
        b->x = 700 + (rnd() & 31);
        b->y = 30 + (rnd() & 15);
    }
    birds_alive = N_BIRD;
}

static void update_birds(void) {
    uint8_t r;
    bird_t *b;
    if (!birds_alive) return;
    /* move one bird per frame */
    if (++bird_rr >= N_BIRD) bird_rr = 0;
    b = &birds[bird_rr];
    if (b->alive) {
        r = rnd();
        b->x += (int16_t)(r & 3) - 1;
        if (b->x > 740) b->x--; else if (b->x < 680) b->x++;
        r >>= 2;
        b->y += (int16_t)(r & 3) - 1;
        if (b->y > 50) b->y--; else if (b->y < 20) b->y++;
    }
    /* collision with the player only when near the flock */
    if (pstate == P_FLY && pxp >= BIRD_X0 && pxp < BIRD_X1 && pyp < 64) {
        uint8_t i;
        b = birds;
        for (i = 0; i < N_BIRD; i++, b++) {
            if (b->alive && NEAR(pxp - b->x, 5) && NEAR(pyp - b->y, 4)) {
                b->alive = 0;
                birds_alive--;
                player_shot();
            }
        }
    }
}

/* ------------------------------------------------------------ rendering */
static void render(void) {
    uint8_t i;
    volatile OAM_item_t *s;
    enemy_t *e;
    bird_t *b;

    if (pstate != P_DEAD) draw_plane(&shadow_OAM[OAM_PLAYER], pxp, pyp, pdir, pinv, 0);
    else shadow_OAM[OAM_PLAYER].y = shadow_OAM[OAM_PLAYER + 1].y = 0;

    PROF(10)
    s = &shadow_OAM[OAM_ENEMY];
    e = enemies;
    for (i = 0; i < N_ENEMY; i++, e++, s += 2) {
        if (e->state) {
            uint8_t d = e->dir;
            draw_plane(s, e->xp, e->yp, d, (d >= 5 && d <= 11) ? 1 : 0, S_PALETTE);
        } else s[0].y = s[1].y = 0;
    }

    PROF(11)
    if (n_pbul | r_pbul) { draw_objs(&shadow_OAM[OAM_PBUL], pbul, N_PBUL, 3, 7); r_pbul = n_pbul; }
    if (n_ebul | r_ebul) { draw_objs(&shadow_OAM[OAM_EBUL], ebul, N_EBUL, 3, 7); r_ebul = n_ebul; }
    if (n_bomb | r_bomb) { draw_objs(&shadow_OAM[OAM_BOMB], bombs, N_BOMB, 4, 8); r_bomb = n_bomb; }
    PROF(12)
    if (n_deb | r_deb)   { draw_objs(&shadow_OAM[OAM_DEB], debris, N_DEB, 4, 8);  r_deb = n_deb; }

    PROF(13)
    s = &shadow_OAM[OAM_BIRD];
    if (birds_alive && camx < BIRD_X1 && camx + 168 > BIRD_X0) {
        birds_drawn = 1;
        uint8_t fl = (frame >> 3) & 1;
        int16_t sx;
        b = birds;
        for (i = 0; i < N_BIRD; i++, b++, s++) {
            sx = b->x - camx + 4;
            if (b->alive && (uint16_t)(sx - 1) < 167) {
                s->y = (uint8_t)(b->y + 9);
                s->x = (uint8_t)sx;
                s->tile = ((fl + i) & 1) ? SPR_BIRD_A : SPR_BIRD_B;
                s->prop = 0;
            } else s->y = 0;
        }
    } else if (birds_drawn) {
        birds_drawn = 0;
        for (i = 0; i < N_BIRD; i++, s++) s->y = 0;
    }
}

/* ------------------------------------------------------------ flow */
static void read_input(void) {
    joy_prev = joy;
    joy = joypad();
    joy_new = joy & ~joy_prev;
}

static void wait_frames(uint8_t n) {
    while (n--) wait_vbl_done();
}

/* shows a message in the sky; waits for START or timeout (0 = wait forever) */
static void message(const char *s, const char *s2, uint16_t timeout) {
    bg_text(s, MSG_ROW);
    if (s2) bg_text(s2, MSG_ROW + 2);
    wait_frames(20);
    read_input();
    while (1) {
        wait_vbl_done();
        read_input();
        if (joy_new & J_START) break;
        if (timeout) { if (--timeout == 0) break; }
    }
    bg_restore_row(MSG_ROW);
    bg_restore_row(MSG_ROW + 2);
}

static void clear_objects(void) {
    uint8_t i;
    for (i = 0; i < N_ENEMY; i++) enemies[i].state = 0;
    for (i = 0; i < N_PBUL; i++) pbul[i].life = 0;
    for (i = 0; i < N_EBUL; i++) ebul[i].life = 0;
    for (i = 0; i < N_BOMB; i++) bombs[i].life = 0;
    for (i = 0; i < N_DEB; i++) debris[i].life = 0;
    n_pbul = n_ebul = n_bomb = n_deb = 0;
    r_pbul = r_ebul = r_bomb = r_deb = 1;   /* force one hiding pass */
}

static void start_level(void) {
    uint8_t i;
    char buf[11];
    target_t *t = targets;
    memcpy(world, world_init, sizeof(world));
    for (i = 0; i < NUM_TARGETS; i++, t++) {
        t->alive = 1;
        t->x0 = (int16_t)target_init[i * 3] * 8;
        t->y0 = (int16_t)target_init[i * 3 + 1] * 8 + 6;
        t->hp = (target_init[i * 3 + 2] == T_HANGAR) ? 12 : 6;
    }
    targets_left = NUM_TARGETS;
    clear_objects();
    init_birds();
    espawn_t = 480;
    spawn_player();
    hide_all();
    render();
    minimap_draw();
    hud_draw(1);
    strcpy(buf, "MISSION 00");
    buf[8] = '0' + level / 10;
    buf[9] = '0' + level % 10;
    message(buf, "GOOD LUCK!", 120);
}

static void title_screen(void) {
    char hs[] = "HI 00000";
    uint16_t v = hiscore;
    uint8_t i;
    HIDE_WIN;
    memcpy(world, world_init, sizeof(world));
    camx = 0;
    cam_full_load();
    hide_all();
    pxp = RUNWAY_HOME_X0 + 60;
    pyp = (int16_t)ground_y(pxp) - 6;
    draw_plane(&shadow_OAM[OAM_PLAYER], pxp, pyp, 0, 0, 0);
    for (i = 7; i > 2; i--) { hs[i] = '0' + v % 10; v /= 10; }
    bg_text("S O P W I T H", 1);
    bg_text("A:GUN  B:BOMB", 3);
    bg_text("UP/DOWN:PITCH", 4);
    bg_text("LEFT/RIGHT:SPEED", 5);
    bg_text("SELECT:FLIP", 6);
    bg_text(hs, 8);
    while (1) {
        wait_vbl_done();
        frame++;
        rnd();
        read_input();
        if ((frame & 31) == 0) bg_text("PRESS START", 10);
        else if ((frame & 31) == 20) bg_restore_row(10);
        if (joy_new & J_START) break;
    }
    rnd_s ^= frame ^ DIV_REG;
    if (!rnd_s) rnd_s = 1;
    SHOW_WIN;
}

static void play_game(void) {
    uint8_t ph;
    score = 0;
    lives = START_LIVES;
    level = 1;
    start_level();
    while (1) {
        wait_vbl_done();
#ifdef PROFILE
        fl_ly0 = LY_REG;
#endif
        flush_cols();
        SCX_REG = (uint8_t)camx;
#ifdef PROFILE
        fl_ly1 = LY_REG;
        if (fl_ly1 < 144 && fl_ly1 >= fl_bad) fl_bad = fl_ly1 + 1;
        if (fl_ly0 < 144) fl_bad0++;
#endif
        /* still in VBlank: VRAM is free, copy a minimap slice directly */
        ph = frame & 15;
        if (ph >= 3 && ph <= 6)
            memcpy((uint8_t *)0x8000 + (BGT_MINIMAP + (ph - 3) * 3) * 16, &mm[(ph - 3) * 48], 48);
        frame++;
        loops++;
        read_input();

        if (joy_new & J_START) {
            engine_silence();
            message("PAUSED", 0, 0);
        }
        PROF(9)
        update_player();  PROF(0)
        update_enemies(); PROF(1)
        update_bullets(); PROF(2)
        update_bombs();   PROF(3)
        update_debris();  PROF(4)
        update_birds();   PROF(5)
        cam_update();     PROF(6)
        render();         PROF(7)
        engine_update();

        /* spread minimap / HUD work over several frames */
        ph = frame & 15;
        if (ph == 0) memcpy(mm, minimap_base, sizeof(mm));
        else if (ph == 1) minimap_plot_targets();
        else if (ph == 2) minimap_plot_planes();
        else if (ph == 8 || ph == 12) hud_draw(0);
        PROF(8)

        if (pstate == P_DEAD) {
            if (--ptimer == 0) {
                lives--;
                if (lives == 0) {
                    hud_draw(1);
                    if (score > hiscore) hiscore = score;
                    message("GAME OVER", "PRESS START", 600);
                    return;
                }
                clear_objects();
                espawn_t = 360;
                spawn_player();
                hud_draw(1);
            }
        }

        if (targets_left == 0 && pstate != P_DEAD && pstate != P_FALL) {
            wait_frames(60);
            score += 500;
            if (lives < 9) lives++;
            engine_off();
            hud_draw(1);
            message("MISSION COMPLETE!", "BONUS 500", 240);
            if (level < 99) level++;
            start_level();
        }
    }
}

void main(void) {
    DISPLAY_OFF;
    LCDC_REG |= LCDCF_BG8000;     /* BG & sprites share tiles at 0x8000 */
    SPRITES_8x16;
    BGP_REG = 0xE4;
    OBP0_REG = 0xE4;              /* player: light / dark / black        */
    OBP1_REG = 0x9C;              /* enemy : black / dark / light swapped */
    set_sprite_data(0, SPR_TILE_COUNT, spr_tiles);
    set_bkg_data(BG_TILE_BASE, BG_TILE_COUNT, bg_tiles);
    fill_win_rect(0, 0, 20, 2, BGT_FONT);
    fill_bkg_rect(0, 16, 32, 16, BGT_DIRT);
    move_win(7, 128);
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;
    snd_init();

    while (1) {
        title_screen();
        play_game();
        engine_off();
        hide_all();
    }
}
