# -*- coding: utf-8 -*-
"""从 libminecraftpe.so 自动推导 OreUI 配置函数的特征码 —— 不需要 IDA。

思路（三步）：
  1. 定位锚点字符串：OreUI 路由串（/oreui-marketplace/search 等）在 .rodata 里，
     位置稳定且跨版本存在。
  2. 找引用：在 .text 里找 ADRP+ADD 指令对，解出它们算出的地址，等于该字符串的
     那些位置，就是「代码里用到这个路由」的地方。
  3. 回溯函数头：从引用处往前扫，找到最近的「sub sp,sp / stp」序言起点，
     然后把序言指令按类别通配化（抹掉寄存器号与立即数，保留 opcode），
     而 mov xN, xM（M 是参数寄存器）这类「保存参数」的指令保持固定 ——
     它正是区分版本的落款。

用法:
    python derive_sig.py libminecraftpe.so
    python derive_sig.py libminecraftpe.so --json out.json
"""
from __future__ import annotations

import json
import struct
import sys

# 跨版本都存在的 OreUI 路由串（越多越稳，命中任一条即可定位）
ANCHOR_STRINGS = [
    b"/oreui-marketplace/search",
    b"/oreui-marketplace/wishlist",
    b"/start-from-template",
    b"/create-new-world",
    b"/player-permissions",
    b"/send-invites",
    b"/gameplay/bedtime",
    b"/pre-start",
]

MAX_PROLOGUE_INSNS = 16
MAX_HITS = 6  # 每个模式最多回报 6 个命中点（与 mod 的 ResolveSignatureAll 一致）


# --------------------------------------------------------------------------
# ELF
# --------------------------------------------------------------------------
class Elf:
    def __init__(self, path: str):
        self.f = open(path, "rb")
        self.f.seek(0)
        hdr = self.f.read(64)
        if hdr[:4] != b"\x7fELF":
            raise SystemExit("不是 ELF 文件")
        if hdr[4] != 2:
            raise SystemExit("只支持 ELF64")
        (e_shoff,) = struct.unpack_from("<Q", hdr, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", hdr, 0x3A)
        raw = []
        for i in range(e_shnum):
            self.f.seek(e_shoff + i * e_shentsize)
            b = self.f.read(e_shentsize)
            name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", b, 0)
            raw.append((name, typ, flags, addr, off, size))
        strtab = self.read(raw[e_shstrndx][4], raw[e_shstrndx][5])

        def nm(n):
            e = strtab.find(b"\0", n)
            return strtab[n:e].decode("latin-1")

        self.secs = []
        for name, typ, flags, addr, off, size in raw:
            self.secs.append({"name": nm(name), "addr": addr, "off": off, "size": size})

    def read(self, off: int, size: int) -> bytes:
        self.f.seek(off)
        return self.f.read(size)

    def section(self, name: str):
        for s in self.secs:
            if s["name"] == name and s["size"]:
                return s
        return None

    def va_to_off(self, va: int):
        for s in self.secs:
            if s["size"] and s["addr"] <= va < s["addr"] + s["size"]:
                return s["off"] + (va - s["addr"])
        return None


# --------------------------------------------------------------------------
# AArch64 解码
# --------------------------------------------------------------------------
def sext(v: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (v & (sign - 1)) - (v & sign)


def dec_adrp(w: int):
    """ADRP: 1 immlo(2) 10000 immhi(19) Rd(5)"""
    if (w & 0x9F000000) != 0x90000000:
        return None
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    rd = w & 0x1F
    return rd, sext((immhi << 2) | immlo, 21) << 12


def dec_add_imm(w: int):
    """ADD (immediate) sf=1: 100100010 sh imm12 Rn Rd"""
    if ((w >> 23) & 0x1FF) != 0x122:
        return None
    sh = (w >> 22) & 1
    imm12 = (w >> 10) & 0xFFF
    rn = (w >> 5) & 0x1F
    rd = w & 0x1F
    return rn, rd, imm12 << (12 if sh else 0)


def insn_class(w: int) -> str | None:
    """序言里我们认识的指令类别；返回 None 表示序言到此结束。

    注意 ORDER 有讲究：sub_sp / add_imm / adrp 的编码区间重叠，
    必须先判最具体的，否则会把 add 误判成 sub。
    """
    # 无条件跳转 / ret —— 序言不可能从这里开始
    if (w & 0xFC000000) == 0x14000000:
        return None
    # mov xN,xM / mov wN,wM (ORR Xd,XZR,Xm / ORR Wd,WZR,Wm)
    if (w & 0x7FE0FFE0) == 0x2A0003E0:
        return "mov_reg"
    # add sp,sp,#imm (无移位) —— sub sp,sp 的兄弟，序言里也常见
    if (w & 0xFF8003FF) == 0x910003FF:
        return "sub_sp"
    # sub sp,sp,#imm
    if (w & 0xFF8003FF) == 0xD10003FF:
        return "sub_sp"
    # add xN, xM, #imm (12 位立即数, 无移位)
    if (w & 0xFF800000) == 0x91000000:
        return "add_imm"
    # sub xN, xM, #imm
    if (w & 0xFF800000) == 0xD1000000:
        return "add_imm"
    # stp/ldp (含前索引 !)
    if (w & 0x3B000000) == 0x29000000:
        return "stp"
    # str/ldr (unsigned offset) / 前索引 / 后索引
    if (w & 0xFFC00000) == 0xF9000000 or (w & 0xFFC00000) == 0xB9000000:
        return "str"
    if (w & 0xFFC00000) == 0xF8000000 or (w & 0xFFC00000) == 0xB8000000:
        return "str"
    # mrs xN, <sysreg>
    if (w & 0xFFF00000) == 0xD5300000:
        return "mrs"
    # adrp —— 序言里也可能出现，但用不着单独归类，算「其他」继续往下走
    if (w & 0x9F000000) == 0x90000000:
        return "misc"
    return None


def mov_src_is_arg(w: int) -> bool:
    """mov xN, xM / mov wN, wM 且 M 在 x0..x7 —— 典型的「把参数存起来」。

    64 位编码 ORR Xd, XZR, Xm = 0xAA0003E0
    32 位编码 ORR Wd, WZR, Wm = 0x2A0003E0
    """
    if (w & 0xFFE0FFE0) not in (0xAA0003E0, 0x2A0003E0):
        return False
    return ((w >> 16) & 0x1F) <= 7


def wildcard(w: int, cls: str) -> bytes | None:
    """按类别返回 4 字节掩码形态：返回 None 表示整条保留固定。"""
    return b"\x00\x00\x00\x00"   # 占位，真实掩码在 build_pattern 里算


# --------------------------------------------------------------------------
# 定位
# --------------------------------------------------------------------------
def find_string_vaddrs(elf: Elf):
    """在 .rodata 区内查找锚点字符串的虚拟地址。"""
    out = []
    for s in elf.secs:
        if s["size"] < 16:
            continue
        data = elf.read(s["off"], s["size"])
        for pat in ANCHOR_STRINGS:
            idx = data.find(pat)
            while idx >= 0:
                out.append((pat.decode(), s["addr"] + idx))
                idx = data.find(pat, idx + 1)
    return out


def find_code_refs(text_off: int, text_va: int, blob: bytes, target_va: int):
    """找算出 target_va 的 ADRP+ADD 指令对，返回 ADD 指令所在的 VA 列表。"""
    refs = []
    n = len(blob) // 4
    words = struct.unpack_from(f"<{n}I", blob, 0)
    pending = {}   # rd -> adrp 算出的页地址 VA
    for i, w in enumerate(words):
        va = text_va + i * 4
        a = dec_adrp(w)
        if a:
            rd, imm = a
            pending[rd] = ((va & ~0xFFF) + imm) & 0xFFFFFFFFFFFF
            continue
        d = dec_add_imm(w)
        if d:
            rn, rd, imm = d
            if rn in pending and rn == rd:
                if pending[rn] + imm == target_va:
                    refs.append(va)
            continue
        # 其他指令不影响 adrp 缓存（跨基本块会失效，但我们的引用在邻近位置）
    return refs


def is_terminator(w: int) -> bool:
    """ret / br xN / 无条件 b —— 函数结束的标志。"""
    if w == 0xD65F03C0:                 # ret
        return True
    if (w & 0xFFFFFC1F) == 0xD65F0000:  # ret xN / br xN / blr xN
        return True
    if (w & 0xFC000000) == 0x14000000:  # b .+imm (无条件跳转)
        return True
    return False


def looks_like_func_start(words, i: int) -> bool:
    """判定 words[i] 是否真的是函数起点。

    前面必须是 ret/br/b 或者是文件头；且第一条指令不能是 ADRP
    （ADRP 与 sub sp,sp 的编码在掩码下会混淆，必须单独排除）。
    """
    if i <= 0:
        return True
    w = words[i]
    if (w & 0x9F000000) == 0x90000000:   # ADRP
        return False
    prev = words[i - 1]
    return is_terminator(prev)


def prologue_score(pro):
    """给序言打分：目标函数的形态是 sub sp,sp → 若干 stp → add（或 str）→ mrs → mov。

    引用密度不可靠 —— 1.21.132.1 上引用最多的小函数其实是相邻的辅助函数，
    真正被特征码盯上的是「序言更长」的那个。
    """
    if not pro:
        return -1
    classes = [c for _, _, c in pro]
    score = 0
    if classes[0] == "sub_sp":
        score += 10          # 一定从 sub sp,sp 开头
    score += 4 * classes.count("stp")       # 保存大量寄存器 = 巨型函数
    if "add_imm" in classes:
        score += 3
    if "mrs" in classes:
        score += 3
    if classes[-1] == "mov_reg" or "mov_reg" in classes:
        score += 6           # 尾部把参数搬进 callee-saved 寄存器
    return score


def find_func_start(text_va: int, blob: bytes, ref_va: int, lookback: int = 0x8000):
    """从引用处向前找最近的、真正像函数起点的位置。"""
    n = len(blob) // 4
    words = struct.unpack_from(f"<{n}I", blob, 0)
    lo = max(0, (max(text_va, ref_va - lookback) - text_va) // 4)
    hi = (ref_va - text_va) // 4
    for i in range(hi, lo - 1, -1):
        c = insn_class(words[i])
        if c not in ("sub_sp", "stp"):
            continue
        if looks_like_func_start(words, i):
            return text_va + i * 4
    return None


def collect_prologue(blob: bytes, text_va: int, func_va: int, max_insn: int = MAX_PROLOGUE_INSNS):
    """收集序言指令，返回 [(va, word, cls)]；遇到不认识的类别即停止。"""
    off = func_va - text_va
    out = []
    for k in range(max_insn):
        w = struct.unpack_from("<I", blob, off + k * 4)[0]
        c = insn_class(w)
        if c is None:
            break
        out.append((func_va + k * 4, w, c))
    return out


# --------------------------------------------------------------------------
# 特征码生成
# --------------------------------------------------------------------------
# 每类的「可变位」掩码：1 = 抹成通配符。
# AArch64 是小端，opcode 落在最高字节，所以约定是「低 3 字节通配、最高字节保留」——
# 这正是 mod 里 `? ? ? D1` 这种写法的由来。
CLASS_MASK = {
    # sub sp, sp, #imm : imm12(10-21) Rn(5-9) Rd(0-4) 可变 → 保留 0xD1
    "sub_sp": 0x003FFFE0,
    # stp/ldp : imm7(15-21) Rt2(10-14) Rn(5-9) Rt(0-4) 可变 → 保留 0xA9
    "stp": 0x003FFFE0,
    # add imm : imm12 Rn Rd 可变 → 保留 0x91
    "add_imm": 0x003FFFE0,
    # str/ldr unsigned offset : imm12 Rn Rt 可变 → 保留 0xB9 / 0xF9
    "str": 0x003FFFE0,
    # mrs : CRm/op2 可变 → 保留 0xD5
    "mrs": 0x00FFFFFF,
    # mov wN, wM : 两个寄存器号都可变 → 保留 0x2A
    "mov_reg": 0x00FFFFFF,
    # adrp : immlo Rd immhi 可变 → 保留 0x90
    "misc": 0x00FFFFFF,
}


def build_pattern(prologue):
    """生成 '? ? ? D1 ...' 形式；mov 且源为参数寄存器的整条保留（版本落款）。"""
    toks = []
    for va, w, cls in prologue:
        if cls == "mov_reg" and mov_src_is_arg(w):
            toks.extend(f"{b:02X}" for b in w.to_bytes(4, "little"))
            continue
        mask = CLASS_MASK.get(cls, 0x00FFFFFF)
        raw = w.to_bytes(4, "little")
        for byte_i in range(4):
            if (mask >> (byte_i * 8)) & 0xFF:
                toks.append("?")
            else:
                toks.append(f"{raw[byte_i]:02X}")
    return " ".join(toks)


def anchor_of(pat: list[int]) -> tuple[int, bytes]:
    """最长连续固定字节段 —— 用来把逐字节比对降成 C 层 find。"""
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


def parse_pattern(sig: str) -> list[int]:
    """'? ? 3F D1' -> [-1, -1, 0x3F, 0xD1]"""
    return [-1 if t == "?" else int(t, 16) for t in sig.split()]


def self_verify(blob: bytes, text_va: int, pat_str: str) -> list[int]:
    """把刚生成的特征码在 .text 上重新扫一遍，返回命中的 VA 列表。

    「命中」不等于「可用」：必须确认命中处之前是 ret/br/b，才说明它落在
    函数入口。在函数中间下钩子会覆盖被调用指令或破坏栈帧。
    """
    pat = parse_pattern(pat_str)
    aoff, anc = anchor_of(pat)
    plen = len(pat)
    hits = []
    start = 0
    end = len(blob) - plen
    while len(hits) < MAX_HITS:
        j = blob.find(anc, start, end + aoff + len(anc))
        if j < 0:
            break
        pos = j - aoff
        if pos >= 0 and pos <= end and all(
            b < 0 or blob[pos + k] == b for k, b in enumerate(pat)
        ):
            hits.append(text_va + pos)
        start = j + 1
    return hits


def is_func_entry(blob: bytes, text_va: int, va: int) -> bool:
    """命中处是否落在函数入口（前一条是 ret/br/b，或就在 .text 开头）。"""
    off = va - text_va
    if off <= 0:
        return True
    if off % 4:
        return False
    prev = struct.unpack_from("<I", blob, off - 4)[0]
    return is_terminator(prev)


def main(argv):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    path = argv[1]
    elf = Elf(path)
    text = elf.section(".text") or elf.section(".text")
    if not text:
        raise SystemExit("找不到 .text")
    blob = elf.read(text["off"], text["size"])
    print(f"file   : {path}")
    print(f".text  : va=0x{text['addr']:X} off=0x{text['off']:X} size=0x{text['size']:X}")

    strings = find_string_vaddrs(elf)
    print(f"anchors: 找到 {len(strings)} 处锚点字符串")
    seen = {}
    for name, va in strings:
        seen.setdefault(name, []).append(va)

    all_refs = []
    for name, vas in seen.items():
        for va in vas[:1]:
            refs = find_code_refs(text["off"], text["addr"], blob, va)
            if refs:
                print(f"  {name:32} va=0x{va:<10X} refs={len(refs)}")
                all_refs.extend(refs)

    if not all_refs:
        print("[-] 没有任何路由字符串被代码引用，失败")
        return 1

    # 聚到函数起点
    starts = {}
    for r in all_refs:
        fs = find_func_start(text["addr"], blob, r)
        if fs:
            starts[fs] = starts.get(fs, 0) + 1
    if not starts:
        print("[-] 无法回溯到任何函数起点")
        return 1

    scored = []
    for va, cnt in starts.items():
        pro = collect_prologue(blob, text["addr"], va)
        scored.append((prologue_score(pro), cnt, va, pro))
    scored.sort(key=lambda t: (-t[0], -t[1]))

    print(f"\n候选函数（按序言结构评分）：")
    for score, cnt, va, pro in scored[:6]:
        print(f"  func 0x{va:X}  score={score:3}  refs={cnt}  prologue={len(pro)} insn")

    best_score, best_cnt, best_va, pro = scored[0]
    pat = build_pattern(pro)
    print(f"\n选中函数 0x{best_va:X}（{best_cnt} 条引用）")
    print(f"序言 {len(pro)} 条指令 / {len(pro)*4} 字节：")
    for va, w, cls in pro:
        print(f"  0x{va:X}  {w:08X}  {cls}")
    print(f"\n生成特征码：\n{pat}\n")

    # ---- 自验：把刚生成的特征码回扫，确认唯一命中且落在函数入口 ----
    hits = self_verify(blob, text["addr"], pat)
    entry_ok = [h for h in hits if is_func_entry(blob, text["addr"], h)]
    print(f"自验：命中 {len(hits)} 处 -> {[hex(h) for h in hits] or '无'}")
    print(f"      位于函数入口：{len(entry_ok)} 处 {[hex(h) for h in entry_ok]}")
    if len(hits) == 1 and len(entry_ok) == 1:
        print("      ✅ 唯一命中且位于函数入口，可直接使用")
    elif len(entry_ok) >= 1:
        print("      ⚠️ 有多个命中，其中包含函数入口，可用")
    elif hits:
        print("      ❌ 命中处均不在函数入口，请人工复核")
    else:
        print("      ❌ 连自己都扫不到，模式生成有误")
    print()

    if "--json" in argv:
        out = argv[argv.index("--json") + 1]
        json.dump(
            {"file": path, "func_va": best_va, "refs": best_cnt,
             "prologue_insns": [{"va": hex(v), "word": f"{w:08X}", "cls": c} for v, w, c in pro],
             "signature": pat},
            open(out, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
