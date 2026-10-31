# English translation

`en.tsv`: the English script, one row per game string: `id <TAB> English`. Ids come from
`tools/text/dump.py` (the source and the string's offset in it), so the Japanese for any row is in
your own dump (`python tools/text/dump.py rom.z64` writes `out/text/script.tsv`). No Japanese text
is kept in this repo.

Markup: `⏎` line break, `⏎▼⏎` page break (wait for a button), `{F2}` a Yes/No choice follows.
Lines are at most 42 characters and pages at most 4 lines (until the English text engine sets the
real limits).

Status (2026-10-31): first draft of all 615 strings, translated from the Japanese with Eltale's
own names, then a second verification pass against the Japanese. The owner polishes from here.
The text drawn as pictures (logos, the New Game prologue, the ending's closing pages, menu labels)
is not in here yet.

## en.v2.tsv: accuracy and voice

`en.v2.tsv` has the same ids, written without the original window limits: a page is flowing text
(the English text engine wraps it and adds pages when needed) and `⏎` marks only breaks that
belong to the text (sign lists, inscriptions). Page breaks (`⏎▼⏎`) and `{F2}` stay where the
Japanese has them. Status (2026-10-31): every row retranslated from the Japanese with the speaker
of each NPC message known, each speaker given a voice taken from the Japanese cues (pronouns,
sentence endings, dialect), nuance that v1 cut to fit restored; then an independent check of
every row against the Japanese. 315 rows differ from v1. Rules: American spelling; full item
names (Traveler's White Wings); swearing allowed where the Japanese is genuinely rough.

## In the game (2026-10-31)

v2 is the script the game uses (`text/en.tsv` next to the exe, built from en.v2.tsv). F1 menu,
Game: Text English / Japanese, and English font: the game's own (default, the owner's and testers'
choice) or Royalty Mono, Quest Square, Pixel Sans, Retro Sans, Rune Mono (third_party/fonts, CC0).
How it works: docs/P1_OS_STATUS.md, "English text".

## Text in pictures

`docs/IMAGE_TEXT.md` lists the 45 pictures with Japanese in them; `tools/images/extract.py` decodes
them from your ROM into `out/images/` with a review page (catalog.html: a brief for the artist, who
makes each picture, lettering families). The artist's English art (out/images/sol-en/, local) is
fitted to the game by `tools/images/fit_art.py` (original size and colours, the original bullets on
the pause-menu headings). Status: title menu strip and 22 labels done and accepted pending an
in-game look; the logo is being redesigned; the prologue pages, warning screens, epilogue page,
world maps and the spell-name font are ours to make in code. None are in the game yet (next: a
replacement-image folder the port loads, and the wider English title-menu strip).

## Open questions for the owner

- npc.28F8, npc.2970: a small child says トーゾク (a word she doesn't know): "bannit", or plain
  "bandit"?
- npc.5BB8: a woman remembers "the warmth of" a man from the Sanctum (likely Bart): keep the hint
  as written, or soften it?
- ぼうや / ぼうず ("boy") is "lad", "sweetie" (the flirty Mint Leaves woman), "Child" (an
  innkeeper) or "Kid" (a bounty hunter) by speaker: fine, or one word throughout?
- npc.2E70 / npc.3080: the king says "Sir Bart" in one and "Bart" in the other.
- npc.7B08, npc.7BC0: ウフフ is "Hee hee"; "Tee-hee" may suit the flirty speaker better.
- spot.0960: the "Old Bridgeman" portrait is a guess (the bridge builders' father).
- spot.0818: "Big Soft Kingly Bed" or "Plush King-Size Bed"?
- Item-get message: "Obtained <Item>!" (no articles needed). "You got a/the <Item>!" would need
  a per-item article table in code.
- Controller Pak terms: the game says "note" for a save file on the Pak (the N64 term). Keep, or
  "save file"?

## To check in game

- Item descriptions run up to ~40 characters a line: the item window may be narrower.
- The Controller Pak manager's button rows are laid out with spaces to sit under the cursor.
- Two leftover English battle strings ("Brian escape from battle...") now say Jean-Jacques; they
  may be unused.
