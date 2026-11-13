# ``libminecraftpe.so`` 特征码推导与校验

针对 Minecraft 基岩版 `libminecraftpe.so`，用**纯字节分析**（不需要 IDA、不需要反编译）
定位 OreUI 配置函数并生成特征码。

> 为什么不用 IDA：`libminecraftpe.so` 有 240~340 MB，走 IDA 全量反编译需要几 GB 内存、
> 数分钟到数小时；而找特征码只需要读 ELF 头、定位字符串、扫 `.text`，几十秒即可完成，
> GitHub Actions 标准 runner 完全够用。

---

## 一、特征码长什么样

```
? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? B9 ? ? ? D5 F9 03 03 2A
```

- `?` 表示通配任意字节，其余为固定字节
- **AArch64 是小端，opcode 落在最高字节**，所以约定是「低 3 字节抹成 `?`，最高字节保留」——
  `D1` 是 `sub sp,sp,#imm`、`A9` 是 `stp`、`91` 是 `add imm`、`B9` 是 `str`、`D5` 是 `mrs`
- 尾部保留 1~2 条 `mov`（`F9 03 03 2A` = `mov w25, w3`），这是**版本落款**：
  它把某个参数搬进 callee-saved 寄存器，不同版本搬的寄存器不同，正是区分版本的依据

---

## 二、生成算法（`derive_sig.py`）

### 第 1 步：定位函数 —— 靠字符串，不靠特征码

```
/oreui-marketplace/search   /oreui-marketplace/wishlist
/start-from-template        /create-new-world
/player-permissions         /send-invites
/gameplay/bedtime           /pre-start
```

这些 OreUI 路由串在 `.rodata` 里位置稳定、跨版本存在（但会增减，见第四节）。
在 `.text` 里找引用它们的 `ADRP+ADD` 指令对：

```
ADRP Xn, page          ; 算出 4KB 对齐的页地址
ADD  Xn, Xn, #off      ; 补齐低 12 位 → 地址 = (PC & ~0xFFF) + page + off
```

解出这个地址，等于目标字符串在哪，那里就是「代码里用到这个路由」的位置。

### 第 2 步：回溯函数头

从引用处往前扫，找最近的、真正像函数起点的位置。判据：

- 指令是 `sub sp,sp` 或 `stp`
- **前一条指令是 `ret` / `br` / 无条件 `b`**（函数结束标志）
- 第一条指令不能是 `ADRP`

> 最后一条很重要：`ADRP` 与 `sub sp,sp` 的编码在掩码下会混淆
> （`912EFC00` 是 `add x0,x0,#0xBBF`，被误判成 `sub sp,sp`），
> 早期版本漏了这个判据，导致选错函数。

### 第 3 步：取序言并按类别通配

从函数头往下收集 `sub sp,sp` / `stp` / `add imm` / `str` / `mrs` / `mov` 这几类指令，
遇到不认识的类别即停止。每类的可变位掩码：

| 类别 | 掩码 | 保留的最高字节 |
|---|---|---|
| `sub_sp` | `0x003FFFE0` | `D1` |
| `stp` | `0x003FFFE0` | `A9` |
| `add_imm` | `0x003FFFE0` | `91` |
| `str` | `0x003FFFE0` | `B9` / `F9` |
| `mrs` | `0x00FFFFFF` | `D5` |
| `mov_reg` | `0x00FFFFFF` | `2A` / `AA` |

`mov` 且源寄存器是 `x0`~`x7`（参数）时**整条保留固定**，即版本落款。

### 第 4 步：候选函数选择

引用同一个路由串的函数可能有几十个（每个界面一个注册函数）。按序言结构评分：

```
+10  第一条是 sub sp,sp
+ 4  ×  stp 的条数            （保存的寄存器越多 = 函数越大）
+ 3  含 add_imm
+ 3  含 mrs                   （栈保护金丝雀）
+ 6  含 mov_reg               （参数搬运 = 落款所在）
```

**不要按引用密度排序** —— 实测在 1.21.132.1 上，引用最多的反而是相邻的辅助小函数，
真正的目标函数序言更长。

---

## 三、交叉验证结果

用本算法对 1.26.62.1 推导，得到：

```
? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? B9 ? ? ? D5 F9 03 03 2A
```

与仓库源码里手工维护的 `1.26.60` 那条**逐字节完全一致**。这是算法正确性的直接证据。

同一条特征码在 1.26.62.1 上命中 `0x9AF937C`，命中处正好是函数入口
（前一条指令为 `ret`），函数体引用 `/oreui-marketplace/search`、`/send-invites`、
`/main-menu` 等路由 —— 确认是 OreUI 配置注册函数。

---

## 四、必须知道的限制

### 1. OreUI 是逐步扩张的，不是开关

从 1.20 开始，界面一点点从 JSON-UI 搬到 OreUI：

| 版本 | 状态 |
|---|---|
| 1.20.10.01 | 已有 HBUI/OreUI **运行时核心**，界面仍以 JSON-UI 为主 |
| 1.21.x | OreUI 界面逐个出现 |
| 1.26.x | 几乎所有界面都是 OreUI |

1.20.10.01 上特征码命中 `0x56DBB0C`（是函数入口），但该函数引用的是：

```
/hbui/gameplay.html   core:routing:not-found   core:gui:resize-hack
core:telemetry:firstContentfulPaint   core:exception   JsException
core:keyboard:dismissed   ScreenLoadFailed
```

—— 运行时核心，不是具体界面。也就是说 **1.20 和 1.26 的钩子目标不是同一个函数**，
不能直接复用。

### 2. 路由串会增减、反复

每个版本的路由串集合都不同，有些界面在某个版本消失、下个版本又回来。
所以固定特征码表必然随版本腐化，**必须靠本算法对新版本重新推导**。

### 3. 命中点不一定在函数入口

| 版本 | 命中地址 | 前一条指令 | 是否函数入口 |
|---|---|---|---|
| 1.20.10.01 | 0x56DBB0C | `ret` | ✅ |
| 1.21.0.03 | 0x5F85100 | `ret` | ✅ |
| 1.21.90.3 | 0x5EC9928 | `ret` | ✅ |
| 1.21.132.1 | 0x907E4BC | `bl` | ❌ 真实函数头在 0x907E3D4 |
| 1.26.0.2 | 0x913DF54 | `bl` | ❌ |
| 1.26.62.1 | 0x9AF937C | `ret` | ✅ |

在函数中间下钩子会覆盖被调用指令或破坏栈帧。**命中不等于可用**，
必须额外校验「前一条指令是 `ret`」。

---

## 五、用法

### 推导新版特征码

```bash
python3 tools/derive_sig.py libminecraftpe.so
python3 tools/derive_sig.py libminecraftpe.so --json out.json
```

输出包含：定位到的函数地址、序言指令列表、生成的特征码。

### 校验现有特征码能命中哪些版本

```bash
python3 tools/scan.py libminecraftpe.so            # 直接传 .so
python3 tools/scan.py "Minecraft PE x.xx.apk"      # 也可以直接传 APK,自动取出 .so
python3 tools/scan.py --json report.json *.so
```

### 从 APK 里取 .so

`scan.py` 会自动处理；单独提取用：

```bash
python3 tools/scan.py --dump-so libminecraftpe.so game.apk
```

---

## 六、接进 GitHub Actions

`libminecraftpe.so` 有 240~340 MB，不要跟 IDA 全量反编译混在一起跑。建议独立一个轻量
job：上传 `.so` → 跑 `derive_sig.py` → 输出特征码。整个流程几十秒，不需要安装 IDA、
不需要 patch、不需要虚拟桌面。

实测矩阵见 `tools/SIG_MATRIX.md`。
