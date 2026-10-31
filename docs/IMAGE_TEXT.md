# Images with Japanese text

Total: 45 images.

Every 2D image whose pixels contain Japanese, found by decoding all title, prologue, menu, map,
HUD, Controller Pak and ending images. Regenerate the PNGs and a visual catalog (Japanese and English side
by side) from your own ROM with:

    python tools/images/extract.py <rom.z64>

Output goes to `out/images/` (git-ignored): `png/`, `manifest.json`, and `catalog.html` when
`out/images/notes.json` holds the translations. ROM offsets are for the Japanese ROM. Line breaks in the
English are marked with ` / ` and paragraph breaks with ` // `.

Formats: CI4/CI8 = 4/8-bit colour-indexed with an RGBA5551 palette ("pal" = palette ROM offset).

## Title screen (2)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| title_center_image_tex | 0xDB0C70 | CI8 194x76, pal 0xDB0A70 | Title screen logo, drawn at (63,69). Note: Title logo, 194x76, drawn at (63,69). Plenty of room: 6 Latin letters in the same blue/gold block style with the red extrusion. | ELTALE |
| title_menu_atlas_tex | 0xDB0240 | CI4 176x12, pal 0xDB0220 | Title menu, three items on one row at y=160; atlas pieces s=0 w=54, s=54 w=55, s=109 w=66 (one 176x12 CI4 atlas). Note: Each piece is only 12 px tall and 54/55/66 px wide. 'New Game' and 'Continue' fit; if 'Delete Data' is too wide use 'Erase' or 'Delete'. Piece offsets live in the title DLs (D_8020F2A8/F300/F358), so widths can change if the atlas is re-cut. | New Game / Continue / Delete Data |

## Prologue (8)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| D_80218800_ovl_intro | 0xDD29E0 | CI8 194x76, pal 0xDD27E0 | Prologue (anims 3 and 30): the logo again, revealed left to right in 15-px columns at (63,85); separate copy with its own palette. Note: Same art as the title logo; redraw once and paste into both. | ELTALE |
| intro_sprite_anim_8_tex | 0xE08438 | CI4 194x114 (rows 208 px), pal 0xE0B290 | Prologue page 1 (anim 8), white text over the map, 194x114 shown at (63,63). Note: Japanese uses 7 lines of ~11 px; the English needs 9-10 lines, so use a slightly smaller font or tighter leading. Texture is 208x114 (CI4). | ~ Celtland ~ // An island where the power of the spirits / lives and breathes in every place. // Here dwell the Spiritcallers, who make / pacts with the spirits and borrow / their power to work magic. / Jean-Jacques, an apprentice Spirit Mage / of the Spirit Sanctum of Melrode, / is one of them. |
| intro_sprite_anim_12_tex | 0xE0B498 | CI4 156x99 (rows 160 px), pal 0xE0D390 | Prologue page 2 (anim 12), 156x99 shown at (82,71). Note: Only 156 px wide: break lines short (about 26 characters at 6 px). | ~ Stonia ~ // The great northern kingdom of magic, / home of the Spirit Sanctum of Melrode. // It is ruled by King Glen Scottfort, / whom the people call a hero. |
| intro_sprite_anim_17_tex | 0xE0D598 | CI4 172x78 (rows 176 px), pal 0xE0F070 | Prologue page 3 (anim 17), 172x78 shown at (74,81) | ~ Angulus ~ // The merchant kingdom of central Celtland. / Ruled by Queen Diana, it is the fairest / and richest land in all Celtland. |
| intro_sprite_anim_22_tex | 0xE0F278 | CI4 171x65 (rows 176 px), pal 0xE108D8 | Prologue page 4 (anim 22), 171x65 shown at (75,88). Note: 65 px tall: 4 English lines plus the heading is the limit; drop the blank line if needed. | ~ Veragoon ~ // The kingdom of the sword, at the island's / southern tip: a land of iron and fire, / ruled by the ambitious King Geas, / who prizes might above all. |
| intro_sprite_anim_29_tex | 0xE10AE0 | CI4 155x39 (rows 160 px), pal 0xE11718 | Prologue page 5 (anim 29), 155x39 shown at (83,101). Note: 3 lines in 39 px, 155 px wide. | The three kingdoms were bound by treaty, / and their peoples lived / in calm and peace. |
| intro_sprite_anim_34_tex | 0xE11920 | CI4 145x39 (rows 160 px), pal 0xE12558 | Prologue page 6 (anim 34), 145x39 shown at (88,101). Note: Only 145 px wide. | But one night, a book hidden / deep within the Spirit Sanctum / was stolen, and vanished. |
| intro_sprite_anim_32_tex | 0xE12760 | CI4 121x43 (rows 128 px), pal 0xE13228 | Prologue page 7 (anim 32), 121x43 shown at (100,99). Note: Narrowest page: 121 px. Keep lines under ~20 characters. | It was the beginning / of a peril such as Celtland / had never known. |

## Boot Controller Pak / controller warnings (5)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| D_80259250_ovl_intro | 0xE13430 | CI4 208x112, pal 0xE161B8 | Boot, no Controller Pak (Intro_RunNoPakScreen): 200x112 at (60,64). 'No' shows the next page (intro_pak_help).. Note: The cursor is drawn at x=90 / 161, y=157 (screen), so 'Yes' and 'No' must stay where the Japanese Yes/No are. | No Controller Pak is inserted. / You can play as you are, but you will / not be able to save your game. / Is that all right? // Yes      No |
| D_8025C1E0_ovl_intro | 0xE163C0 | CI4 208x42, pal 0xE174D8 | Boot: second page of the no-Pak and Pak-error screens (B returns), 200x42 at (60,99) | Turn the power off, insert a / Controller Pak, then turn / the power on again. |
| D_8025D500_ovl_intro | 0xE176E0 | CI4 208x112, pal 0xE1A468 | Boot: Controller Pak full (Intro_RunPakFullScreen), 200x112 at (60,64); A/Start continues | The Controller Pak is full. Saving / this game needs 1 note and 2 pages. / To save on this Controller Pak, erase / data you no longer need in the / Controller Pak Menu to make room. |
| D_80260490_ovl_intro | 0xE1A670 | CI4 208x42, pal 0xE1B788 | Boot: no controller (Intro_DrawNoControllerMessage), 200x42 at (60,99) | Turn the power off and / connect a controller. |
| D_802617B0_ovl_intro | 0xE1B990 | CI4 208x112, pal 0xE1E718 | Boot: wrong accessory in the controller (Intro_RunPakErrorScreen), 200x112 at (60,64). 'No' shows intro_pak_help.. Note: Cursor at x=90 / 164, y=157: keep Yes/No under it. | Something other than a Controller Pak / is inserted. You can play as you are, / but you will not be able to save. / Is that all right? // Yes      No |

## Pause menu (10)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| menu_sprite_26_tex | 0x63690 | CI8 82x13, pal 0x62A98 | Pause menu, Status page title (sprite 0x1A, 82x13 at x+119,y=25). Note: Keep the two grey bullets; the word sits in about 60 px between them. | Status |
| menu_sprite_33_tex | 0x64280 | CI8 86x14, pal 0x62A98 | Pause menu, Options page title (sprite 0x21, 86x14 at x+117,y=24) | Options |
| menu_sprite_34_tex | 0x64740 | CI8 54x12, pal 0x62A98 | Options page, first row label (sprite 0x22, 54x12 at x+49,y=59). Note: The row picks message speed; 'Text Speed' just fits 54 px, or use 'Message'. | Text Speed |
| menu_sprite_35_tex | 0x649D0 | CI8 42x12, pal 0x62A98 | Options page, second row label (sprite 0x23, 42x12 at x+53,y=104) | Sound |
| menu_sprite_86_tex | 0x675F0 | CI8 26x9, pal 0x62A98 | Options page, text speed, left choice (sprite 0x56, 26x9 at x+108,y=60). Note: 26x9: tiny; 4 letters max. | Slow |
| menu_sprite_87_tex | 0x676E8 | CI8 26x9, pal 0x62A98 | Options page, text speed, right choice (sprite 0x57, 26x9 at x+233,y=60) | Fast |
| menu_sprite_42_tex | 0x65138 | CI8 26x9, pal 0x62A98 | Options page, sound volume, left choice (sprite 0x2A, 26x9 at x+108,y=107). Note: 26x9; 'Low' also works. | Soft |
| menu_sprite_41_tex | 0x65040 | CI8 26x9, pal 0x62A98 | Options page, sound volume, right choice (sprite 0x29, 26x9 at x+233,y=107). Note: 'High' also works. | Loud |
| menu_sprite_43_tex | 0x65230 | CI8 58x16, pal 0x62A98 | Pause menu, Map page title (sprite 0x2B, 58x16 at x+131,y=24) | Map |
| menu_sprite_45_tex | 0x656A0 | CI8 86x13, pal 0x62A98 | Pause menu, Elements (spirit levels) page title (sprite 0x2D, 86x13 at x+117,y=25) | Elements |

## World map (10)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| map_region_0 | 0xF2FD70 | CI8 244x185, pal 0xF2FB70 | Pause menu map, region 0 (244x185 at (39,29)); labels are painted into the map. Note: Title top-left in the large shaded style; place labels in the small dark style next to their icons. | Holy Plain (title) / Impaness / Darlam / Jeanbat Forest |
| map_region_1 | 0xF3AFD0 | CI8 244x185, pal 0xF3ADD0 | Pause menu map, region 1. Note: Title is on two lines in the Japanese; 'Darlam / Flats' can stay on two lines. | Darlam Flats (title) / Jeanbat Forest / Glengo Forest |
| map_region_2 | 0xF46230 | CI8 244x185, pal 0xF46030 | Pause menu map, region 2 | West Angulus (title) / Blue Cape / Lalapool / Task Hazard / Haliel / Windward Forest |
| map_region_3 | 0xF51490 | CI8 244x185, pal 0xF51290 | Pause menu map, region 3 | Rideen Suburbs (title) / Rideen Castle / Veragoon Tunnel |
| map_region_4 | 0xF5C6F0 | CI8 244x185, pal 0xF5C4F0 | Pause menu map, region 4 | Dingam Desert (title, bottom-left) / Veragoon Tunnel / Grinos / Boiled Hole |
| map_region_5 | 0xF67950 | CI8 244x185, pal 0xF67750 | Pause menu map, region 5 | Veragoon Moor (title) / Boiled Hole / Blanick / Blanick Castle |
| map_region_6_small | 0xF72BB0 | CI8 65x18, pal 0xF729B0 | Pause menu map, region 6: no map, just this 65x18 name panel at (127,112). Note: 65 px is tight for 11 letters; 'Aqua Isle' or a condensed face. Size is in menu_map_small_sizes, so it can be widened. | Aqua Island |
| map_region_7_small | 0xF73250 | CI8 65x19, pal 0xF73050 | Pause menu map, region 7: 65x19 name panel at (127,112) | Lake Lenos |
| map_region_8_small | 0xF73930 | CI8 96x17, pal 0xF73730 | Pause menu map, region 8: 96x17 name panel at (112,112) | Shelwood |
| map_region_9_small | 0xF74190 | CI8 99x18, pal 0xF73F90 | Pause menu map, region 9: 99x18 name panel at (110,112). Note: 99 px for 16 letters: use a condensed face, or 'The Floating Sanctum' on two lines if the height is raised. | Floating Sanctum |

## Battle HUD (1)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| hud_glyph_font | 0xD7CF40 | CI4 144x45, pal 0xD7DBE8 | Battle HUD: 8x9 katakana glyphs used to write spell names under the spell wheel (Hud_DrawGlyphString); the last row spells 'can't use', shown for a spell you cannot cast. Note: Spell names are strings in a table at RAM 800C03D8 (ROM 0xD49498 in the D490C0 block), encoded as glyph numbers into this sheet. Translating them means drawing Latin 8x9 glyphs into this sheet and re-encoding that table, not redrawing a picture. 'Can't use' could be 'Sealed' or 'No MP' to stay short. | (a font, not a picture) / 'Can't use' |

## Controller Pak manager (8)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| D_801030A8_ovl_DA4570 | 0xDA7618 | CI4 160x14, pal 0xDA75F8 | Controller Pak manager: heading, 160x14 at (82,30). Note: 19 letters in 160 px fits. | Controller Pak Menu |
| D_80103528_ovl_DA4570 | 0xDA7A98 | CI4 64x12, pal 0xDA7A78 | Pak manager: prompt, 64x12 at (94,55). Note: 64 px: 'Select Note' or 'Pick a Note'. | Select a Note |
| D_801036C8_ovl_DA4570 | 0xDA7C38 | CI4 32x12, pal 0xDA7C18 | Pak manager: exit option, 32x12 at (204,54) | Exit |
| D_80103878_ovl_DA4570 | 0xDA7DE8 | CI4 32x12, pal 0xDA7DC8 | Pak manager: column header, 32x12 at (149,73) | Name |
| D_80103958_ovl_DA4570 | 0xDA7EC8 | CI4 32x12, pal 0xDA7EA8 | Pak manager: column header, 32x12 at (254,73). Note: 32 px: 'Pages' fits, or 'Size'. | Pages |
| D_80103A38_ovl_DA4570 | 0xDA7FA8 | CI4 80x12, pal 0xDA7F88 | Pak manager: free-space label, 80x12 at (126,173); the number is drawn after it | Free pages: |
| D_80103DD8_ovl_DA4570 | 0xDA8348 | CI4 32x12, pal 0xDA8328 | Pak manager: erase option, 32x12 at (107,193) | Erase |
| D_80103C38_ovl_DA4570 | 0xDA81A8 | CI4 64x12, pal 0xDA8188 | Pak manager: cancel option, 64x12 at (174,193) | Cancel |

## Ending (1)

| Asset | ROM | Format | Where | English |
|---|---|---|---|---|
| ending_sprite_anim_1_tex | 0xE37BD8 | CI4 267x310 (rows 272 px), pal 0xE42090 | Ending epilogue (ending anim 1): a 267x310 page that scrolls up from y=192 over the Floating Sanctum backdrop; texture rows are 272 px (CI4, 16 colours). Note: Keep the three paragraph groups at about the same heights (rows 0-130, 170-280, ~300): the scroll timing and backdrop fades are tied to the page height. Lines may run to ~44 characters at 6 px. | Only a handful know that it was / a boy of ten who saved Celtland. / Knowing nothing of it, the people / return to their calm and peaceful lives. / In time the ruined towns will heal, / will be rebuilt, and will grow anew. / In time the wounded will rise again / and walk on, hand in hand. // And people will feel it: / the warmth of fire, the gentle wind on the skin, / the coolness of flowing water, / and the rich bounty of the earth... // So long as people never forget / to speak with the spirits, / the world shall never perish. // So long as we walk with the spirits / who watch over the world... |

## Not covered

- Fonts (main text font, Controller Pak manager font) are glyph sheets, not pictures.
- The battle HUD spell names are strings drawn with the katakana mini-font above (table at RAM 0x800C03D8,
  ROM 0xD49498); translating them needs Latin glyphs in that font plus a re-encoded table.
- 3D map and model textures, sky backdrops, item icons and effects were not decoded.
- Already English, no change needed: PUSH START, the copyright line, the boot splashes, the stat labels
  (Hp, Mp, Def, Agi, exp, No., days, Name), the battle element panel, Miss!/Escape!, the Pak manager NO.
  header, all ending staff credits and the End logo.
