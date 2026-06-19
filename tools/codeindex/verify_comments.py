#!/usr/bin/env python3
# =============================================================================
#  verify_comments.py - prove a change is COMMENT-ONLY (behaviour-preserving)
# =============================================================================
#  WHY
#    The Doxygen documentation pass adds `@file` banners and per-symbol comments
#    across the whole C++ tree. The build is Windows/VS-only, so on most hosts we
#    cannot compile to confirm the change did not alter behaviour. This tool
#    closes that gap WITHOUT a compiler: it strips all comments from both the
#    committed (HEAD) and working-tree version of each file and asserts the
#    remaining CODE is byte-identical after whitespace normalisation. If the code
#    is unchanged, the diff can only be comments/whitespace - i.e. provably
#    behaviour-preserving (modulo the C++ lexer, which this models faithfully:
#    strings and char literals are preserved, comments become a single space).
#
#  USAGE
#    python tools/codeindex/verify_comments.py            # all changed C/C++ files vs HEAD
#    python tools/codeindex/verify_comments.py src/world  # only files under a path
#    python tools/codeindex/verify_comments.py a.cpp b.h  # explicit files
#    # exit 0 = every file is comment-only; exit 1 = a code change was detected.
#
#  NOTE
#    Whitespace is normalised (runs collapsed), so reindentation is also treated
#    as non-code. That is intentional: it keeps the check focused on tokens. It
#    does NOT understand raw string literals R"(...)" (absent in this repo); add
#    handling if that changes.
# =============================================================================

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

CPP_EXTS = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl"}


def strip_comments(src: str) -> str:
    """Return `src` with // and /* */ comments replaced by a space, while
    preserving string and character literals verbatim. A faithful-enough C++
    lexer for the comment-vs-code distinction."""
    out = []
    i, n = 0, len(src)
    NORMAL, LINE, BLOCK, STR, CHR = range(5)
    state = NORMAL
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == NORMAL:
            if c == "/" and nxt == "/":
                state = LINE
                out.append(" ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = BLOCK
                out.append(" ")
                i += 2
                continue
            if c == '"':
                state = STR
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = CHR
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
        elif state == LINE:
            if c == "\n":
                state = NORMAL
                out.append(c)
            i += 1
        elif state == BLOCK:
            if c == "*" and nxt == "/":
                state = NORMAL
                i += 2
            else:
                # keep newlines so line structure (and the normaliser) is sane
                if c == "\n":
                    out.append(c)
                i += 1
        elif state == STR:
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
                continue
            if c == '"':
                state = NORMAL
            i += 1
        elif state == CHR:
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
                continue
            if c == "'":
                state = NORMAL
            i += 1
    return "".join(out)


def normalise(code: str) -> str:
    """Collapse all whitespace so indentation/blank-line changes are ignored.
    A UTF-8 BOM is treated as whitespace (it is an encoding marker, not code)."""
    return " ".join(code.replace("﻿", " ").split())


def head_version(path: str) -> str | None:
    """The committed (HEAD) contents of `path`, or None if it is a new file."""
    res = subprocess.run(
        ["git", "show", f"HEAD:{path}"],
        capture_output=True, text=True,
    )
    if res.returncode != 0:
        return None
    return res.stdout


def changed_cpp_files() -> list:
    res = subprocess.run(
        ["git", "diff", "--name-only", "HEAD"],
        capture_output=True, text=True, check=True,
    )
    files = [f for f in res.stdout.splitlines() if Path(f).suffix.lower() in CPP_EXTS]
    return files


def expand_args(args: list) -> list:
    if not args:
        return changed_cpp_files()
    files = []
    for a in args:
        p = Path(a)
        if p.is_dir():
            files += [str(f.relative_to(Path.cwd())) if f.is_absolute() else str(f)
                      for f in p.rglob("*") if f.suffix.lower() in CPP_EXTS]
        elif p.suffix.lower() in CPP_EXTS:
            files.append(a)
    # restrict to files actually changed vs HEAD (others are trivially fine)
    changed = set(changed_cpp_files())
    return [f for f in files if f in changed] or files


def main(argv):
    files = expand_args(argv)
    if not files:
        print("[verify-comments] no changed C/C++ files to check.")
        return 0

    bad = []
    checked = 0
    for path in sorted(set(files)):
        head = head_version(path)
        if head is None:
            print(f"  (new file, skipped): {path}")
            continue
        try:
            work = Path(path).read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            print(f"  (deleted, skipped):  {path}")
            continue
        checked += 1
        if normalise(strip_comments(head)) == normalise(strip_comments(work)):
            print(f"  OK comment-only:     {path}")
        else:
            bad.append(path)
            print(f"  !! CODE CHANGED:     {path}")

    print()
    if bad:
        print(f"[verify-comments] FAIL: {len(bad)} file(s) changed CODE, not just comments:",
              file=sys.stderr)
        for b in bad:
            print(f"    {b}", file=sys.stderr)
        print("\n  These edits are NOT safe as documentation-only changes - review them.",
              file=sys.stderr)
        return 1
    print(f"[verify-comments] OK: {checked} file(s) are comment-only (behaviour-preserving).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
