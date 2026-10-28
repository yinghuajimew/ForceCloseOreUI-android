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
    add_syslinks("log")
    add_cxflags("-O3")

    -- 16KB 页对齐。天玑 9400+ / Android 15+ 的设备页大小是 16384，
    -- 而 NDK r26 默认仍按 4KB 对齐产出 LOAD 段，加载器会因此拒绝该 so。
    -- 注意必须用 add_shflags：xmake 对 shared 目标走 clang++ 链接，
    -- 只有 shflags 会真正进到链接命令行，add_ldflags 会被丢掉。
    add_shflags("-Wl,-z,max-page-size=16384")
