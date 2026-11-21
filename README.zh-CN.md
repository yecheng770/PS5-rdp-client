<div align="center">

# 🎮 PSrdp — PS5 远程桌面客户端

### 把 PS5 变成 Windows 电脑的大屏幕

[English](README.md) · **简体中文** · [日本語](README.ja.md)

![分辨率](https://img.shields.io/badge/1080P%20%7C%202K%20%7C%204K-真机验证-green)
![帧率](https://img.shields.io/badge/59.6fps_%401080p-实测-orange)
![许可](https://img.shields.io/badge/License-GPL--3.0-lightgrey)

**[⬇️ 前往 Releases 下载](https://github.com/yecheng770/PS5-rdp-client/releases)**

</div>

## 这是什么

PSrdp 让你的 PS5 变成 Windows 电脑的大屏显示器：坐在沙发上用手柄操作电脑桌面，
电脑的声音从 PS5 出来。**PS5 原生应用**——不绕浏览器、不经过任何云服务、不是模拟器。

> 🏠 局域网或远程域名均可连接 · 不上传任何数据 · 免费开源

## 功能

| | |
|---|---|
| **分辨率** | 任意数值随便填——**1080P / 2K / 4K 真机逐档验证**（1080p 实测 59.6 fps） |
| **操作** | 手柄（左摇杆=指针，L2/R2=左右键）· USB 鼠标 · 内置软键盘 |
| **声音** | PC 的一切声音实时以 48kHz 立体声从 PS5 播放 |
| **界面** | 简体中文 · English · 日本語 · Français · Русский |
| **历史** | 记住最近 5 组连接，下次直接连 |

## 适合这些场景

📄 文档 · 💻 敲代码 · ⌨️ 跑终端 · 🌐 查资料 · 🖱️ 一切日常桌面操作

不适合视频播放 / 游戏——这是位图传输方案的物理上限，不是 bug
（后续 GFX 图形线版本会解决）。

## 安装（三步）

**准备**：能跑自制应用的 PS5 + 一台[开启远程桌面](https://support.microsoft.com/zh-cn/windows)的
Windows 电脑（专业版）。

1. 从 [Releases](https://github.com/yecheng770/PS5-rdp-client/releases) 下载最新的 `psrdp.ffpfs`（约 10 MB）
2. 用你常用的 loader（ShadowMount+ / etaHEN 等）导入，主页出现 **PSrdp**
3. 启动应用，**在应用内**填电脑的 IP（或域名）、账号、密码——手柄+软键盘就够了，
   全程不需要浏览器

应用不预置任何信息，所有内容首次启动自己填。

<details>
<summary><b>规格参数 / 从源码构建</b></summary>

| | |
|---|---|
| 画面传输 | 位图全帧重写，1080p 实测 59.6 fps |
| 音频 | RDP 音频通道，48kHz S16 立体声 |
| 协议 | FreeRDP 3.x（兼容所有 Windows RDP 服务端） |
| 运行环境 | PS5 自制系统（FW 13.42 验证） |
| 网络 | RDP 默认 3389（端口可改），局域网或远程域名均可，无云依赖 |

### 从源码构建

需要一台 Linux 机器（Ubuntu 22.04+）：

```bash
cd 源码
cp .env.example .env      # PS5_HOST 填你自己的
make app                  # 编译+签名+打包，产物在 dist/
```

首次构建自动下载官方 ps5-payload-sdk v0.42 并校验 SHA256。

</details>

## 相关仓库

- 🌐 **PSrdp GFX** —— 硬件加速图形线，解决流畅视频与游戏（开发中）

## 许可

GPL-3.0 · [`LICENSE`](LICENSE) · 第三方组件详见
[`THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md)

**与 Sony Interactive Entertainment 无关**——仅限已破解固件的 PS5 运行。

<div align="center">

[![Release](https://img.shields.io/github/v/release/yecheng770/PS5-rdp-client?label=%F0%9F%93%A6%20%E4%B8%8B%E8%BD%BD&color=orange)](https://github.com/yecheng770/PS5-rdp-client/releases)
[![Stars](https://img.shields.io/github/stars/yecheng770/PS5-rdp-client?style=social)](https://github.com/yecheng770/PS5-rdp-client/stargazers)

**觉得有用请点一个 Star ⭐，让更多 PS5 玩家看到**

</div>
