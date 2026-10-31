# English art

The English versions of the game's pictures with text in them (`docs/IMAGE_TEXT.md`).
`tools/images/build_en_images.py` turns these, with the user's ROM, into what the port swaps in.

- `<name>.png`: labels drawn for this project (the owner, with an AI image tool), any size, the
  original's proportions; the build shrinks them and maps them to the original's palette.
  `title_menu_atlas.png` is the title menu strip (New Game / Continue / Delete Data);
  `title_center_image.png` the logo (shrunk to 194x76 with a palette of its own).
- `pages.json`: the English of the pictures that are mostly text (prologue pages, warning screens,
  the epilogue page), drawn in the game's style by the build.
- `maps.json`: the world maps' place names: where the Japanese is, and the English.

No ROM content is kept here: anything taken from the original pictures (the grey bullets of the
pause-menu headings, the maps themselves) is read from the ROM at build time.
