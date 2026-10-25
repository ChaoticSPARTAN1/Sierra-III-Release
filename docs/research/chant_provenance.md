# The loading chant's bytes: where they came from (2026-10-17)

The question: should ChupathingyCE carry `compressed_monk_tone` in its source at all? It looked like "the exact kind" of mistake that slips through upstream. This note answers it for the port's go-live (`go_live.md` 3.5) and for a possible upstream issue. Not legal advice. Labels: [D] read in code or git history, [W] the web, [U] inference.

## What it is

- `source/interface/progress_bar.c:322–323` (ChupathingyCE v0.7.5d, pinned `d98982b`): `byte compressed_monk_tone[18396]`, ~1,150 lines of hex. Its comment says only what it is for, not where it came from [D].
- It is initialised data from the Xbox game's executable (`default.xbe`, `.data`): the file's symbol header places it at 0x2FD5A8, and the deleted `config/symbols.json` (commit `e830efbd`) gave its file offset as 3,134,896 [D].
- It plays as Xbox ADPCM, mono, 22,050 Hz (`progress_bar.c:2201–2204`) [D]. The port plays it as the chant's four voices (`psp/src/psp_loading.c:79–99`) [D].

## How it got into upstream's source [D]

| Commit | Date | What |
|---|---|---|
| `e830efbd` | 2025-11-27 | Initial commit: the symbol only |
| `3be7d026` | 2026-09-03 | `extern byte compressed_monk_tone[18396];`: still no bytes |
| `62b3cec7` | 2026-09-07 | Logged as unresolved: "do not extract an opaque byte table merely to manufacture data credit" |
| `9b26ebcf` | 2026-09-21 | Listed as an "unwritten 18 KB blob" |
| `4f4c8502` | 2026-09-22 | The extern becomes the full byte table ("Reconcile Lane C with canonical house-rule audit"; branches `jonas/exact-pilots`, `claude/lane-c-graphics-ui-20260920`; no PR shown [W](https://github.com/ChupathingyCE/chupathingyce/commit/4f4c85021db48dae61252d8e1301c60e1c2cc138)) |
| `4adc3a87` | 2026-09-30 | The tooling and notes that called it an extracted asset removed; the array kept |

So the project's own notes said not to do this, and then it was done.

## Upstream's own rules

- README: "Bring your own copy of the game: we never host or share Microsoft's files" (:21–22); "doesn't include the game's maps, sounds or art" (:94) [D].
- CONTRIBUTING, "Game files" (:41–43): "Don't commit, attach or link maps, disc images or other copyrighted game data" [D].
- Its other tables under `source/` are engine constants (a vector palette, vertex shader declarations), not media; `port/assets/` holds redraws and freely licensed fonts, and reads anything else from the player's own `ui.map` [D]. The chant is the one exception [U].

## What it means for Sierra-III

- Every Sierra-III EBOOT has carried these bytes, and with them so have the `build-archive` zips. That conflicts with the port's own rule, "No game data, ever" (`CLAUDE.md`) [D].
- **The fix (b139, `plan_loading_screen.md` §1):** the bake reads the 18,396 bytes from the player's own `default.xbe` at the known offset and checks them against a hash kept in the source (a hash isn't game data); the EBOOT links without the array, and the loading screen is silent if the bake has no chant [U].
- **Upstream:** a plain issue is appropriate: file and line, commit `4f4c8502`, CONTRIBUTING's "Game files" and their own note in `62b3cec7`, suggesting the bytes load from the XBE at run time (silent when missing), with no bytes attached. As a removal backed by their own rule it also fits the port's upstreaming rule, if the maintainer sends the fix [U].
