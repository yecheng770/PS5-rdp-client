PSrdp — PS5 原生 RDP 远程桌面客户端
==================================

在 PS5 上直接运行的 FreeRDP 远程桌面客户端（homebrew）。
局域网连 Windows 的 RDP（远程桌面），画面直接写到 PS5 视频输出，
手柄/鼠标/触控板操作，内置 Web 配置页。


安装包
------

仓库根目录提供现成的 PS5 安装包，不想自己编译的可以直用：

| 文件 | 说明 |
|---|---|
| `PPSA90210-v1.7.23-opensource.ffpfs` | **对外发布版**（私有信息已全部脱敏，默认端口 3389，见下） |
| —— | 真机长期验收版源码 tag 为 `v1.7.22`（见下方"验收状态"） |

### 从 1.7.22 到 1.7.23 变了什么

1. **版本号显示修正**：1.7.22 源码里 `PSRDP_VERSION` 宏漏写，页面显示仍是 1.7.20，本版修正
2. **默认 RDP 端口修正**：默认端口从 33890 改回标准 RDP 端口 **3389**，符合绝大多数 Windows 用户
3. **预置连接信息脱敏**：源码里预填的默认主机/用户名/密码全部清空，新装用户在 Web 配置页填自己的即可

沙盒与运行语义同 1.7.22，无新功能。


验收状态（2026-11-20，FW 13.42）
--------------------------------

**v1.7.22** 已在 PS5 上通过长期真机使用验收：桌面+音频+输入全部通过。
**v1.7.23** 是同一份代码的发布整理版（上面三条小修），**未上真机回归**，
但代码改动只有上面三处且彼此独立，风险极低；介意的话请用 `git checkout v1.7.22`
自行编译验收过的版本。

已知原理性限制：**位图渲染方案无法流畅播放视频**（每帧全屏位图重画的
物理原理决定，动态视频内容必然卡顿）。治本方案在独立的图形线 PSrdp GFX
（v1.8+），不在本仓库范围。


本仓库内容
----------

```
源码/                              完整可复现源码树
    ├── src/                       应用主源码（渲染、输入、音频、UI）
    │   ├── rdpdr/                 RDP 设备重定向
    │   └── rdpsnd/                RDP 音频重定向
    ├── sce_sys/                   应用元数据 param.json + 图标 icon0.png
    ├── vendor/                    FreeRDP 3.33.1 / OpenSSL 1.1.1w / zlib 预编译静态库
    ├── tooling/                   构建与签名工具链（ps5-native-tool 等）
    │   └── link-stubs/            libSceMouse 链接 stub（上游 SDK 缺失件）
    ├── tools/                     构建入口、部署、依赖下载脚本
    ├── runtime/                   clean-room libc 运行时（构建时再生，哈希校验）
    ├── assets/                    图标、字体、装饰素材及其许可
    ├── Makefile                   顶层构建入口
    ├── .env.example               构建参数模板（无真实信息）
    ├── THIRD-PARTY-NOTICES.md     第三方依赖与素材清单
    └── .gitignore


如何从源码构建
--------------

需要一台 Linux 编译机（x86_64，Ubuntu 22.04+ 或类似发行版），首次构建会自动下载
官方 ps5-payload-sdk v0.42（SHA256 校验）到 `.deps/`，不需要手动装。

```bash
cd 源码
cp .env.example .env         # 按需改 PS5_HOST / FTP_PORT / DEPLOY_DRY_RUN
make app                     # 编译、链接、签名、打包，产物出在 dist/
```

要求：
- clang-18（或 SDK 自带 prospero-clang）
- python3、sha256sum、wget、unzip、ninja、make
- mkpfs 1.1.0+（pip install mkpfs）

注意事项：
- 构建脚本会对 SDK、zlib、运行时 libc 全部做哈希校验，不匹配会拒绝构建
- 鼠标输入必须走 `APP_IMPORT_STUBS=tooling/link-stubs/libSceMouse.so`（在 .env 或 .env.example 里已给）
- `.env` 里的变量会透传给构建（见 Makefile 第 10 行 `-include .env`），改完直接构建即可


如何安装到 PS5
--------------

需先在 PS5 上有加载器（例：ShadowMount+/etaHEN 自制应用库，自行选择）。把生成的
`dist/PPSA90210*.ffpfs` 或本仓库根目录的预编译包上传到加载器识别的目录，
按加载器说明操作即可。

你需要在 PS5 上自己完成：
- 关闭正在运行的 PSrdp（如果开着）
- 让加载器重扫包目录（新包覆盖旧包）
- 启动应用验证


端口约定
--------

- 应用运行时会在本机起一个 Web 配置页（默认 8081 调参），详见源码 main.c
- 连接 Windows 的 RDP 用目标主机自己的端口（默认 **3389**，可在配置页改）
- 对构建/部署工具链而言，PS5 的 FTP 装载口为 **2121**（ps5-payload loader 标准）


许可
----

本项目按 **GPL-3.0** 许可开源（根目录 LICENSE 为原版 GPL-3.0 全文）。
为什么是 GPL 而不是更宽松的许可：构建工具链和链接产物来自
ps5-payload-sdk、ps5-homebrew-starter、ps5-native-app-boilerplate 这几个
GPL-3.0 上游项目，成品里静态链接了它们，按规则衍生工作只能继续
以 GPL-3.0 发布。借来的项目清单详见 `源码/THIRD-PARTY-NOTICES.md`。

本项目与 Sony Interactive Entertainment 无关。仅在已破解（自定义固件）
的 PS5 上测试通过，在正版固件上不运行，也不要尝试在正版固件上运行。


历史
----

项目由 v0.2.1 开始，历经 32+ commit 演进到 v1.7.22 长期真机验收收口，
1.7.23 为开源发布的整理版（脱敏+端口修正+版本号修正）。
图形线 PSrdp GFX（v1.8+）另立仓库/分支，不在本仓库范围。
