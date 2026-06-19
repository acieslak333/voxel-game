#!/usr/bin/env python3
# =============================================================================
#  codeindex.py - machine-readable symbol index for the voxel-game repo
# =============================================================================
#  WHAT
#    Scans the source tree and emits a navigable, machine-readable index of
#    every C++ type/function, GLSL shader entry point + binding, Python tool
#    function, and data asset. Output lives under docs/index/:
#
#      symbols.json   structured index (files -> symbols with line numbers)
#      symbols.tags   ctags-format file (editors / agents can jump-to-symbol)
#      SYMBOLS.md     human-readable per-file symbol listing
#      manifest.json  sha1 of every scanned file (drives the staleness check)
#
#  WHY
#    `docs/CODE_INDEX.md` is the curated, prose architecture map a human/agent
#    reads first. THIS file is the mechanical, always-exhaustive companion:
#    it never editorialises and it never goes silently stale, because the
#    `check` subcommand compares the tree against `manifest.json` and fails
#    when source changed but the index was not regenerated (see the CI guard
#    in .github/workflows/code-index.yml and scripts/hooks/pre-commit).
#
#  HOW (usage)
#    python tools/codeindex/codeindex.py gen      # regenerate docs/index/*
#    python tools/codeindex/codeindex.py check    # fail if index is stale
#    python tools/codeindex/codeindex.py check --quiet   # CI mode, no diff body
#
#  DESIGN NOTES
#    * Stdlib only - no ctags, no pyyaml, no clang. Runs anywhere Python 3.8+
#      runs, which matters because the build itself is Windows/VS-only and CI
#      hosts may have nothing installed. The C++ extraction is therefore a
#      best-effort REGEX HEURISTIC, not a real parser: it is a navigation aid,
#      not a source of truth for exact signatures. Treat a missing/duplicate
#      symbol as a known limitation, never a correctness bug.
#    * Vendored third-party files (see VENDORED_MARKERS) are listed but NOT
#      deep-scanned for symbols - we do not own their API surface.
#    * Deterministic output: everything is sorted so regeneration produces a
#      stable diff and the staleness check is meaningful.
# =============================================================================

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

# --- Repository layout -------------------------------------------------------
# Resolve the repo root from this file's location: tools/codeindex/codeindex.py
REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "docs" / "index"

# Directories scanned for source. Order is cosmetic (output is sorted anyway).
SCAN_ROOTS = ["src", "shaders", "tools", "tests", "scripts"]

# Files we list but do not deep-scan: vendored third-party code whose API we do
# not own. Matched as a substring of the repo-relative POSIX path.
VENDORED_MARKERS = (
    "utilities/noise/FastNoise",
    "third_party/",
    "tools/vendor/",
)

CPP_EXTS = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl"}
GLSL_EXTS = {".vert", ".frag", ".comp", ".geom", ".tesc", ".tese", ".glsl"}
PY_EXTS = {".py"}
DATA_EXTS = {".yaml", ".yml"}

# C++ keywords that look like "ReturnType name(" but are control flow, never a
# function definition. Used to suppress false positives in the heuristic.
CPP_NOT_FUNCTIONS = {
    "if", "for", "while", "switch", "return", "sizeof", "catch", "throw",
    "and", "or", "not", "alignof", "static_assert", "decltype", "noexcept",
    "case", "do", "else", "constexpr", "requires",
}


@dataclass
class Symbol:
    """One indexed declaration: a type, function, shader binding, etc."""
    name: str
    kind: str          # class|struct|enum|function|namespace|binding|entry|def|key
    line: int
    detail: str = ""   # short signature / context fragment (trimmed)


@dataclass
class FileEntry:
    """Everything known about one scanned file."""
    path: str                       # repo-relative POSIX path
    lang: str                       # cpp|glsl|python|data
    lines: int
    sha1: str
    vendored: bool = False
    symbols: list = field(default_factory=list)  # list[Symbol]


# --- Helpers -----------------------------------------------------------------

def sha1_of(path: Path) -> str:
    h = hashlib.sha1()
    h.update(path.read_bytes())
    return h.hexdigest()


def is_vendored(rel_posix: str) -> bool:
    return any(marker in rel_posix for marker in VENDORED_MARKERS)


def strip_block_comments_stateful(lines):
    """Yield (lineno, code_without_block_comments) so the regex heuristics do
    not trip on commented-out declarations. Single-line // comments are left in
    place (cheap) and handled per-pattern. Tracks /* */ across lines."""
    in_block = False
    for i, raw in enumerate(lines, start=1):
        line = raw
        out = []
        j = 0
        while j < len(line):
            if in_block:
                end = line.find("*/", j)
                if end == -1:
                    j = len(line)
                else:
                    in_block = False
                    j = end + 2
            else:
                start = line.find("/*", j)
                if start == -1:
                    out.append(line[j:])
                    j = len(line)
                else:
                    out.append(line[j:start])
                    in_block = True
                    j = start + 2
        yield i, "".join(out)


# --- C++ extraction (heuristic) ----------------------------------------------

_NS_RE = re.compile(r"^\s*namespace\s+([A-Za-z_]\w*)\s*\{")
_TYPE_RE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(class|struct|union|enum(?:\s+class)?)\s+"
    r"([A-Za-z_]\w*)"
    r"(?!\s*;)"                       # skip forward declarations "class Foo;"
)
# Function-ish: optional return type tokens, then  name( ... )  then { or ; or :
_FUNC_RE = re.compile(
    r"^\s*(?:[A-Za-z_][\w:<>,*&\s]*?\s+)?"   # optional return type
    r"([A-Za-z_~]\w*)\s*"                    # function name (group 1)
    r"\([^;{}]*\)\s*"                        # arg list (no nested braces)
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:->[^;{]*)?"
    r"[{;]"                                  # body open or prototype end
)


def extract_cpp(rel: str, lines):
    syms = []
    for lineno, code in strip_block_comments_stateful(lines):
        # drop trailing // comment for matching
        slash = code.find("//")
        if slash != -1:
            code = code[:slash]
        stripped = code.strip()
        if not stripped or stripped.startswith("#"):
            continue

        m = _NS_RE.match(code)
        if m:
            syms.append(Symbol(m.group(1), "namespace", lineno))
            continue

        m = _TYPE_RE.match(code)
        if m:
            kind = m.group(1).split()[0]
            if kind == "enum":
                kind = "enum"
            syms.append(Symbol(m.group(2), kind, lineno, stripped[:120]))
            continue

        m = _FUNC_RE.match(code)
        if m:
            name = m.group(1)
            if name in CPP_NOT_FUNCTIONS:
                continue
            # crude guard: a bare "Foo()" with no return type and lowercase is
            # usually a call, not a definition - keep only if it looks declared
            syms.append(Symbol(name, "function", lineno, stripped[:120]))
    return syms


# --- GLSL extraction ---------------------------------------------------------

_LAYOUT_RE = re.compile(
    r"layout\s*\(([^)]*)\)\s*"
    r"(uniform|buffer|in|out|push_constant)?\b[^;{]*?"
    r"([A-Za-z_]\w*)?\s*[;{]"
)
_GLSL_ENTRY_RE = re.compile(r"^\s*void\s+main\s*\(")


def extract_glsl(rel: str, lines):
    syms = []
    for lineno, code in strip_block_comments_stateful(lines):
        slash = code.find("//")
        if slash != -1:
            code = code[:slash]
        if _GLSL_ENTRY_RE.match(code):
            syms.append(Symbol("main", "entry", lineno))
            continue
        if "layout" in code:
            m = _LAYOUT_RE.search(code)
            if m:
                qualifier = (m.group(2) or "var").strip()
                name = (m.group(3) or "").strip()
                detail = code.strip()[:120]
                if name:
                    syms.append(Symbol(name, "binding", lineno, f"{qualifier}: {detail}"))
    return syms


# --- Python extraction -------------------------------------------------------

_PY_DEF_RE = re.compile(r"^(?:    )?(def|class)\s+([A-Za-z_]\w*)")


def extract_python(rel: str, lines):
    syms = []
    for i, raw in enumerate(lines, start=1):
        m = _PY_DEF_RE.match(raw)
        if m:
            kind = "def" if m.group(1) == "def" else "class"
            # top-level only (no leading indent) keeps the index focused
            if not raw.startswith((" ", "\t")):
                syms.append(Symbol(m.group(2), kind, i, raw.strip()[:120]))
    return syms


# --- Data (YAML) skim --------------------------------------------------------

_YAML_KEY_RE = re.compile(r"^([A-Za-z_][\w\-]*):")


def extract_data(rel: str, lines):
    """Lightweight skim: top-level YAML keys only. Not a YAML parser."""
    syms = []
    for i, raw in enumerate(lines, start=1):
        m = _YAML_KEY_RE.match(raw)
        if m:
            syms.append(Symbol(m.group(1), "key", i))
    return syms


# --- Scan driver -------------------------------------------------------------

def lang_for(ext: str):
    if ext in CPP_EXTS:
        return "cpp"
    if ext in GLSL_EXTS:
        return "glsl"
    if ext in PY_EXTS:
        return "python"
    if ext in DATA_EXTS:
        return "data"
    return None


def scan() -> list:
    entries = []
    roots = [REPO_ROOT / r for r in SCAN_ROOTS]
    # assets/*.yaml are cataloged (top-level keys) but we cap depth to avoid
    # indexing every texture-adjacent yaml; only the *.yaml directly drives data.
    roots.append(REPO_ROOT / "assets")
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            lang = lang_for(path.suffix.lower())
            if lang is None:
                continue
            rel = path.relative_to(REPO_ROOT).as_posix()
            # Only catalog top-level asset yaml (skip deeply nested data noise)
            if lang == "data" and rel.startswith("assets/"):
                depth = rel.count("/")
                if depth > 2:
                    continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except Exception:
                continue
            lines = text.splitlines()
            vendored = is_vendored(rel)
            entry = FileEntry(
                path=rel,
                lang=lang,
                lines=len(lines),
                sha1=sha1_of(path),
                vendored=vendored,
            )
            if not vendored:
                if lang == "cpp":
                    entry.symbols = extract_cpp(rel, lines)
                elif lang == "glsl":
                    entry.symbols = extract_glsl(rel, lines)
                elif lang == "python":
                    entry.symbols = extract_python(rel, lines)
                elif lang == "data":
                    entry.symbols = extract_data(rel, lines)
            entries.append(entry)
    return entries


# --- Output writers ----------------------------------------------------------

def write_json(entries):
    payload = {
        "schema": "voxel-game/codeindex/1",
        "generator": "tools/codeindex/codeindex.py",
        "note": "Heuristic regex index - a navigation aid, not a parser. "
                "See docs/CODE_INDEX_GUIDE.md.",
        "roots": SCAN_ROOTS + ["assets"],
        "files": [
            {
                "path": e.path,
                "lang": e.lang,
                "lines": e.lines,
                "sha1": e.sha1,
                "vendored": e.vendored,
                "symbols": [asdict(s) for s in e.symbols],
            }
            for e in entries
        ],
    }
    (OUT_DIR / "symbols.json").write_text(
        json.dumps(payload, indent=1, sort_keys=False) + "\n", encoding="utf-8"
    )


def write_tags(entries):
    """Emit a sorted ctags-compatible file. Format per line:
       {name}<TAB>{file}<TAB>/^{pattern}$/;"<TAB>{kind}
    Vim/Emacs/agents can consume this for jump-to-definition."""
    lines = [
        "!_TAG_FILE_FORMAT\t2\t/extended format/",
        "!_TAG_FILE_SORTED\t1\t/0=unsorted, 1=sorted/",
        "!_TAG_PROGRAM_NAME\tcodeindex.py\t//",
    ]
    rows = []
    kind_letter = {
        "class": "c", "struct": "s", "union": "u", "enum": "g",
        "function": "f", "namespace": "n", "binding": "v", "entry": "f",
        "def": "f", "key": "m",
    }
    for e in entries:
        for s in e.symbols:
            # ctags excmd: search pattern; escape backslashes and slashes
            pat = s.detail if s.detail else s.name
            pat = pat.replace("\\", "\\\\").replace("/", "\\/")
            letter = kind_letter.get(s.kind, "x")
            rows.append(f"{s.name}\t{e.path}\t/^{pat}/;\"\t{letter}\tline:{s.line}")
    rows.sort()
    (OUT_DIR / "symbols.tags").write_text(
        "\n".join(lines + rows) + "\n", encoding="utf-8"
    )


def write_symbols_md(entries):
    out = []
    out.append("# Symbol Index (generated)\n")
    out.append(
        "> Auto-generated by `tools/codeindex/codeindex.py gen`. **Do not edit "
        "by hand** - rerun the generator. This is a heuristic regex index "
        "(navigation aid, not a parser); the curated map is "
        "[CODE_INDEX.md](../CODE_INDEX.md).\n"
    )
    # group by top-level directory for readability
    by_dir = {}
    for e in entries:
        top = e.path.split("/")[0]
        by_dir.setdefault(top, []).append(e)

    total_files = len(entries)
    total_syms = sum(len(e.symbols) for e in entries)
    out.append(f"**{total_files} files indexed, {total_syms} symbols.**\n")

    for top in sorted(by_dir):
        out.append(f"\n## `{top}/`\n")
        for e in sorted(by_dir[top], key=lambda x: x.path):
            tag = " _(vendored - not deep-scanned)_" if e.vendored else ""
            out.append(f"\n### `{e.path}` ({e.lines} lines, {e.lang}){tag}\n")
            if not e.symbols:
                out.append("_(no indexed symbols)_\n")
                continue
            for s in e.symbols:
                detail = f" - `{s.detail}`" if s.detail and s.kind not in ("namespace", "entry") else ""
                out.append(f"- L{s.line} **{s.kind}** `{s.name}`{detail}")
    (OUT_DIR / "SYMBOLS.md").write_text("\n".join(out) + "\n", encoding="utf-8")


def write_manifest(entries):
    manifest = {
        "schema": "voxel-game/codeindex-manifest/1",
        "files": {e.path: e.sha1 for e in sorted(entries, key=lambda x: x.path)},
    }
    (OUT_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=1) + "\n", encoding="utf-8"
    )


# --- Subcommands -------------------------------------------------------------

def cmd_gen(_args):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    entries = scan()
    write_json(entries)
    write_tags(entries)
    write_symbols_md(entries)
    write_manifest(entries)
    nsym = sum(len(e.symbols) for e in entries)
    print(f"[codeindex] gen: {len(entries)} files, {nsym} symbols -> {OUT_DIR.relative_to(REPO_ROOT)}/")
    return 0


def cmd_check(args):
    manifest_path = OUT_DIR / "manifest.json"
    if not manifest_path.exists():
        print("[codeindex] check: no manifest.json - run `gen` first.", file=sys.stderr)
        return 2
    stored = json.loads(manifest_path.read_text(encoding="utf-8")).get("files", {})
    current = {e.path: e.sha1 for e in scan()}

    added = sorted(set(current) - set(stored))
    removed = sorted(set(stored) - set(current))
    changed = sorted(p for p in (set(current) & set(stored)) if current[p] != stored[p])

    drift = added or removed or changed
    if not drift:
        print(f"[codeindex] check: OK - index matches {len(current)} source files.")
        return 0

    print("[codeindex] check: STALE - the symbol index is out of date.", file=sys.stderr)
    if not args.quiet:
        for p in added:
            print(f"  + added (not indexed):   {p}", file=sys.stderr)
        for p in removed:
            print(f"  - removed (still indexed): {p}", file=sys.stderr)
        for p in changed:
            print(f"  ~ changed since gen:       {p}", file=sys.stderr)
    print("\n  Fix: python tools/codeindex/codeindex.py gen   (then commit docs/index/)",
          file=sys.stderr)
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Machine-readable symbol index for the voxel-game repo."
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen", help="regenerate docs/index/* from the source tree")
    g.set_defaults(func=cmd_gen)
    c = sub.add_parser("check", help="fail if the index is stale (CI/hook guard)")
    c.add_argument("--quiet", action="store_true", help="suppress the per-file drift list")
    c.set_defaults(func=cmd_check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
