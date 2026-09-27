#!/usr/bin/env python3
"""Generates src/assets.c and src/assets.h for Sopwith GB.

All graphics are original pixel art / procedurally rasterised shapes.
Run:  python3 tools/gen_assets.py   (from the project root)
"""
import math, random, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "src", "assets.c")
OUT_H = os.path.join(ROOT, "src", "assets.h")

# ---------------------------------------------------------------- helpers
def tile_from_rows(rows):
    """rows: 8 strings of 8 chars '.0123' -> 16 bytes 2bpp"""
    assert len(rows) == 8, rows
    out = []
    for r in rows:
        assert len(r) == 8, r
        lo = hi = 0
        for i, ch in enumerate(r):
            v = 0 if ch == '.' else int(ch)
            if v & 1: lo |= 0x80 >> i
            if v & 2: hi |= 0x80 >> i
        out += [lo, hi]
    return out

def grid_to_tiles(grid, order="rows"):
    """grid: list of strings, w,h multiples of 8.
    order 'rows'  -> tiles row-major (BG use)
    order 'cols16'-> 8x16 sprite order: column by column, top then bottom"""
    h = len(grid); w = len(grid[0])
    for g in grid: assert len(g) == w, (g, w)
    tiles = []
    if order == "rows":
        for ty in range(h // 8):
            for tx in range(w // 8):
                tiles.append(tile_from_rows([grid[ty*8+y][tx*8:tx*8+8] for y in range(8)]))
    else:
        for tx in range(w // 8):
            for ty in range(h // 8):
                tiles.append(tile_from_rows([grid[ty*8+y][tx*8:tx*8+8] for y in range(8)]))
    return tiles

def pix_to_grid(p):
    return ["".join('.' if v == 0 else str(v) for v in row) for row in p]

# ---------------------------------------------------------------- plane
# Vector biplane in plane-local coords: +x = nose, +y = up (pilot's head).
def in_poly(x, y, pts):
    c = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]; xj, yj = pts[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / (yj - yi) + xi):
            c = not c
        j = i
    return c

def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

PLANE_SHAPES = [  # painted in order, later on top: (color, kind, data)
    (1, 'poly', rect(5.0, -3.8, 6.2, 3.8)),                                   # prop disc (light)
    (3, 'poly', rect(0.0, -3.6, 1.0, -1.0)),                                  # gear strut
    (3, 'circ', (0.5, -4.4, 1.2)),                                            # wheel
    (2, 'poly', [(-7.5, 0.6), (-2.0, 1.6), (4.0, 1.6), (5.2, 0.8), (5.2, -1.0),
                 (4.0, -1.6), (-2.0, -1.4), (-7.5, -0.6)]),                    # fuselage
    (2, 'poly', [(-7.8, 0.0), (-7.8, 4.2), (-6.0, 4.2), (-4.4, 1.0)]),         # tail fin
    (3, 'poly', rect(-2.0, 3.0, 4.0, 4.4)),                                    # upper wing
    (3, 'poly', rect(0.6, 1.2, 1.6, 3.2)),                                     # strut
    (3, 'poly', rect(-2.0, -1.0, 3.2, 0.2)),                                   # lower wing
]

def shape_color(x, y, shapes):
    col = 0
    for c, kind, d in shapes:
        if kind == 'poly':
            if in_poly(x, y, d): col = c
        else:
            cx, cy, r = d
            if (x-cx)**2 + (y-cy)**2 <= r*r: col = c
    return col

def raster(shapes, angle_deg, size=16, ss=6, thresh=0.40):
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    img = []
    for py in range(size):
        row = []
        for px in range(size):
            counts = {}
            for sy in range(ss):
                for sx in range(ss):
                    # screen coords relative to centre, y up
                    X = (px + (sx + 0.5) / ss) - size / 2
                    Y = size / 2 - (py + (sy + 0.5) / ss)
                    # inverse rotate into plane space
                    lx = X * ca + Y * sa
                    ly = -X * sa + Y * ca
                    c = shape_color(lx, ly, shapes)
                    if c: counts[c] = counts.get(c, 0) + 1
            tot = sum(counts.values())
            if tot / (ss*ss) >= thresh:
                # darkest colour with decent share wins (keeps outlines crisp)
                best = max(counts, key=lambda k: counts[k] + k*0.5)
                row.append(best)
            else:
                row.append(0)
        img.append(row)
    return img

plane_imgs = [raster(PLANE_SHAPES, k * 22.5) for k in range(8)]

# ---------------------------------------------------------------- sprites
spr_tiles = []  # list of 16-byte tiles, sprite 8x16 mode (pairs)
def add_sprite16(grid):
    """grid 16 rows x (8*n) cols, returns first tile index"""
    idx = len(spr_tiles)
    spr_tiles.extend(grid_to_tiles(grid, "cols16"))
    return idx

SPR = {}
for k, im in enumerate(plane_imgs):
    t = add_sprite16(pix_to_grid(im))
    if k == 0: SPR['PLANE'] = t

def pad16(rows):
    return rows + ["........"] * (16 - len(rows))

SPR['BULLET'] = add_sprite16(pad16(["........"]*7 + ["...33..."] + ["...33..."]))
SPR['BOMB_V'] = add_sprite16(pad16(["........"]*5 + [
    "...3.3..",
    "....3...",
    "...222..",
    "...232..",
    "...232..",
    "....2...",
]))
SPR['BOMB_D'] = add_sprite16(pad16(["........"]*5 + [
    "..3.....",
    "...32...",
    "...222..",
    "....232.",
    ".....22.",
    "........",
]))
SPR['DEBRIS'] = add_sprite16(pad16(["........"]*6 + ["...23...", "...33...", "....2..."]))
SPR['EXPL_A'] = add_sprite16([
    "........",
    "........",
    "........",
    "...3....",
    ".3.2..3.",
    "..212.3.",
    ".3111...",
    "3211123.",
    "..1111..",
    ".32112..",
    "..3.2.3.",
    ".3..3...",
    "........",
    "........",
    "........",
    "........",
])
SPR['EXPL_B'] = add_sprite16([
    "........",
    "........",
    "3...3..3",
    "..3.2.3.",
    ".2.2.2..",
    "3.2...2.",
    "..2.1.2.",
    "32.1.1.3",
    "..2.1.2.",
    ".2..2.2.",
    "3.2.2..3",
    "..3.3...",
    ".3...3..",
    "........",
    "........",
    "........",
])
SPR['BIRD_A'] = add_sprite16(pad16(["........"]*6 + [
    "3.....3.",
    ".3...3..",
    "..333...",
]))
SPR['BIRD_B'] = add_sprite16(pad16(["........"]*6 + [
    "........",
    "........",
    ".33333..",
    "3.....3.",
]))
SPR['SMOKE'] = add_sprite16(pad16(["........"]*6 + ["..11....", ".1221...", ".1221...", "..11...."]))
assert len(spr_tiles) <= 64, len(spr_tiles)

# ---------------------------------------------------------------- BG tiles
BG_BASE = 64
bg_tiles = []
BG = {}
def add_bg(name, rows_or_tiles):
    BG[name] = BG_BASE + len(bg_tiles)
    if isinstance(rows_or_tiles[0], str):
        bg_tiles.append(tile_from_rows(rows_or_tiles))
    else:
        bg_tiles.extend(rows_or_tiles)

add_bg('SKY', ["........"] * 8)
DIRT = [
    "11111111",
    "11121111",
    "11111111",
    "11111121",
    "11111111",
    "12111111",
    "11111111",
    "11112111",
]
add_bg('DIRT', DIRT)
add_bg('GRASS', ["22222222", "21212122"] + DIRT[2:])
def slope(up):
    rows = []
    for ly in range(8):
        r = ""
        for px in range(8):
            surf = (8 - px) if up else px   # local y of surface in this tile
            if ly < surf: r += "."
            elif ly < surf + 2: r += "2"
            else: r += DIRT[ly][px]
        rows.append(r)
    return rows
add_bg('SLOPE_UP', slope(True))
add_bg('SLOPE_DN', slope(False))
add_bg('RUNWAY', ["33333333", "22022202", "22222222"] + DIRT[3:])

def add_bg16(name, grid):
    BG[name] = BG_BASE + len(bg_tiles)
    bg_tiles.extend(grid_to_tiles(grid, "rows"))   # order: TL, TR, BL, BR

add_bg16('FACTORY', [
    "................",
    ".33.............",
    ".33.............",
    ".33.............",
    ".33..3...3...3..",
    ".33.33..33..33..",
    ".3333333333333..",
    "3222222222222223",
    "3211221122112123",
    "3211221122112123",
    "3222222222222223",
    "3222333322222223",
    "3222311322112223",
    "3222311322112223",
    "3222311322222223",
    "3333333333333333",
])
add_bg16('FUEL', [
    "................",
    "................",
    "................",
    "....33333333....",
    "..331111111133..",
    ".31111111111113.",
    ".32211111111223.",
    ".32222222222223.",
    ".32112222222223.",
    ".32112222222223.",
    ".32112222222223.",
    ".32112222222223.",
    ".32222222222223.",
    ".33222222222233.",
    "..333333333333..",
    ".3.3........3.3.",
])
add_bg16('TANK', [
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "......3333......",
    "....322223333333",
    "...3222222333...",
    "..333333333333..",
    ".32222222222223.",
    "3222222222222223",
    "3333333333333333",
    "3.33.33.33.33.33",
    ".33.33.33.33.33.",
    "..3333333333333.",
][:16])
add_bg16('HANGAR', [
    "................",
    "................",
    "................",
    "......3333......",
    "....33222233....",
    "...3222222223...",
    "..322222222223..",
    ".32222222222223.",
    ".32333333333323.",
    "3231111111111323",
    "3231111111111323",
    "3231111111111323",
    "3231111111111323",
    "3231111111111323",
    "3231111111111323",
    "3333333333333333",
])
add_bg16('RUBBLE', [
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "......3.........",
    ".....323....3...",
    "..3..3223..323..",
    ".323.32223.3223.",
    "3222332223322223",
    "3222232222322223",
    "3223222322232223",
    "3333333333333333",
])
add_bg16('HOMEFLAG', [
    "................",
    "..3.............",
    "..33333333......",
    "..3111111133....",
    "..31133111113...",
    "..31133111113...",
    "..3111111133....",
    "..33333333......",
    "..3.............",
    "..3.............",
    "..3.............",
    "..3.............",
    "..3.............",
    "..3.............",
    ".333............",
    "33333...........",
])
BG['TREE'] = BG_BASE + len(bg_tiles)
bg_tiles.extend(grid_to_tiles([
    "...22...",
    "..2222..",
    ".222322.",
    ".223222.",
    "22222322",
    "23222222",
    "22232222",
    ".222222.",
    "..2322..",
    "...33...",
    "...33...",
    "...33...",
    "...33...",
    "...33...",
    "..3333..",
    ".333333.",
], "rows"))  # TREE top, TREE+1 bottom

# --- font
FONT = {
 'A': ".###. #...# #...# ##### #...# #...# #...#",
 'B': "####. #...# #...# ####. #...# #...# ####.",
 'C': ".###. #...# #.... #.... #.... #...# .###.",
 'D': "####. #...# #...# #...# #...# #...# ####.",
 'E': "##### #.... #.... ####. #.... #.... #####",
 'F': "##### #.... #.... ####. #.... #.... #....",
 'G': ".###. #...# #.... #.### #...# #...# .####",
 'H': "#...# #...# #...# ##### #...# #...# #...#",
 'I': ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",
 'J': "..### ...#. ...#. ...#. ...#. #..#. .##..",
 'K': "#...# #..#. #.#.. ##... #.#.. #..#. #...#",
 'L': "#.... #.... #.... #.... #.... #.... #####",
 'M': "#...# ##.## #.#.# #.#.# #...# #...# #...#",
 'N': "#...# #...# ##..# #.#.# #..## #...# #...#",
 'O': ".###. #...# #...# #...# #...# #...# .###.",
 'P': "####. #...# #...# ####. #.... #.... #....",
 'Q': ".###. #...# #...# #...# #.#.# #..#. .##.#",
 'R': "####. #...# #...# ####. #.#.. #..#. #...#",
 'S': ".#### #.... #.... .###. ....# ....# ####.",
 'T': "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",
 'U': "#...# #...# #...# #...# #...# #...# .###.",
 'V': "#...# #...# #...# #...# #...# .#.#. ..#..",
 'W': "#...# #...# #...# #.#.# #.#.# #.#.# .#.#.",
 'X': "#...# #...# .#.#. ..#.. .#.#. #...# #...#",
 'Y': "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",
 'Z': "##### ....# ...#. ..#.. .#... #.... #####",
 '0': ".###. #...# #..## #.#.# ##..# #...# .###.",
 '1': "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",
 '2': ".###. #...# ....# ...#. ..#.. .#... #####",
 '3': "####. ....# ....# .###. ....# ....# ####.",
 '4': "...#. ..##. .#.#. #..#. ##### ...#. ...#.",
 '5': "##### #.... ####. ....# ....# #...# .###.",
 '6': "..##. .#... #.... ####. #...# #...# .###.",
 '7': "##### ....# ...#. ..#.. .#... .#... .#...",
 '8': ".###. #...# #...# .###. #...# #...# .###.",
 '9': ".###. #...# #...# .#### ....# ...#. .##..",
 '-': "..... ..... ..... ##### ..... ..... .....",
 '!': "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",
 '.': "..... ..... ..... ..... ..... ..... ..#..",
 ':': "..... ..#.. ..#.. ..... ..#.. ..#.. .....",
 '/': "....# ...#. ...#. ..#.. .#... .#... #....",
 '(': "...#. ..#.. .#... .#... .#... ..#.. ...#.",
 ')': ".#... ..#.. ...#. ...#. ...#. ..#.. .#...",
 '=': "..... ..... ##### ..... ##### ..... .....",
 "'": "..#.. ..#.. .#... ..... ..... ..... .....",
 ',': "..... ..... ..... ..... ..... ..#.. .#...",
 '+': "..... ..#.. ..#.. ##### ..#.. ..#.. .....",
}
FONT_CHARS = " " + "".join(FONT.keys())
BG['FONT'] = BG_BASE + len(bg_tiles)
bg_tiles.append(tile_from_rows(["........"]*8))  # space
for ch in FONT_CHARS[1:]:
    rows = FONT[ch].split()
    assert len(rows) == 7 and all(len(r) == 5 for r in rows), ch
    g = ["........"] + ["." + r.replace('#', '3') + ".." for r in rows]
    bg_tiles.append(tile_from_rows(g))

# --- HUD bars (0..8 filled columns) with frame
BG['BAR'] = BG_BASE + len(bg_tiles)
for n in range(9):
    rows = ["33333333"]
    for y in range(6):
        rows.append("".join('2' if x < n else '.' for x in range(8)))
    rows.append("33333333")
    bg_tiles.append(tile_from_rows(rows))
add_bg('HUD_BOMB', [
    "........",
    "..3.3...",
    "...3....",
    "..222...",
    ".22322..",
    ".22322..",
    "..222...",
    "...2....",
])
add_bg('HUD_PLANE', [
    "........",
    "..3333..",
    "....3...",
    "3..222.2",
    "33222222",
    "3..333..",
    "....3...",
    "...3.3..",
])
add_bg('HUD_LINE', ["........"]*7 + ["33333333"])
MINIMAP_TILES = 12
BG['MINIMAP'] = BG_BASE + len(bg_tiles)
for i in range(MINIMAP_TILES):
    bg_tiles.append([0]*16)
assert BG_BASE + len(bg_tiles) <= 256, BG_BASE + len(bg_tiles)

# ---------------------------------------------------------------- world
WORLD_COLS = 192
ROWS = 16
random.seed(1916)
v = [13] * (WORLD_COLS + 1)
HOME = (1, 22)        # flat vertex range for home base
ENEMY = (168, 190)    # flat range for enemy base
h = 13
for i in range(HOME[1] + 1, ENEMY[0]):
    r = random.random()
    target = 11 if (i // 24) % 2 else 12
    if r < 0.28:
        step = 1 if h < target else -1
        if random.random() < 0.3: step = -step
        h = max(9, min(14, h + step))
    v[i] = h
# ramp back down to 13 before enemy base
i = ENEMY[0] - 1
while v[i] != 13 and i > 0:
    pass
    break
# fix transition near enemy base: walk backwards and adjust
for j in range(ENEMY[0] - 8, ENEMY[0]):
    diff = 13 - v[j - 1]
    v[j] = v[j - 1] + (1 if diff > 0 else -1 if diff < 0 else 0)
for j in range(ENEMY[0], WORLD_COLS + 1):
    v[j] = 13
for i in range(WORLD_COLS):
    assert abs(v[i + 1] - v[i]) <= 1

world = [[BG['SKY']] * ROWS for _ in range(WORLD_COLS)]
for c in range(WORLD_COLS):
    a, b = v[c], v[c + 1]
    if a == b:
        top = a
        world[c][a] = BG['GRASS']
        fill = a + 1
    elif b < a:  # rising
        world[c][b] = BG['SLOPE_UP']
        fill = a
    else:
        world[c][a] = BG['SLOPE_DN']
        fill = a + 1
    for r in range(fill, ROWS):
        world[c][r] = BG['DIRT']

# home base: flag, hangar, runway
RUNWAY_HOME = (5, 21)
RUNWAY_ENEMY = (170, 188)
for c in range(RUNWAY_HOME[0], RUNWAY_HOME[1]):
    world[c][13] = BG['RUNWAY']
for c in range(RUNWAY_ENEMY[0], RUNWAY_ENEMY[1]):
    world[c][13] = BG['RUNWAY']

def put16(col, row, base):
    world[col][row] = base
    world[col + 1][row] = base + 1
    world[col][row + 1] = base + 2
    world[col + 1][row + 1] = base + 3

put16(2, 11, BG['HANGAR'])
put16(0, 11, BG['HOMEFLAG'])

# targets
T_FACTORY, T_FUEL, T_TANK, T_HANGAR = 0, 1, 2, 3
targets = []
used = set()
def flat_at(c):
    return v[c] == v[c + 1] == v[c + 2]
cands = [c for c in range(HOME[1] + 6, ENEMY[0] - 3) if flat_at(c)]
random.shuffle(cands)
for c in sorted(cands):
    if len(targets) >= 11: break
    if any(abs(c - t[0]) < 11 for t in targets): continue
    ttype = [T_FACTORY, T_FUEL, T_TANK][len(targets) % 3]
    targets.append((c, v[c] - 2, ttype))
# enemy hangar + an extra tank at enemy base
targets.append((189, 11, T_HANGAR))
targets.append((167 if flat_at(167) else 172, 11, T_FUEL))
targets.sort()
tbase = [BG['FACTORY'], BG['FUEL'], BG['TANK'], BG['HANGAR']]
for c, r, t in targets:
    assert flat_at(c) or c >= 170, (c, v[c:c+3])
    put16(c, r, tbase[t])

# trees
for c in range(HOME[1] + 2, ENEMY[0] - 2):
    if random.random() < 0.12 and v[c] == v[c + 1] and all(abs(c - t[0]) > 2 and c != t[0] + 1 for t in targets):
        if world[c][v[c] - 1] == BG['SKY'] and world[c][v[c]-2] == BG['SKY']:
            world[c][v[c] - 2] = BG['TREE']
            world[c][v[c] - 1] = BG['TREE'] + 1

# minimap base image (96 x 8) -> 12 tiles
mm = [[0] * 96 for _ in range(8)]
for mx in range(96):
    x = mx * 16 + 8
    c = x // 8
    gy = v[c] * 8
    my = min(7, gy // 16)
    for yy in range(my, 8):
        mm[yy][mx] = 1
    mm[my][mx] = 2
for mx in range(RUNWAY_HOME[0] * 8 // 16, RUNWAY_HOME[1] * 8 // 16):
    mm[6][mx] = 3
mm_tiles = []
for t in range(12):
    mm_tiles.append(tile_from_rows(["".join('.' if mm[y][t*8+i] == 0 else str(mm[y][t*8+i]) for i in range(8)) for y in range(8)]))

# ---------------------------------------------------------------- emit
def carr(name, data, ctype="uint8_t", per=16):
    s = f"const {ctype} {name}[{len(data)}] = {{\n"
    for i in range(0, len(data), per):
        s += "  " + ",".join(f"0x{b:02X}" if ctype == "uint8_t" else str(b) for b in data[i:i+per]) + ",\n"
    return s + "};\n"

flat_spr = [b for t in spr_tiles for b in t]
flat_bg = [b for t in bg_tiles for b in t]
flat_world = [world[c][r] for c in range(WORLD_COLS) for r in range(ROWS)]
flat_mm = [b for t in mm_tiles for b in t]

# direction tables (16 dirs, 0 = right, 4 = up), scaled by 64
cos64 = [round(64 * math.cos(math.radians(k * 22.5))) for k in range(16)]
sin64 = [round(64 * math.sin(math.radians(k * 22.5))) for k in range(16)]

charmap = [0] * 96
for i, ch in enumerate(FONT_CHARS):
    charmap[ord(ch) - 32] = BG['FONT'] + i
for ch in "abcdefghijklmnopqrstuvwxyz":
    charmap[ord(ch) - 32] = charmap[ord(ch.upper()) - 32]

c = ['/* generated by tools/gen_assets.py - do not edit */', '#include <stdint.h>', '#include "assets.h"', '']
c.append(carr("spr_tiles", flat_spr))
c.append(carr("bg_tiles", flat_bg))
c.append(carr("world_init", flat_world))
c.append(carr("ground_vtx", v))
c.append(carr("minimap_base", flat_mm))
tgt_flat = []
for col, row, t in targets: tgt_flat += [col, row, t]
c.append(carr("target_init", tgt_flat, per=3))
c.append(carr("cos64", cos64, "int8_t"))
c.append(carr("sin64", sin64, "int8_t"))
c.append(carr("charmap", charmap))
open(OUT_C, "w").write("\n".join(c))

hh = ['/* generated by tools/gen_assets.py - do not edit */', '#ifndef ASSETS_H', '#define ASSETS_H', '#include <stdint.h>', '']
hh.append(f"#define WORLD_COLS {WORLD_COLS}")
hh.append(f"#define WORLD_ROWS {ROWS}")
hh.append(f"#define NUM_TARGETS {len(targets)}")
hh.append(f"#define SPR_TILE_COUNT {len(spr_tiles)}")
hh.append(f"#define BG_TILE_BASE {BG_BASE}")
hh.append(f"#define BG_TILE_COUNT {len(bg_tiles)}")
hh.append(f"#define RUNWAY_HOME_X0 {RUNWAY_HOME[0]*8}")
hh.append(f"#define RUNWAY_HOME_X1 {RUNWAY_HOME[1]*8}")
hh.append(f"#define GROUND_MIN_Y {min(v)*8 - 8}")
hh.append(f"#define ENEMY_BASE_X {RUNWAY_ENEMY[0]*8 + 40}")
hh.append(f"#define T_FACTORY 0\n#define T_FUEL 1\n#define T_TANK 2\n#define T_HANGAR 3")
for k, val in SPR.items(): hh.append(f"#define SPR_{k} {val}")
for k, val in BG.items(): hh.append(f"#define BGT_{k} {val}")
hh.append("")
for name, n, t in [("spr_tiles", len(flat_spr), "uint8_t"), ("bg_tiles", len(flat_bg), "uint8_t"),
                   ("world_init", len(flat_world), "uint8_t"), ("ground_vtx", len(v), "uint8_t"),
                   ("minimap_base", len(flat_mm), "uint8_t"), ("target_init", len(tgt_flat), "uint8_t"),
                   ("cos64", 16, "int8_t"), ("sin64", 16, "int8_t"), ("charmap", 96, "uint8_t")]:
    hh.append(f"extern const {t} {name}[{n}];")
hh += ["", "#endif", ""]
open(OUT_H, "w").write("\n".join(hh))

# preview
try:
    from PIL import Image
    pal = [(224, 248, 208), (136, 192, 112), (52, 104, 86), (8, 24, 32)]
    im = Image.new("RGB", (16 * 8 + 8 * 7, 16), pal[0])
    for k, p in enumerate(plane_imgs):
        for y in range(16):
            for x in range(16):
                im.putpixel((k * 23 + x, y), pal[p[y][x]])
    im.resize((im.width * 6, im.height * 6), Image.NEAREST).save(os.path.join(ROOT, "tools", "plane_preview.png"))
except ImportError:
    pass
print("targets:", targets)
print("spr tiles", len(spr_tiles), "bg tiles", len(bg_tiles), "-> last", BG_BASE + len(bg_tiles))
