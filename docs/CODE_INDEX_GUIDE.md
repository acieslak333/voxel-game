<!-- ===========================================================================
  CODE_INDEX_GUIDE.md — how to code-index & document this repo, for future
  iterations (human-readable companion to the `/skill code-index` skill).
============================================================================ -->

# Code Indexing & Documentation Guide

How to keep this repository **navigable and self-documenting** as it changes. It
is the playbook behind [`docs/CODE_INDEX.md`](CODE_INDEX.md) and is runnable as a
Claude Code skill: **`/skill code-index`**.

Read this when you: add/move/delete a source file, finish a feature, notice the
index has drifted, or are onboarding and want to understand how the docs are
organised.

---

## 1. The three layers

Documentation here is deliberately layered so each layer has one job and a clear
owner. Keep them in sync — a stale layer is worse than no layer.

| Layer | Artifact | Owner | Nature |
|---|---|---|---|
| **A. Machine index** | `docs/index/` — `symbols.json`, `symbols.tags`, `SYMBOLS.md`, `manifest.json` | `tools/codeindex/codeindex.py` | Generated, exhaustive, never editorialised. Every file + symbol + line. |
| **B. Curated prose** | `docs/CODE_INDEX.md` (hub) + `src/*/README.md` (per-subsystem) | Humans/agents | Hand-written architecture: responsibilities, flows, invariants, "read-first". |
| **C. In-source Doxygen** | `@file` banners + per-symbol comments in `.h`/`.cpp` | Humans/agents | Documentation that lives next to the code; renders to HTML via Doxygen. |

**Why three?** Layer A is always complete and never lies, but it cannot explain
*why*. Layer B explains the architecture but goes stale silently. Layer C is
closest to the code and survives refactors, but is scattered. Together: an agent
greps Layer A to locate, reads Layer B to understand, and reads Layer C at the
point of edit. The `code-index` skill regenerates A and reminds you to update B
and C.

---

## 2. Regenerating the machine index (Layer A)

Stdlib-only Python — no ctags, no build, runs anywhere:

```bash
python tools/codeindex/codeindex.py gen     # rewrite docs/index/*
python tools/codeindex/codeindex.py check    # exit 1 if the index is stale
```

`gen` scans `src/ shaders/ tools/ tests/ scripts/ assets/` and emits:

- **`symbols.json`** — structured `{file → {lang, lines, sha1, vendored, symbols[]}}`.
- **`symbols.tags`** — ctags-format; point your editor/agent at it for jump-to-symbol.
- **`SYMBOLS.md`** — human-readable per-file symbol listing (the exhaustive view).
- **`manifest.json`** — `sha1` of every scanned file; the staleness check's baseline.

The C++ extraction is a **regex heuristic**, not a parser (it has no compiler /
clang). It is a navigation aid: a missed or duplicated symbol is a known
limitation, never a correctness bug. Vendored files (`utilities/noise/FastNoise`,
`third_party/`, `tools/vendor/`) are listed but not deep-scanned.

> **Always run `gen` and commit `docs/index/` in the same change as a source
> edit.** The CI guard and the pre-commit hook run `check` and will flag a stale
> index (see §6).

---

## 3. Updating the curated prose (Layer B)

When you change the shape of a subsystem (new file, moved responsibility, new
cross-cutting concern), update:

1. The relevant table/section in **`docs/CODE_INDEX.md`** (the subsystem map, and
   any of: threading invariants, worldgen pipeline, streaming, shaders, asset
   catalog, CLI/env vars).
2. The matching **`src/<dir>/README.md`**.

Keep it dense and agent-first: tables over paragraphs, `file:line` references
(they're clickable), explicit "read-first" ordering, and call out invariants.
Don't restate what Layer A already lists exhaustively — link to
`docs/index/SYMBOLS.md` instead.

**The drift rule.** When a doc and the code disagree, **the code wins** — fix the
doc (or point it at the index) rather than propagate the stale claim. The index
carries a [Documentation drift](CODE_INDEX.md#documentation-drift) section
precisely so future readers trust code over prose; add to it when you find a new
divergence you can't immediately fix.

---

## 4. Writing in-source Doxygen (Layer C)

### 4.1 The comment template

Every `.h`/`.cpp` (except vendored files) gets a **file banner** at the top
(after `#pragma once` / include guard and any license header):

```cpp
/**
 * @file World.h
 * @brief Chunk-grid owner: generation, lighting, editing, streaming.
 *
 * Owns the BlockRegistry and all chunk + light data; answers world-space
 * queries used by the renderer and player. Worldgen is a pure function of
 * (seed, coords) — generateColumnInto() is const so the pregen thread can call
 * it safely.
 * @see docs/CODE_INDEX.md
 */
```

Per **public type** (class/struct/enum):

```cpp
/** @brief One voxel: a block id (0 == air) plus a metadata byte. */
struct Block { ... };
```

Per **public function / method** (declaration in the header, definition in the
`.cpp`):

```cpp
/**
 * @brief Apply a single block edit and return the chunks that must remesh.
 * @param wx,wy,wz World-block coordinates.
 * @param b        The new block.
 * @return The containing chunk plus any neighbours whose mesh/light changed.
 * @warning Main-thread only; mutates the window. Drain mesh workers first.
 */
std::vector<glm::ivec3> setBlock(int wx, int wy, int wz, Block b);
```

Tags used: `@file @brief @param @return @tparam @note @warning @see`. Use `///`
for one-liners, `/** … */` for blocks (start continuation lines with ` * `).
`JAVADOC_AUTOBRIEF` is on, so the first sentence is the brief.

### 4.2 The rules (so the change stays safe & consistent)

1. **Comment-only.** Add comment lines and blank lines; you may convert an
   existing leading `//` banner into an `@file` block. **Never** change, reorder,
   reformat, or delete a line of code. (The build is Windows/VS-only, so this is
   how an edit is proven safe without compiling — see §5.)
2. **No trailing comments on existing code lines** (don't turn `int x;` into
   `int x; ///< foo`). Put member docs on their own `///` line *above* the member.
   This keeps the comment-only check simple and ironclad.
3. **Never** insert a comment between preprocessor line-continuations (a line
   ending in `\`) or inside a `\`-continued macro.
4. **Accuracy over volume.** Don't invent behaviour. Unsure → describe at a high
   level or omit. Match the repo's terse style. Big `.cpp` files: prioritise the
   `@file` banner + major functions; you needn't comment every static helper.
5. **Document invariants loudly.** Threading rules, determinism requirements, and
   format-version contracts get `@warning`/`@note`. See
   [Threading model](CODE_INDEX.md#threading-model--invariants).
6. **Skip vendored code** (`utilities/noise/FastNoise.*`, `third_party/`): a
   single `@file` line noting it's vendored, nothing more.

### 4.3 Doing a large pass with sub-agents

The initial pass documented ~110 files by partitioning `src/` by subsystem and
giving each `cpp-pro` sub-agent the template + rules above for a **disjoint** file
list (so no two agents touch the same file). After each agent, run the
comment-only verifier (§5). To repeat for a new batch, copy the prompt skeleton
in the `code-index` skill.

---

## 5. Proving a doc pass is safe without a compiler

Because the build is Windows/VS-only, most hosts can't compile to confirm a
documentation pass didn't change behaviour. Use the verifier:

```bash
python tools/codeindex/verify_comments.py            # all changed C/C++ files vs HEAD
python tools/codeindex/verify_comments.py src/world  # a subset
```

It strips all comments from both the `HEAD` and working-tree version of each file
and asserts the remaining **code is byte-identical** (whitespace-normalised). If
it passes, the diff can only be comments/whitespace — i.e. provably
behaviour-preserving. If it reports `CODE CHANGED`, the listed file has a real
edit; review it before committing.

> Run this after **any** "documentation-only" change, especially agent-authored
> ones, before committing.

---

## 6. Keeping it fresh (automation)

Two guards stop the index rotting:

- **CI** — `.github/workflows/code-index.yml` runs
  `python tools/codeindex/codeindex.py check` on push/PR (Python only, no build).
  A stale index fails the job with the exact drift list.
- **Pre-commit hook** — `scripts/hooks/pre-commit` runs the same `check`. Install
  it once per clone:
  ```bash
  git config core.hooksPath scripts/hooks
  ```
  It warns by default; set `CODEINDEX_HOOK_STRICT=1` to make it block the commit.

When `check` fails: run `gen`, review `git diff docs/index/`, and commit it
alongside your source change.

---

## 7. Adding a file: the checklist

1. Write the file with an `@file` banner + Doxygen on its public symbols (§4).
2. `python tools/codeindex/codeindex.py gen` and commit `docs/index/`.
3. Add it to the matching `src/<dir>/README.md` table and, if it changes the
   subsystem's shape, to `docs/CODE_INDEX.md`.
4. If it's a new `.cpp`, add it to `CMakeLists.txt` (`add_executable`).
5. `python tools/codeindex/verify_comments.py` if you touched existing files.

---

## 8. Rendering the API docs (Doxygen, Layer C → HTML)

Once `doxygen` is installed (it is **not** required to build the game):

```bash
cmake --build build --target docs     # configured target, or:
doxygen Doxyfile                        # from the repo root
```

Output: `docs/doxygen/html/index.html` (git-ignored). The `Doxyfile` is tuned for
this repo (recursive over `src/`, excludes vendored FastNoise + `build/` +
`third_party/`, `JAVADOC_AUTOBRIEF`, source browser on).

---

## TL;DR

```bash
# after any source change:
python tools/codeindex/codeindex.py gen        # refresh the machine index
python tools/codeindex/verify_comments.py      # (if you only added comments) prove it's safe
#   ...update docs/CODE_INDEX.md + src/<dir>/README.md if the shape changed...
git add docs/index src/ docs/CODE_INDEX.md     # commit code + index together
```

Or just run **`/skill code-index`** and follow it.
