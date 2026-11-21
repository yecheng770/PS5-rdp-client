# 第三方引用清单（THIRD-PARTY NOTICES）

本项目构建与运行依赖以下开源组件，均保留原许可并在此致谢。

## 依赖与许可

| 组件 | 版本 | 许可 | 用途 | 凭证位置 |
|---|---|---|---|---|
| FreeRDP（含 WinPR） | 3.33.1（静态库） | Apache-2.0 | RDP 协议客户端核心 | `assets/licenses/FreeRDP-Apache-2.0.txt`；`vendor/lib/libfreerdp3.a`、`libwinpr3.a` |
| OpenSSL | 1.1.1w（静态库） | OpenSSL 与 Apache-2.0 双许可 | TLS 加密连接 | `vendor/lib/libssl.a`、`libcrypto.a` |
| zlib | 1.3.2（源码构建） | zlib 许可（自由使用） | 数据压缩 | 构建时从 `.deps/native/zlib/` 张贴原码哈希构建；产物 `vendor/lib/libz.a` |
| ps5-payload-sdk | v0.42 | GPL-3.0+（除 include/freebsd 目录为 BSD） | 目标机系统调用头与链接工具 | 工具链 URL 与 SHA256 钉在 `tools/setup-native-dependencies.sh`；引自 github.com/ps5-payload-dev/sdk |
| LLVM clang-18 / libc++ / libunwind | SDK 内 bundled | Apache-2.0（LLVM 例外条款） | 编译器与 C++ 运行时（其中 libunwind 解包后去 .deplibs 重打包为 `libunwind_noext.a`） | 构建于 ps5-payload-sdk 内 |
| FreeBSD libc / pthread | SDK sysroot 内 | BSD 系 | 目标机 C 运行时基座 | `.deps/.../target/lib/` |
| ps5-native-app-boilerplate | 构建脚本上游 | GPL-3.0-or-later（Copyright 2026 BlackBearReloaded） | `tools/build.sh`、签名/链接工具框架 | 各脚本文件头有原始版权标注 |
| ps5-homebrew-starter | 工程布局上游 | GPL-3.0 | 目录结构与构建范式 | github.com/BlackBearReloaded/ps5-homebrew-starter |
| stb_truetype.h | v1.26 | Public Domain（Sean Barrett） | TrueType 字体解析渲染 | `src/stb_truetype.h` 文件头自带声明 |
| MkPFS | 1.1.0 | 视上游仓库（PSBrew/MkPFS） | FFPS 镜像打包工具 | 打包环节使用 |

## 内置素材来源

- **手柄图标（DualSense SVG）**：取自 [SVG Repo](https://www.svgrepo.com/)，原文件头部保留上传出处注释；`assets/dual-sense/render_dualsense.py` 在本地烘焙成位图头文件。
- **地球装饰（globe）**：`assets/globe/` 下的线描 SVG（24×24、stroke 1.6 的通用图标样式），`render_globe.py` 把两条椭圆+赤道烘焙成 alpha mask 头文件。
- **CJK 字形片段**：源自 OFL-1.1 授权的开源中文字体（见 `assets/licenses/CJK-OFL-1.1.txt`），随后生成点阵头文件 `psrdp_cjk_hex.h`、`psrdp_font_hex.h`。

## 许可分层说明

本项目分两类许可：

1. **GPL-3.0-or-later**（上游 starter kit / boilerplate / ps5-payload-sdk 产物）——这些目录按要求继续开源：
   - `tooling/`（ps5-native-tool 与构建脚本框架，原版权 BlackBearReloaded）
   - `tools/` 下源自 boilerplate 的构建脚本（各文件头部有版权行）
2. **应用代码**（`src/` 下的 RDP 客户端实现、`sce_sys/`、`assets/` 的自制内容）按仓库根目录 LICENSE 所述许可发布。

静态中间产物（`.a` 静态库、`eboot.bin`、FFPFS 镜像）由上述两套代码在各自许可下生成，再发行 FFPFS 包时请按 GPL 义务附带对应源码（或指回本仓库）。

本项目与 Sony Interactive Entertainment 无关联。仅在已破解（payload loader / HEN 类环境）的 PS5 上测试通过，正版固件下不运行。
