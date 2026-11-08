add_rules("mode.release")
add_requires("nlohmann_json v3.11.3")
add_requires("dobby")

target("ForceCloseOreUI")
    set_kind("shared")
    add_files("src/**.cpp")
    add_includedirs("src")
    set_languages("c++20")
    set_strip("all")
    add_packages("nlohmann_json", "dobby")
    -- log: 本项目的日志
    -- z: mc_version.h 解压 APK 里的 AndroidManifest.xml（libz 在 NDK sysroot 里）
    add_syslinks("log", "z")
    -- ★ 保留帧指针：崩溃回溯要靠 x29 链还原调用栈，-O3 默认会把它省掉。
    --   只影响本库自身，游戏 so 是否保留不受我们控制（见 captureFrames
    --   的栈扫描兜底）。
    add_cxflags("-O3", "-fno-omit-frame-pointer")

    -- 16KB 页对齐。天玑 9400+ / Android 15+ 的设备页大小是 16384，
    -- 而 NDK r26 默认仍按 4KB 对齐产出 LOAD 段，加载器会因此拒绝该 so。
    -- 注意必须用 add_shflags：xmake 对 shared 目标走 clang++ 链接，
    -- 只有 shflags 会真正进到链接命令行，add_ldflags 会被丢掉。
    add_shflags("-Wl,-z,max-page-size=16384")
