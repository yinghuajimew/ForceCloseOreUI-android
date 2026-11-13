# -*- coding: utf-8 -*-
"""ForceCloseOreUI 特征码扫描 —— 纯字节扫描，不需要 IDA。

用法:
    python scan.py <file.so|file.apk> [...]
    python scan.py --json out.json file.so
"""
from __future__ import annotations

import io
import json
import os
import sys
import zipfile

# 与 ForceCloseOreUI/src/ForceCloseOreUI.cpp SIG_FALLBACK 完全一致（顺序照抄）
SIGS: list[tuple[str, str]] = [
    ("1.26.60", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? B9 ? ? ? D5 F9 03 03 2A"),
    ("1.26.50", "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F5 03 03 2A F6 03 02 2A ? ? ? F9 F4 03 01 AA"),
    ("1.26.40", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FA 03 03 2A F7 03 02 2A ? ? ? F9 F4 03 01 AA"),
    ("1.26.30", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A"),
    ("H8 1.26.20", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 03 2A F8 03 02 2A"),
    ("H4 1.21.130~1.26.0", "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA"),
    ("H5 1.26.0~1.26.10", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA"),
    ("H2 1.21.90~1.21.120", "? ? ? D1 ? ? ? A9 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA"),
    ("H3 1.21.120", "? ? ? D1 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA"),
    ("H7 1.26.10", "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA"),
]

MAX_HITS = 6  # 与 mod 的 ResolveSignatureAll 一致:每个匹配点都试,最多 6 个


def parse(sig: str) -> list[int]:
    return [-1 if t == "?" else int(t, 16) for t in sig.split()]


def anchor_of(pat: list[int]) -> tuple[int, bytes]:
    """最长连续固定字节段 —— 用来把 O(n) 全量比对降成 C 层 find。"""
    best_off = best_len = 0
    i, n = 0, len(pat)
    while i < n:
        if pat[i] < 0:
            i += 1
            continue
        j = i
        while j < n and pat[j] >= 0:
            j += 1
        if j - i > best_len:
            best_off, best_len = i, j - i
        i = j
    return best_off, bytes(pat[best_off:best_off + best_len])


def scan(data: bytes, sig: str) -> list[int]:
    pat = parse(sig)
    aoff, anc = anchor_of(pat)
    plen, end = len(pat), len(data) - len(pat)
    hits: list[int] = []
    start = 0
    while len(hits) < MAX_HITS:
        j = data.find(anc, start, end + aoff + len(anc))
        if j < 0:
            break
        pos = j - aoff
        if pos >= 0 and pos <= end and all(
            b < 0 or data[pos + k] == b for k, b in enumerate(pat)
        ):
            hits.append(pos)
        start = j + 1
    return hits


def load(path: str, dump_to: str | None = None) -> tuple[bytes, str]:
    """返回 (so 内容, 描述)。APK 则取出 lib/arm64-v8a/libminecraftpe.so。"""
    if path.lower().endswith(".apk"):
        with zipfile.ZipFile(path) as z:
            cand = [n for n in z.namelist() if n.endswith("libminecraftpe.so")]
            if not cand:
                raise SystemExit(f"APK 里没有 libminecraftpe.so: {path}")
            name = sorted(cand, key=len)[0]
            with z.open(name) as f:
                data = f.read()
            if dump_to:
                with open(dump_to, "wb") as out:
                    out.write(data)
                print(f"extracted -> {dump_to} ({len(data):,} bytes)")
            return data, f"{os.path.basename(path)}::{name}"
    data = open(path, "rb").read()
    if dump_to:
        with open(dump_to, "wb") as out:
            out.write(data)
    return data, os.path.basename(path)


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    args = argv[1:]
    out_json = None
    dump_so = None
    if args and args[0] == "--json":
        out_json, args = args[1], args[2:]
    if args and args[0] == "--dump-so":
        dump_so, args = args[1], args[2:]

    report = {}
    for path in args:
        data, desc = load(path, dump_so)
        print(f"\n=== {desc}  ({len(data):,} bytes) ===")
        rows = []
        for name, sig in SIGS:
            hits = scan(data, sig)
            print(f"  {name:22} {'HIT %d' % len(hits) if hits else '-':>8}"
                  + (f"  {', '.join(hex(h) for h in hits)}" if hits else ""))
            rows.append({"sig": name, "hits": hits})
        report[desc] = rows

    if out_json:
        json.dump(report, open(out_json, "w", encoding="utf-8"), indent=1)
        print(f"\nwrote {out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
