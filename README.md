<div align="center">

<h1 align="center">ForceCloseOreUI — Standalone</h1>

<p>Force Minecraft Bedrock to drop OreUI and go back to the classic JSON-UI,<br>
with a self-validating signature scanner that survives game updates.</p>

<p>
<b>English</b> · <a href="./README.zh-CN.md">中文</a>
</p>

</div>

<br/>

> [!IMPORTANT]
> **This is the standalone `.so`. You probably don't need it.**
> The easiest way to use this project is the one-tap app:
> **<https://github.com/yinghuajimew/yhjmew.no.ore.ui>** — it downloads Minecraft,
> injects the correct `.so`, re-signs and installs. No PC, no manual patching.
> This repo is for people who want the raw library, or want to build it themselves.

## What this is

An `arm64-v8a` shared library that hooks Minecraft Bedrock's OreUI config initializer and
forces every entry in `OreUIConfig` to `false`, which makes the game fall back to JSON-UI.

It does **not** modify the APK's assets or resources — it hooks at runtime, so it works on
any Minecraft version whose function signature it can find.

## Requirements

| Item | Requirement |
|---|---|
| ABI | `arm64-v8a` only |
| Android | 11+ (API 30+) |
| Page size | 4 KB and 16 KB both supported |
| Injection | Needs a way to load a `.so` into Minecraft |

The library is built with **16 KB page alignment** (`-Wl,-z,max-page-size=16384`), so it also
loads on devices with 16 KB pages such as Dimensity 9400+ — those devices reject a 4 KB-aligned
`.so` outright.

## How to use it

The `.so` must end up at `lib/arm64-v8a/libForceCloseOreUI.so` inside a repackaged Minecraft APK
(or be injected by a mod loader). You also need `libpreloader.so` **only for the dependency
variant** — this standalone build has no such requirement.

**Recommended: use the one-tap app instead** — <https://github.com/yinghuajimew/yhjmew.no.ore.ui>
It handles repackaging, injection and signing in one step.

**Rolling your own:** repackage the APK, add the `.so` under `lib/arm64-v8a/`,
keep the original signature requirement in mind (you will need to re-sign, and a different
signature means you must uninstall the official build first — which deletes your world data).

Config and logs land in:

```
/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/ForceCloseOreUI/
├── config.json        # what to disable; created on first successful hook
├── signatures.json    # the signature + detour variant that worked (auto-managed)
├── debug.log          # session log
└── crash_native.log   # native crash dump, if one happens
```

If that path is not writable it falls back to `/storage/emulated/0/games/com.mojang/ForceCloseOreUI/`.

## What changed in this build

This fork is based on the upstream project but the hook installation was substantially reworked.
The short version: **hooks were being thrown away before they had a chance to fire.**

### 1. Fixed: every hook reported "not called" and was discarded

The old flow installed a hook, waited **2.5 seconds**, and if the target had not been called yet it
tore the hook down and moved on. But the OreUI config initializer is often only invoked when the
player enters a particular screen — which can be **10+ seconds** after launch. Valid hooks were
being destroyed, and the log showed a wall of `hook INVALID (not called)`.

Now the wait is split in two:

- a short **2.5 s** window for the common case (called right away)
- then an **observation window** of up to **30 s**, confirming the hook instead of deleting it

A hook is only removed once it is genuinely proven wrong.

### 2. Fixed: wrong address vs. not-yet-called were treated the same

Those are different problems and need different responses. The verdict is now explicit:

| Verdict | Meaning | Action |
|---|---|---|
| `Valid` | Config map populated and sane | Keep, cache the signature |
| `Empty` | Called, but the map was still empty | Keep — config applies on a later call |
| `Invalid` | Map is clearly malformed (`> 2000` entries) | Destroy — wrong address |
| `Pending` | Installed, not called yet | Keep waiting |

### 3. Faster scan — anchor prefilter

The pattern engine compared every token at every offset, including wildcards. On a ~320 MB code
region with 10 signatures that is a lot of work. It now finds the **longest run of fixed bytes** in
the pattern and uses it as an anchor, so the vast majority of positions are rejected after a
single byte comparison.

### 4. Scan happens earlier (but hooking does not)

There is a deliberate **500 ms** delay before hooking: installing too early races Minecraft's own
`BootstrapConfig` initialization and caused a `SIGABRT` on every other launch. That delay is
required and stays.

But *scanning* only reads memory and touches no game state, so it now runs **during** that wait.
The addresses are already resolved by the time hooking is allowed to start.

### 5. Every match point is tried, not just the first

A signature with many wildcards can match several functions. The old code hooked the first match and,
if that was some unrelated prologue that never gets called, gave up on the entire signature.
All match points (up to 6 per signature) are now tried in order.

### 6. Total time budget raised

180 s → 300 s, so the observation windows fit without starving later candidates.

## Signature coverage

Covers Minecraft Bedrock **1.21.90 ~ 1.26.60**. After a game update the signature may stop matching;
use the app above, or add a new signature to `signatures.json`.

## Building

```sh
# needs NDK r26b (or newer), xmake
./build.sh
# -> build/android/arm64-v8a/release/libForceCloseOreUI.so
```

CI (`.github/workflows/build.yml`) produces the same artifact on every push.

## Credits

Based on the original [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI) by QYCottage
and contributors.
