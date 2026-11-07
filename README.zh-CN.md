<div align="center">

<h1 align="center">ForceCloseOreUI — 独立版</h1>

<p>强制关闭 Minecraft 基岩版的 OreUI，恢复经典 JSON-UI，<br>
并带一套能扛过游戏更新的自验证特征码扫描。</p>

<p>
<a href="./README.md">English</a> · <b>中文</b>
</p>

</div>

<br/>

> [!IMPORTANT]
> **这是独立版 `.so`，多数人其实用不到它。**
> 想省事请用一键打包 App：**<https://github.com/yinghuajimew/yhjmew.no.ore.ui>**
> 它会自动下载游戏、注入正确的 `.so`、重签名并安装，不需要电脑也不需要手动改包。
> 本仓库面向想要裸库、或想自己编译的人。

## 这是什么

一个 `arm64-v8a` 动态库，钩住 Minecraft 基岩版初始化 OreUI 配置的函数，把 `OreUIConfig`
里的每一项都强制设为 `false`，游戏于是回退到 JSON-UI。

它**不改** APK 里的 assets 或资源文件——完全在运行时挂钩，所以只要特征码能命中，
任何版本的 Minecraft 都能用。

## 运行要求

| 项 | 要求 |
|---|---|
| ABI | 仅 `arm64-v8a` |
| Android | 11+（API 30+） |
| 页大小 | 4 KB / 16 KB 都支持 |
| 注入方式 | 需要能往 Minecraft 里加载 `.so` 的手段 |

库按 **16 KB 页对齐**编译（`-Wl,-z,max-page-size=16384`），所以天玑 9400+ 这类
16 KB 页设备也能加载——那些设备会直接拒绝 4 KB 对齐的 `.so`。

## 怎么用

`.so` 需要被放进重新打包的 Minecraft APK 里的 `lib/arm64-v8a/libForceCloseOreUI.so`
（或者由其他加载器注入）。**只有依赖版**才需要额外带 `libpreloader.so`，
这个独立版不需要。

**推荐直接用一键打包 App** —— <https://github.com/yinghuajimew/yhjmew.no.ore.ui>
重打包、注入、签名一步到位。

**自己动手的话**：重打包 APK，把 `.so` 放进 `lib/arm64-v8a/`。
注意签名——换了签名就装不上原版，必须先把官方版卸掉，而卸载会连存档一起删。

配置与日志位置：

```
/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/ForceCloseOreUI/
├── config.json        # 要关掉哪些项，首次挂钩成功后生成
├── signatures.json    # 记下能用的特征码与 detour 变体（自动维护）
├── debug.log          # 会话日志
└── crash_native.log   # 原生崩溃现场，崩了才有
```

该目录不可写时会回退到 `/storage/emulated/0/games/com.mojang/ForceCloseOreUI/`。

## 这个版本改了什么

本分支基于上游，但**挂钩流程基本重写了一遍**。一句话概括：
**以前钩子还没等到被调用就被扔掉了。**

### 1. 修复：所有钩子都报「not called」然后被丢弃

旧流程装上钩子后只等 **2.5 秒**，目标函数没被调用就拆掉、换下一个。但 OreUI 配置
初始化函数往往只在玩家进入某个界面时才调用，那时距启动可能已经**十几秒**。
结果就是正确的钩子被销毁，日志里一片 `hook INVALID (not called)`。

现在拆成两段：

- 先等 **2.5 秒**，覆盖「很快就调用」的常见情况
- 再进入**观察期**，最长 **30 秒**，等到的是确认，不是删除

只有确实证明钩错了才会拆。

### 2. 修复：「地址错了」和「还没调用」被当成一回事

这是两个不同的问题，处理方式也不该一样。现在判定明确分开：

| 判定 | 含义 | 处理 |
|---|---|---|
| `Valid` | 配置表已填充且正常 | 保留，写入特征码缓存 |
| `Empty` | 已调用，但配置表还是空的 | 保留——配置会在后续调用时生效 |
| `Invalid` | 配置表明显畸形（`> 2000` 项） | 拆掉——地址错了 |
| `Pending` | 已装上，还没被调用 | 继续等 |

### 3. 扫描提速——锚点预筛

原实现每个偏移都要比对整条模式，通配符也照样走循环。在约 320 MB 代码段上跑 10 条特征码，
开销很大。现在先找出模式里**最长的一段连续固定字节**作为锚点，绝大多数位置
一次比较就被排除。

### 4. 扫描提前（但挂钩没有）

挂钩前有一段**固定 500 ms** 的延迟，这是必要的：装太早会和 Minecraft 自己的
`BootstrapConfig` 初始化抢时序，导致每隔一次启动就 `SIGABRT`。这段延迟必须保留。

但**扫描**只是读内存、不碰任何游戏状态，所以现在放在这段等待期间完成。
轮到挂钩时地址已经算好了。

### 5. 每个匹配点都试，不再命中第一个就放弃

通配符多的特征码可能匹配到多个函数。旧代码挂了第一个就认定失败，
如果恰好是个没人调用的同名序言，整条特征码就被浪费了。
现在每条最多试 6 个匹配点。

### 6. 总时间预算提高

180 秒 → 300 秒，给观察期留出空间，不至于挤掉后面的候选。

## 特征码覆盖

覆盖 Minecraft 基岩版 **1.21.90 ~ 1.26.60**。游戏更新后特征码可能失效，
请用上面的 App，或往 `signatures.json` 里补新特征码。

## 编译

```sh
# 需要 NDK r26b 或更新版本，以及 xmake
./build.sh
# -> build/android/arm64-v8a/release/libForceCloseOreUI.so
```

CI（`.github/workflows/build.yml`）每次推送都会产出同样的文件。

## 致谢

基于 QYCottage 及贡献者的原项目 [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI)。
