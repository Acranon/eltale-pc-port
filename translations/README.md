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
