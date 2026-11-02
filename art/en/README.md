# English art

The English versions of the game's pictures with text in them (`docs/IMAGE_TEXT.md`).
`tools/images/build_en_images.py` turns these, with the user's ROM, into what the port swaps in.

- `<name>.png`: labels drawn for this project (the owner, with an AI image tool), any size, the
  original's proportions; the build shrinks them and maps them to the original's palette.
  `title_menu_atlas.png` is the title menu strip (New Game / Continue / Delete Data);
  `title_center_image.png` the logo (shrunk to 194x76 with a palette of its own).
- The Options page's four choices (`menu_sprite_86`, `_87`, `_42`, `_41`: Slow, Fast, Soft, Loud) are 26x9: the
  build draws them in a pixel font in the original's style instead (`small_label`), as their PNGs shrunk 8 times
  lost the letters' top and bottom rows. The PNGs are kept for a larger use.
- `pages.json`: the English of the pictures that are mostly text (prologue pages, warning screens,
  the epilogue page), drawn in the game's style by the build.
- `maps.json`: the world maps' place names: where the Japanese is, and the English.

No ROM content is kept here: anything taken from the original pictures (the grey bullets of the
pause-menu headings, the maps themselves) is read from the ROM at build time.
