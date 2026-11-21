<div align="center">

# 🎮 PSrdp

### Windows 桌面 · 直接跑在你的 PS5 上

<sub>English below · 中文在下面 ↓</sub>

![Resolution](https://img.shields.io/badge/1080P%20%20%202K%20%20%204K-真机验证-green)
![FPS](https://img.shields.io/badge/59.6fps_%401080p-实测-orange)
![输入](https://img.shields.io/badge/手柄_鼠标_键盘-同时支持-blue)
![License](https://img.shields.io/badge/License-GPL--3.0-lightgrey)

</div>

## 这是什么

把 PS5 变成 Windows 电脑的大屏显示器——游戏机上直接操作电脑桌面，
手柄当鼠标、声音从 PS5 出来。原生应用，不绕浏览器，不需要任何云服务。

> 🏠 纯本地局域网连接 · 不上传任何数据 · 免费开源

<details open>
<summary><b>功能一览</b></summary>

| | |
|---|---|
| **分辨率** | 任意数值随便填，1080P / 2K / 4K 真机验证通过（实测 59.6fps） |
| **操作** | 手柄（摇杆射准鼠标、L2/R2 左右键）· USB 鼠标 · 软键盘 |
| **声音** | PC 全部声音实时走 48kHz 立体声 |
| **界面** | 简中 · English · 日本語 · Français · Русский |
| **历史** | 记最近 5 组连接，开机秒连 |

</details>

## 三步连上

**准备**：能跑自制应用的 PS5 + [开启远程桌面](https://support.microsoft.com/zh-cn/windows)的 Windows 电脑（专业版）
 | 
[English setup](#setup-english)

<div align="center">

| ① | ② | ③ |
|:---:|:---:|:---:|
| 下载安装包 | 导入 PS5 | 启动连 PC |

</div>

1. 到 [ Releases](https://github.com/yecheng770/PS5-rdp-client/releases) 下载 `psrdp.ffpfs`（约 10 MB）
2. 用你常用的 loader（ShadowMount+ / etaHEN 等）导入，主页出现 **PSrdp**

> [下载安装包 <img src="https://img.shields.io/badge/→ 下载-Release-orange" />](https://github.com/yecheng770/PS5-rdp-client/releases)

### Setup (English)

1. Grab the latest `psrdp.ffpfs` from [Releases](https://github.com/yecheng770/PS5-rdp-client/releases)
2. Import with your loader (ShadowMount+, etaHEN, …) — **PSrdp** shows up on your home screen
3. Launch, enter your PC's IP, username and password **inside the app** (gamepad + on-screen keyboard,
   no browser involved), and hit Connect

Nothing is pre-configured — everything is entered by you on first launch.

## 适合干这些

📄 文档 · 💻 敲代码 · ⌨️ 跑终端 · 🌐 查资料 · 🖱️ 一切日常桌面操作

不适合：视频 / 游戏（位图传输的物理上限，不是 bug；后续 GFX 图形线版本解决）

<details>
<summary><b>规格参数</b></summary>

| | |
|---|---|
| 画面传输 | 位图全帧重写，1080P 实测 59.6 fps |
| 音频 | RDP 音频通道，48 kHz S16 立体声 |
| 协议 | FreeRDP 3.x（Windows 所有 RDP 服务端通用） |
| 运行环境 | PS5 自制系统（FW 13.42 验证） |
| 通信 | 纯局域网 RDP 3389，无云依赖 |

</details>

<details>
<summary><b>从源码构建 / Build from source</b></summary>

```bash
cd 源码
cp .env.example .env      # PS5_HOST 填你自己的
make app                  # 编译+签名+打包，产物在 dist/
```

首次构建自动下载 ps5-payload-sdk v0.42 并校验 SHA256。

</details>

## 相关仓库

- 🌐 **PSrdp GFX** —— 图形化视频流版本（开发中）

## 许可与致谢

GPL-3.0 · [`LICENSE`](LICENSE) · 第三方组件与许可详见
[`THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md)。

**与 Sony Interactive Entertainment 无关**——仅限已破解固件的 PS5 运行。

<div align="center">

[![Release](https://img.shields.io/github/v/release/yecheng770/psrdp?label=%E4%B8%8B%E8%BD%BD%E5%AE%89%E8%A3%85%E5%8C%85&color=orange)](https://github.com/yecheng770/PS5-rdp-client/releases)
[![Stars](https://img.shields.io/github/stars/yecheng770/psrdp?style=social)](https://github.com/yecheng770/PS5-rdp-client/stargazers)

**觉得有用请点个 Star ⭐，让更多 PS5 玩家看得到**

</div>
