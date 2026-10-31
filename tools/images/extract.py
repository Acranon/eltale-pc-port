#!/usr/bin/env python3
"""Decode the game's 2D images (title, prologue, menus, HUD, Controller Pak, ending,
credits, world map) from the user's own ROM into PNGs, for redrawing text-bearing images.

    python tools/images/extract.py <rom.z64> [--out out/images]

Writes <out>/png/<id>.png and <out>/manifest.json. If <out>/notes.json exists (hand-written
translations, kept in the git-ignored out/ folder), also writes <out>/catalog.html.

No ROM bytes are in this file: only offsets, sizes and formats, taken from the decomp
(decomp/assets.yaml and the code that draws each image).
"""
import argparse
import html
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
ASSETS_YAML = os.path.join(REPO, "decomp", "assets.yaml")

# Runtime-loaded blocks: (ROM start, RAM address) for converting a RAM pointer to a ROM offset.
BLOCK_HUD = (0xD74AC0, 0x8039ADB0)      # Gfx_InitFrames: D74AC0..D86170 -> 8039ADB0
BLOCK_CUTSCENE = (0xE43780, 0x801C0000)  # Core_RunCameraCutsceneLoop: E43780..E5BF60 -> 801C0000
CUTSCENE_TABLE = 0x801D8620             # CutsceneSequenceEntry[14]; +0x1C = image cue list
# menu_map_picture_dma: world-map pictures (tlut at +0, pixels at +0x200), regions 0..9
MAP_PICTURES = [0xF2FB70, 0xF3ADD0, 0xF46030, 0xF51290, 0xF5C4F0, 0xF67750,
                0xF729B0, 0xF73050, 0xF73730, 0xF73F90]
MAP_SMALL_SIZES = [(0x41, 0x12), (0x41, 0x13), (0x60, 0x11), (0x63, 0x12)]  # regions 6..9


def load_assets():
    assets = {}
    pat = re.compile(r"name:\s*(\w+),\s*rom:\s*(0x[0-9A-Fa-f]+),\s*size:\s*(0x[0-9A-Fa-f]+)")
    with open(ASSETS_YAML, encoding="utf-8") as f:
        for line in f:
            m = pat.search(line)
            if m:
                assets[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return assets


def hud(addr):
    return BLOCK_HUD[0] + addr - BLOCK_HUD[1]


def cut(addr):
    return BLOCK_CUTSCENE[0] + addr - BLOCK_CUTSCENE[1]


# ---------------------------------------------------------------- pixel decoding

def rgba5551(v):
    r = (v >> 11) & 31
    g = (v >> 6) & 31
    b = (v >> 1) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if v & 1 else 0)


def palette(rom, off, n):
    return [rgba5551(struct.unpack_from(">H", rom, off + i * 2)[0]) for i in range(n)]


def decode(rom, fmt, off, w, h, stride=None, tlut=None):
    """Returns a list of rows of RGBA tuples. stride is in pixels (defaults to w)."""
    stride = stride or w
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            i = y * stride + x
            if fmt == "CI8":
                px = tlut[rom[off + i]]
            elif fmt == "CI4":
                b = rom[off + i // 2]
                px = tlut[(b >> 4) if i % 2 == 0 else (b & 15)]
            elif fmt == "I8":
                v = rom[off + i]
                px = (v, v, v, 255)
            elif fmt == "I4":
                b = rom[off + i // 2]
                v = (b >> 4) if i % 2 == 0 else (b & 15)
                v *= 17
                px = (v, v, v, 255)
            elif fmt == "IA8":
                b = rom[off + i]
                v = (b >> 4) * 17
                px = (v, v, v, (b & 15) * 17)
            elif fmt == "RGBA16":
                px = rgba5551(struct.unpack_from(">H", rom, off + i * 2)[0])
            else:
                raise ValueError(fmt)
            row.append(px)
        rows.append(row)
    return rows


def write_png(path, rows):
    try:
        from PIL import Image
        h = len(rows)
        w = len(rows[0])
        img = Image.new("RGBA", (w, h))
        img.putdata([px for row in rows for px in row])
        img.save(path)
        return
    except ImportError:
        pass
    import zlib
    h = len(rows)
    w = len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------- image list

def specs(rom, A):
    """Every image: dict(id, group, fmt, tex, w, h, stride, tlut, ncol, where)."""
    S = []

    def add(id, group, fmt, tex, w, h, tlut=None, stride=None, where="", ncol=None, crop=None):
        tex_off = A[tex][0] if isinstance(tex, str) else tex
        tl_off = (A[tlut][0] if isinstance(tlut, str) else tlut) if tlut is not None else None
        S.append(dict(id=id, group=group, fmt=fmt, tex=tex if isinstance(tex, str) else id,
                      tex_off=tex_off, w=w, h=h, stride=stride or w, tlut=tlut if isinstance(tlut, str) else None,
                      tlut_off=tl_off, ncol=ncol or (16 if fmt == "CI4" else 256), where=where, crop=crop))

    # --- title overlay (title_draw.c)
    add("title_background", "title", "CI8", "title_background_tex", 320, 224, "title_background_tlut",
        where="Title screen background (320x224, drawn in 6-row strips)")
    add("title_center_image", "title", "CI8", "title_center_image_tex", 194, 76, "title_center_image_tlut",
        where="Title screen logo, drawn at (63,69)")
    add("title_lower_strip", "title", "CI4", "title_lower_strip_tex", 160, 16, "title_lower_strip_tlut",
        where="Title screen, bottom line at (85,178), 147x16 shown")
    add("title_initial_screen_image", "title", "CI8", "title_initial_screen_image_tex", 100, 15,
        "title_initial_screen_image_tlut", where="Title screen before the menu opens (title_initial_screen_dl), 91x15 shown")
    add("title_menu_atlas", "title", "CI4", "title_menu_atlas_tex", 176, 12, "title_menu_atlas_tlut",
        where="Title menu: three items at y=160 (s=0 w=54, s=54 w=55, s=109 w=66)")
    add("title_menu_item_cursor", "title", "CI4", "title_menu_item_cursor_tex", 16, 11, "title_menu_item_cursor_tlut",
        where="Title menu item cursor")
    add("title_confirm_cursor", "title", "CI8", "title_confirm_cursor_tex", 128, 14, "title_confirm_cursor_tlut",
        where="Title confirmation cursor atlas (pieces at s=22,51,72,100)")
    add("title_message_box_top_left_border", "title", "CI8", "title_message_box_top_left_border_tex", 4, 4,
        "title_message_box_border_tlut", where="Title message box border")

    # --- intro overlay: splash screens and Controller Pak warnings (intro_screens.c)
    add("intro_splash1", "intro", "CI4", "D_80264740_ovl_intro", 208, 18, "D_80264E98_ovl_intro",
        where="Boot splash 1 (Intro_DrawSplash1), at (60,111), 201x18 shown")
    add("intro_splash2", "intro", "CI4", "D_802650A0_ovl_intro", 176, 92, "D_80267048_ovl_intro",
        where="Boot splash 2 (Intro_DrawSplash2), at (75,73), 168x92 shown")
    add("intro_splash3", "intro", "CI4", "D_80267250_ovl_intro", 176, 40, "D_80268018_ovl_intro",
        where="Boot splash 3 (Intro_DrawSplash3), at (75,99), 168x40 shown")
    add("intro_nopak_screen", "pak", "CI4", "D_80259250_ovl_intro", 208, 112, "D_8025BFD8_ovl_intro",
        where="Boot: no Controller Pak warning with a 2-way choice (Intro_RunNoPakScreen state 0), 200x112 at (60,64)")
    add("intro_pak_help", "pak", "CI4", "D_8025C1E0_ovl_intro", 208, 42, "D_8025D2F8_ovl_intro",
        where="Boot: second page of the no-Pak / Pak-error screens (state 1, B returns), 200x42 at (60,99)")
    add("intro_pak_full_screen", "pak", "CI4", "D_8025D500_ovl_intro", 208, 112, "D_80260288_ovl_intro",
        where="Boot: Controller Pak full screen (Intro_RunPakFullScreen), 200x112 at (60,64)")
    add("intro_no_controller", "pak", "CI4", "D_80260490_ovl_intro", 208, 42, "D_802615A8_ovl_intro",
        where="Boot: no controller message (Intro_DrawNoControllerMessage), 200x42 at (60,99)")
    add("intro_pak_error_screen", "pak", "CI4", "D_802617B0_ovl_intro", 208, 112, "D_80264538_ovl_intro",
        where="Boot: Controller Pak error screen with a 2-way choice (Intro_RunPakErrorScreen), 200x112 at (60,64)")
    add("intro_choice_cursor", "pak", "CI4", "D_80268220_ovl_intro", 16, 11, "D_80268280_ovl_intro",
        where="Boot warning screens: choice cursor")
    add("intro_skip_fade_strip", "intro", "CI8", "D_80217C80_ovl_intro", 320, 6, "D_80217A80_ovl_intro",
        where="Prologue skip fade (one 320x6 strip repeated)")
    add("intro_unreferenced_D_8024BD38", "intro", "CI4", "D_8024BD38_ovl_intro", 192, 15, "D_8024C2E0_ovl_intro",
        where="Unreferenced image in the intro overlay (size found by trial: 192x15 CI4)")

    # --- intro overlay: prologue sprites (intro_data.c intro_sprite_cues)
    prologue = [
        ("D_8021C3A0_ovl_intro", "intro_sprite_anim_0_tlut", "CI8", 304, 224, "Prologue scene 0 backdrop (full screen), palette A"),
        ("D_8021C3A0_ovl_intro", "D_8021C1A0_ovl_intro", "CI8", 304, 224, "Prologue anims 1/24/30 backdrop (full screen), palette B"),
        ("D_80218800_ovl_intro", "D_80218600_ovl_intro", "CI8", 194, 76, "Prologue anims 3/30: logo at (63,85), revealed in 15-px columns"),
        ("intro_sprite_anim_4_tex", "intro_sprite_anim_4_tlut", "CI4", 64, 64, "Prologue anim 4 at (209,24)"),
        ("intro_sprite_anim_6_variant0_tex", "intro_sprite_anim_6_variant0_tlut", "CI4", 48, 190, "Prologue anim 6: side pillars at x=40 and mirrored at x=234"),
        ("intro_sprite_anim_6_variant1_tex", "intro_sprite_anim_6_variant1_tlut", "CI4", 160, 24, "Prologue anim 6: top/bottom bars at (86,27) and (86,173)"),
        ("D_8022D530_ovl_intro", "D_8022D330_ovl_intro", "CI8", 176, 129, "Prologue anims 7/13/18/23: picture at (73,47)"),
        ("intro_sprite_anim_8_tex", "intro_sprite_anim_8_tlut", "CI4", 208, 114, "Prologue anim 8: page at (63,63), 194 wide shown"),
        ("D_8022CFA0_ovl_intro", "D_8022CDA0_ovl_intro", "CI8", 30, 30, "Prologue anims 9/14/19 at (192,57)"),
        ("D_80232FE0_ovl_intro", "D_80232DE0_ovl_intro", "CI8", 176, 129, "Prologue anims 10/26: picture at (73,47)"),
        ("D_80238A90_ovl_intro", "D_80238890_ovl_intro", "CI8", 109, 179, "Prologue anims 11/16/21 at (78,44)"),
        ("intro_sprite_anim_12_tex", "intro_sprite_anim_12_tlut", "CI4", 160, 99, "Prologue anim 12: page at (82,71), 156 wide shown"),
        ("D_8023D8D0_ovl_intro", "D_8023D6D0_ovl_intro", "CI8", 176, 129, "Prologue anims 15/27: picture at (73,47)"),
        ("intro_sprite_anim_17_tex", "intro_sprite_anim_17_tlut", "CI4", 176, 78, "Prologue anim 17: page at (74,81)"),
        ("D_80243380_ovl_intro", "D_80243180_ovl_intro", "CI8", 176, 129, "Prologue anims 20/28: picture at (73,47)"),
        ("intro_sprite_anim_22_tex", "intro_sprite_anim_22_tlut", "CI4", 176, 65, "Prologue anim 22: page at (75,88)"),
        ("intro_sprite_anim_24_tex", "intro_sprite_anim_24_tlut", "CI8", 92, 103, "Prologue anim 24 at (114,69)"),
        ("intro_sprite_anim_29_tex", "intro_sprite_anim_29_tlut", "CI4", 160, 39, "Prologue anim 29: page at (83,101)"),
        ("intro_sprite_anim_34_tex", "intro_sprite_anim_34_tlut", "CI4", 160, 39, "Prologue anim 34: page at (88,101)"),
        ("intro_sprite_anim_32_tex", "intro_sprite_anim_32_tlut", "CI4", 128, 43, "Prologue anim 32: page at (100,99)"),
        ("intro_sprite_anim_31_tex", "intro_sprite_anim_31_tlut", "CI8", 320, 6, "Prologue anim 31: full-width 320x6 strip"),
    ]
    shown = {"intro_sprite_anim_8_tex": 194, "intro_sprite_anim_12_tex": 156, "intro_sprite_anim_17_tex": 172,
             "intro_sprite_anim_22_tex": 171, "intro_sprite_anim_29_tex": 155, "intro_sprite_anim_34_tex": 145,
             "intro_sprite_anim_32_tex": 121, "intro_sprite_anim_4_tex": 63, "intro_sprite_anim_6_variant1_tex": 149,
             "intro_sprite_anim_6_variant0_tex": 46}  # widest s+w any cue shows
    for i, (tex, tl, fmt, w, h, where) in enumerate(prologue):
        id = "prologue_" + (tex.replace("_ovl_intro", "").replace("intro_sprite_", "").replace("_tex", ""))
        if any(s["id"] == id for s in S):
            id += "_b"
        add(id, "intro", fmt, tex, w, h, tl, where=where, crop=shown.get(tex))

    # --- ending overlay (ending_data.c ending_sprite_cues)
    add("ending_scroll_backdrop_first", "ending", "CI8", "ending_scroll_backdrop_tex", 304, 224,
        "ending_scroll_backdrop_first_tlut", where="Ending anim 0: backdrop at (8,30), first palette")
    add("ending_scroll_backdrop_second", "ending", "CI8", "ending_scroll_backdrop_tex", 304, 224,
        "ending_scroll_backdrop_second_tlut", where="Ending anim 2: same backdrop, second palette")
    add("ending_sprite_anim_1", "ending", "CI4", "ending_sprite_anim_1_tex", 272, 310, "ending_sprite_anim_1_tlut",
        where="Ending anim 1: 267x310 page scrolled up from y=192 (texture rows are 272 px)", crop=267)

    # --- main menu sprites (menu.c menu_sprite_sizes / menu_sprite_tex, CI8 menu_sprite_tlut)
    sizes = [(0x10, 0x10), (0x10, 0x0D), (0x10, 0x10), (0x10, 0x10), (0x0C, 0x0C), (8, 8), (0x10, 0x0B), (0x10, 0x0B),
             (0x0C, 0x0A), (0x0C, 0x0A)] + [(8, 0x0A)] * 16 + [
             (0x52, 0x0D), (0x18, 0x0F), (0x1C, 0x0D), (0x24, 0x0F), (0x1C, 0x0F), (0x24, 0x0D),
             (0x14, 0x0B), (0x56, 0x0E), (0x36, 0x0C), (0x2A, 0x0C), (0x54, 7), (0x50, 3), (8, 0x0B), (8, 0x0B),
             (8, 0x0B), (0x1A, 9), (0x1A, 9), (0x3A, 0x10), (0x0C, 0x10), (0x56, 0x0D), (0x0C, 0x0C), (0x0C, 0x0C),
             (0x0C, 0x0C), (0x0C, 0x0C), (0x3C, 7), (0x34, 3), (0x3C, 0x14), (0x20, 0x0C), (0x1C, 0x0D), (4, 4),
             (4, 4), (4, 4), (4, 4), (4, 4), (4, 4), (4, 4), (0x64, 0x0E), (0x96, 2),
             (2, 0x78), (4, 4), (0x7C, 0x0C), (0x1C, 0x0E), (0x1C, 0x0E), (0x1C, 0x0D), (0x14, 0x0E), (0x18, 0x0E),
             ] + [(8, 0x0A)] * 11 + [(0x0C, 0x10), (0x0C, 0x10), (8, 1), (0x1A, 9), (0x1A, 9)]
    for i, (w, h) in enumerate(sizes):
        if A["menu_sprite_%02d_tex" % i][1] < w * h:
            continue  # empty slot: 8 bytes of data, nothing to draw
        add("menu_sprite_%02d" % i, "menu", "CI8", "menu_sprite_%02d_tex" % i, w, h, "menu_sprite_tlut",
            where="Pause menu sprite %d (Menu_DrawSprite)" % i)
    for i, h in enumerate([32, 32, 32, 24]):
        add("menu_bg_strip_%d" % i, "menu", "CI8", "menu_bg_strip_tex_%d" % i, 64, h, "menu_bg_tlut",
            where="Pause menu background strip")
    for k, (w, h) in enumerate([(25, 50), (25, 50), (25, 50), (28, 56)]):
        for j in range(2):
            add("menu_exp_gauge_%d_%d" % (k, j), "menu", "CI8", "menu_exp_gauge_layer_%d_%d_tex" % (k, j), w, h,
                "menu_exp_gauge_layer_%d_tlut" % k, where="Pause menu EXP gauge layer %d half %d" % (k, j))
    add("text_speaker_name_bg", "menu", "CI8", "text_speaker_name_bg_tex", 40, 13, "text_window_tlut",
        where="Dialogue window speaker-name plate")
    add("text_window_fill", "menu", "CI8", "text_window_fill_tex", 16, 16, "text_window_tlut", where="Dialogue window fill")
    add("text_advance_icon", "menu", "CI8", "text_advance_icon_tex", 12, 12, "text_window_tlut", where="Dialogue advance icon")
    add("text_choice_cursor", "menu", "CI8", "text_choice_cursor_tex", 12, 12, "text_window_tlut", where="Dialogue choice cursor")

    # --- HUD / battle block (hud.c; RAM 8039ADB0.., loaded from ROM D74AC0)
    add("hud_atlas_main", "battle", "CI8", hud(0x8039AFB0), 176, 74, hud(0x8039ADB0),
        where="Field/battle HUD atlas (D_803A86F8): HP/MP frame, meters, spell icons")
    add("hud_damage_digits", "battle", "RGBA16", hud(0x8039E290), 128, 10,
        where="Floating damage/heal numbers (D_8039E290)")
    add("hud_atlas_battle", "battle", "CI8", hud(0x8039EE90), 216, 80, hud(0x8039EC90),
        where="Battle HUD atlas (D_803A8718): command/spell panel pieces")
    add("hud_glyph_font", "battle", "CI4", hud(0x803A3230), 144, 45, hud(0x803A3ED8),
        where="HUD mini-font for spell labels (D_803A8748): a glyph font, drawn via strings")
    add("hud_atlas_icons", "battle", "CI8", hud(0x803A4318), 128, 88, hud(0x803A4118),
        where="HUD atlas D_803A8728 (incl. the out-of-range icon at s=81,t=49, 46x15)")
    add("hud_dl_tex_64x29", "battle", "CI8", hud(0x803A7118), 64, 29, hud(0x803A6F18), where="HUD display-list texture")
    add("hud_dl_tex_16x30", "battle", "CI8", hud(0x803A7A58), 16, 30, hud(0x803A7858), where="HUD display-list texture")
    add("hud_dl_tex_24x30", "battle", "CI8", hud(0x803A7E38), 24, 30, hud(0x803A7C38), where="HUD display-list texture")
    add("hud_dl_tex_24x42", "battle", "CI8", hud(0x803A8308), 24, 42, hud(0x803A8108), where="HUD display-list texture")
    add("hud_dl_tex_i8_40x61", "battle", "I8", hud(0x803A8890), 40, 61, where="HUD display-list texture (I8)")
    add("hud_dl_tex_i8_40x96", "battle", "I8", hud(0x803A92E0), 40, 96, where="HUD display-list texture (I8)")

    # --- Controller Pak manager overlay (pakmenu.c sprite tables; CI4 blits)
    pak = [
        ("pak_menu_cursor_tex", "pak_menu_cursor_tlut", 16, 14, "Pak manager cursor"),
        ("pak_menu_screen_frame_main_tex", "pak_menu_screen_frame_tlut", 32, 32, "Pak manager screen frame tile"),
        ("pak_menu_screen_frame_right_tex", "pak_menu_screen_frame_tlut", 32, 32, "Pak manager screen frame, right edge"),
        ("D_801030A8_ovl_DA4570", "D_80103088_ovl_DA4570", 160, 14, "Pak manager: heading at (82,30)"),
        ("D_80103528_ovl_DA4570", "D_80103508_ovl_DA4570", 64, 12, "Pak manager: label at (94,55)"),
        ("D_801036C8_ovl_DA4570", "D_801036A8_ovl_DA4570", 32, 12, "Pak manager: label at (204,54)"),
        ("D_801037A8_ovl_DA4570", "D_80103788_ovl_DA4570", 32, 11, "Pak manager: column header at (37,74)"),
        ("D_80103878_ovl_DA4570", "D_80103858_ovl_DA4570", 32, 12, "Pak manager: column header at (149,73)"),
        ("D_80103958_ovl_DA4570", "D_80103938_ovl_DA4570", 32, 12, "Pak manager: column header at (254,73)"),
        ("D_80103A38_ovl_DA4570", "D_80103A18_ovl_DA4570", 80, 12, "Pak manager: label at (126,173)"),
        ("D_80103DD8_ovl_DA4570", "D_80103DB8_ovl_DA4570", 32, 12, "Pak manager: button label at (107,193)"),
        ("D_80103C38_ovl_DA4570", "D_80103C18_ovl_DA4570", 64, 12, "Pak manager: button label at (174,193)"),
        ("D_80103EB8_ovl_DA4570", "D_80103E98_ovl_DA4570", 112, 2, "Pak manager: divider line"),
        ("D_80102F60_ovl_DA4570", "D_80102F40_ovl_DA4570", 16, 15, "Pak manager: scroll arrow (previous rows) at (22,88)"),
        ("D_80102FD8_ovl_DA4570", "D_80102F40_ovl_DA4570", 16, 22, "Pak manager: scroll arrow (more rows) at (22,144)"),
    ]
    for n in ["D_80102470", "D_801024F0", "D_80102570", "D_801025F0", "D_80102670", "D_801026F0",
              "D_80102770", "D_801027F0", "D_80102870"]:
        pak.append((n + "_ovl_DA4570", "D_80102450_ovl_DA4570", 16, 16, "Pak manager panel tile"))
    for n in ["D_801028F0", "D_80102980", "D_80102A10"]:
        pak.append((n + "_ovl_DA4570", "D_80102450_ovl_DA4570", 16, 18, "Pak manager panel bottom tile"))
    for n in ["D_80102AC0", "D_80102B40", "D_80102BC0", "D_80102C40", "D_80102CC0", "D_80102D40",
              "D_80102DC0", "D_80102E40", "D_80102EC0"]:
        pak.append((n + "_ovl_DA4570", "D_80102AA0_ovl_DA4570", 16, 16, "Pak manager message window tile"))
    for tex, tl, w, h, where in pak:
        add(tex.replace("_ovl_DA4570", "").replace("_tex", "") if tex.startswith("pak") else "pak_" + tex.replace("_ovl_DA4570", ""),
            "pakmenu", "CI4", tex, w, h, tl, where=where)

    # --- world map pictures (menu.c menu_map_picture_dma; CI8, tlut at +0, pixels at +0x200)
    for r in range(6):
        add("map_region_%d" % r, "map", "CI8", MAP_PICTURES[r] + 0x200, 244, 185, MAP_PICTURES[r],
            where="Pause menu world map, region %d (244x185 at (39,29))" % r)
    for n, (w, h) in enumerate(MAP_SMALL_SIZES):
        add("map_region_%d_small" % (n + 6), "map", "CI8", MAP_PICTURES[n + 6] + 0x200, w, h, MAP_PICTURES[n + 6],
            where="Pause menu world map, region %d (no full map; %dx%d panel)" % (n + 6, w, h))

    # --- ending credit captions (cutscene.c image cues, block E43780 -> 801C0000)
    for seq in range(14):
        e = struct.unpack_from(">iiifffII", rom, cut(CUTSCENE_TABLE + seq * 0x20))
        cue = e[7]
        n = 0
        while n < 80:
            frame, dur, x, y, w, h, tl, img = struct.unpack_from(">HHhhhhII", rom, cut(cue + n * 0x14))
            if frame == 0xFFFF or (n > 0 and frame == 0 and dur == 0):
                break
            id = "credit_%02d_%02d" % (seq, n)
            if not any(s["tex_off"] == cut(img) for s in S):
                add(id, "credits", "CI4", cut(img), w, h, cut(tl), stride=(w + 15) & ~15,
                    where="Ending cutscene %d: caption at (%d,%d), frames %d-%d" % (seq, x, y, frame, frame + dur))
            n += 1
    return S


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--out", default=os.path.join(REPO, "out", "images"))
    a = ap.parse_args()
    rom = open(a.rom, "rb").read()
    if rom[:4] != b"\x80\x37\x12\x40":
        sys.exit("expected a big-endian .z64 ROM")
    A = load_assets()
    os.makedirs(os.path.join(a.out, "png"), exist_ok=True)
    manifest = []
    for s in specs(rom, A):
        tlut = palette(rom, s["tlut_off"], s["ncol"]) if s["fmt"].startswith("CI") else None
        rows = decode(rom, s["fmt"], s["tex_off"], s["w"], s["h"], s["stride"], tlut)
        if s["crop"]:
            rows = [r[:s["crop"]] for r in rows]
        write_png(os.path.join(a.out, "png", s["id"] + ".png"), rows)
        m = dict(s)
        m["png"] = "png/%s.png" % s["id"]
        m["tex_off"] = "0x%X" % s["tex_off"]
        m["tlut_off"] = "0x%X" % s["tlut_off"] if s["tlut_off"] is not None else None
        manifest.append(m)
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print("%d images -> %s" % (len(manifest), a.out))
    notes = os.path.join(a.out, "notes.json")
    if os.path.exists(notes):
        write_catalog(a.out, manifest, json.load(open(notes, encoding="utf-8")))


# ---------------------------------------------------------------- catalog

GROUPS = [("title", "Title screen"), ("intro", "Boot splashes and prologue"), ("menu", "Pause menu and dialogue"),
          ("map", "World map"), ("battle", "Field and battle HUD"), ("pak", "Controller Pak warnings (boot)"),
          ("pakmenu", "Controller Pak manager"), ("ending", "Ending"), ("credits", "Ending credit captions"),
          ("other", "Other")]


OWNERS = {"sol": "For Sol", "claude": "Claude makes this (text in the game's own style)", "done": "Done"}
BRIEF = """<section class=brief><h2>Brief for the artist (Sol)</h2>
<p>We are translating Eltale Monsters (N64, 1999) into English. These are the game's pictures that have Japanese
text in them. The cards marked <b>For Sol</b> are yours; the rest Claude makes in code (long text, maps, fonts),
so leave those alone.</p>
<ul>
<li><b>Match the original.</b> Same colours, outline, shading and mood as the image on the card, with the English
on the card in place of the Japanese. Nothing else changes: no extra text, decoration or background.</li>
<li><b>Shape and size.</b> Keep the original's proportions exactly (the card gives them) and draw it bigger than
the original, 4x or more, on a <b>transparent background</b>. We shrink it to the game's size and put it in
the original's colours ourselves, so don't worry about exact pixels or palette.</li>
<li><b>It must read when small.</b> Most of these are 9 to 16 pixels tall in the game. Bold, simple letter shapes
and a strong outline survive shrinking; fine texture, thin lines, noise and wavy detail do not.</li>
<li><b>Fit the space.</b> If the English does not fit at a readable size, use the shorter wording the card
suggests rather than squeezing the letters.</li>
<li><b>One file per card</b>, named as the card says (<code>&lt;name&gt;.png</code>), all in one folder.</li>
<li><b>The title logo</b> is the one open design question: see its card.</li>
</ul></section>"""


def write_catalog(out, manifest, notes):
    by_id = {m["id"]: m for m in manifest}
    text_ids = [k for k in notes.get("images", {})]
    css = """
:root{--bg:#f4f1ea;--card:#fffdf8;--ink:#1f1d1a;--mute:#6b665d;--line:#ddd6c8;--img:#3a3f4a;--accent:#7a4b1e}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#17181b;--card:#202227;--ink:#ece8e1;--mute:#a19c92;--line:#33363d;--img:#3a3f4a;--accent:#e0a463}}
:root[data-theme=dark]{--bg:#17181b;--card:#202227;--ink:#ece8e1;--mute:#a19c92;--line:#33363d;--img:#3a3f4a;--accent:#e0a463}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,"Segoe UI","Yu Gothic UI","Meiryo",sans-serif}
main{max-width:1180px;margin:0 auto;padding:24px 16px 64px}h1{font-size:26px;margin:0 0 4px}h2{font-size:20px;margin:40px 0 12px;border-bottom:1px solid var(--line);padding-bottom:6px}
p.lead{color:var(--mute);margin:0 0 16px}nav a{color:var(--accent);margin-right:14px;font-size:14px}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,520px),1fr));gap:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;min-width:0}
.pic{background:var(--img) repeating-conic-gradient(#ffffff10 0 25%,transparent 0 50%) 0 0/16px 16px;border-radius:6px;padding:10px;overflow-x:auto;text-align:center}
.pic img{image-rendering:pixelated;display:inline-block}
.meta{font:12px/1.5 ui-monospace,Consolas,monospace;color:var(--mute);margin:8px 0;word-break:break-word}
.where{font-size:13px;color:var(--mute)}.ja{font-size:16px;margin:8px 0 2px;white-space:pre-wrap}.en{font-size:16px;font-weight:600;white-space:pre-wrap}
.lbl{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--mute);margin-top:8px}.note{font-size:13px;border-left:3px solid var(--accent);padding-left:8px;margin-top:8px}
.grid{display:flex;flex-wrap:wrap;gap:8px}.thumb{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:6px;font:11px ui-monospace,Consolas,monospace;color:var(--mute);max-width:100%}
.badge{display:inline-block;font-size:11px;font-weight:600;border-radius:99px;padding:2px 9px;margin-bottom:6px}
.b-sol{background:#2f6fde;color:#fff}.b-claude{background:#6b6b78;color:#fff}.b-done{background:#2e8b57;color:#fff}
.brief{background:var(--card);border:1px solid var(--line);border-left:4px solid #2f6fde;border-radius:10px;padding:14px 18px;margin:16px 0}
.brief h2{margin:0 0 8px;border:0;padding:0}.brief li{margin:4px 0}.deliver{font:12px/1.5 ui-monospace,Consolas,monospace;margin-top:8px}
.thumb div{background:var(--img);padding:4px;border-radius:4px;overflow:hidden;max-width:100%}.thumb img{image-rendering:pixelated;max-width:100%;display:block}
"""
    h = ["<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>",
         "<title>Eltale Image Text</title><style>%s</style></head><body><main>" % css,
         "<h1>Eltale Monsters: images with Japanese text</h1>",
         "<p class=lead>%s</p>" % html.escape(notes.get("intro", "")), BRIEF, "<nav>"]
    used = [g for g in GROUPS if any(by_id[i]["group"] == g[0] for i in text_ids if i in by_id)]
    for g, title in used:
        h.append("<a href='#%s'>%s</a>" % (g, title))
    h.append("<a href='#skipped'>Text-free (skipped)</a></nav>")
    for g, title in used:
        h.append("<h2 id='%s'>%s</h2><div class=cards>" % (g, title))
        for i in text_ids:
            m = by_id.get(i)
            if not m or m["group"] != g:
                continue
            n = notes["images"][i]
            scale = n.get("scale", 3 if m["w"] <= 220 else 2)
            w = (m["crop"] or m["w"]) * scale
            fmt = "%s %dx%d" % (m["fmt"], m["crop"] or m["w"], m["h"])
            if m["stride"] != m["w"]:
                fmt += " (row stride %d px)" % m["stride"]
            pal = (", palette at %s" % m["tlut_off"]) if m["tlut_off"] else ""
            owner = n.get("owner", "sol")
            h.append("<div class=card><span class='badge b-%s'>%s</span><div class=pic>" % (owner, OWNERS[owner]))
            h.append("<img src='%s' width=%d alt='%s'></div>" % (m["png"], w, i))
            if owner == "sol":
                cw, ch = m["crop"] or m["w"], m["h"]
                h.append("<div class=deliver>Deliver: %s.png &middot; transparent PNG &middot; %d:%d shape, e.g. %dx%d (4x)</div>"
                         % (html.escape(i), cw, ch, cw * 4, ch * 4))
            h.append("<div class=meta>%s &middot; ROM %s &middot; %s%s</div>" % (html.escape(m["tex"]), m["tex_off"], fmt, pal))
            h.append("<div class=where>%s</div>" % html.escape(n.get("where") or m["where"]))
            h.append("<div class=lbl>Japanese</div><div class=ja lang=ja>%s</div>" % html.escape(n["ja"]))
            h.append("<div class=lbl>English</div><div class=en>%s</div>" % html.escape(n["en"]))
            if n.get("layout"):
                h.append("<div class=note>%s</div>" % html.escape(n["layout"]))
            h.append("</div>")
        h.append("</div>")
    h.append("<h2 id=skipped>Images that are text-free (skipped)</h2><p class=lead>%s</p><div class=grid>" %
             html.escape(notes.get("skipped_intro", "")))
    for m in manifest:
        if m["id"] in notes["images"]:
            continue
        sc = 2 if m["w"] <= 160 else 1
        h.append("<div class=thumb><div><img src='%s' width=%d loading=lazy alt=''></div>%s<br>%s %dx%d</div>" %
                 (m["png"], (m["crop"] or m["w"]) * sc, html.escape(m["id"]), m["fmt"], m["crop"] or m["w"], m["h"]))
    h.append("</div>")
    if notes.get("not_decoded"):
        h.append("<h2>Not decoded / unsure</h2><ul>")
        for t in notes["not_decoded"]:
            h.append("<li>%s</li>" % html.escape(t))
        h.append("</ul>")
    h.append("</main></body></html>")
    with open(os.path.join(out, "catalog.html"), "w", encoding="utf-8") as f:
        f.write("\n".join(h))
    print("catalog ->", os.path.join(out, "catalog.html"))


if __name__ == "__main__":
    main()
