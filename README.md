<div align="center">

# 🎮 PSrdp — PS5 RDP Client

### Your PS5 becomes the big screen for your Windows PC

**English** · [简体中文](README.zh-CN.md) · [日本語](README.ja.md)

![Resolution](https://img.shields.io/badge/1080P%20%7C%202K%20%7C%204K-real--hardware--tested-green)
![FPS](https://img.shields.io/badge/59.6fps_%401080p-measured-orange)
![License](https://img.shields.io/badge/License-GPL--3.0-lightgrey)

**[⬇️ Download from Releases](https://github.com/yecheng770/PS5-rdp-client/releases)**

</div>

## What is this

PSrdp turns your PS5 into a large display for your Windows PC: operate the
desktop from the couch with a gamepad, hear PC audio through the console.
It's a **native PS5 application** — no browser involved, no cloud in the
middle, no emulator.

> 🏠 Connects to LAN or via a remote hostname · nothing uploaded anywhere · free & open source

## Features

| | |
|---|---|
| **Resolution** | Any value you like — **1080P / 2K / 4K verified on real hardware** (59.6 fps @ 1080p measured) |
| **Input** | Gamepad (left stick = pointer, L2/R2 = click) · USB mouse · on-screen keyboard |
| **Audio** | Everything the PC plays arrives on the PS5 in 48 kHz stereo, in real time |
| **Languages** | English · 简体中文 · 日本語 · Français · Русский |
| **History** | Remembers the last 5 hosts — connect again in seconds |

## Great for

📄 Documents · 💻 Code · ⌨️ Terminals · 🌐 Browsing · 🖱️ everyday desktop work

Not for video playback / gaming — that's a bitmap-transport limitation, not a
bug (the upcoming GFX line will take that on).

## Install (3 steps)

**You need** a homebrew-capable PS5 and a Windows PC with
[Remote Desktop enabled](https://support.microsoft.com/en-us/windows) (Pro).

1. Grab the latest `psrdp.ffpfs` from [Releases](https://github.com/yecheng770/PS5-rdp-client/releases) (~10 MB)
2. Import it with your loader (ShadowMount+ / etaHEN / …). **PSrdp** appears on the home screen
3. Launch and enter your PC's IP (or hostname), username and password **inside the
   app** — gamepad + on-screen keyboard is all you need, no browser anywhere

Nothing is pre-configured — everything is typed by you on first launch.

<details>
<summary><b>Specs / Build from source</b></summary>

| | |
|---|---|
| Transport | Full-frame bitmap, measured 59.6 fps @ 1080p |
| Audio | RDP audio channel, 48 kHz S16 stereo |
| Protocol | FreeRDP 3.x (works with all Windows RDP servers) |
| Environment | PS5 homebrew (verified on FW 13.42) |
| Networking | RDP on port 3389 (or any port), LAN or remote hostname, no cloud |

### Build

Requires one Linux machine (Ubuntu 22.04+):

```bash
cd 源码
cp .env.example .env      # set PS5_HOST to your console
make app                  # builds, signs, packages into dist/
```

The first build downloads the official `ps5-payload-sdk` v0.42 and verifies
its SHA256 automatically.

</details>

## Related

- 🌐 **PSrdp GFX** — hardware-accelerated graphics line for smooth video & games (in development)

## License

GPL-3.0 · [`LICENSE`](LICENSE) · third-party notices:
[`THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md)

**Not affiliated with Sony Interactive Entertainment** — runs on modified
(homebrew-enabled) firmware only.

<div align="center">

[![Release](https://img.shields.io/github/v/release/yecheng770/PS5-rdp-client?label=%F0%9F%93%A6%20Download&color=orange)](https://github.com/yecheng770/PS5-rdp-client/releases)
[![Stars](https://img.shields.io/github/stars/yecheng770/PS5-rdp-client?style=social)](https://github.com/yecheng770/PS5-rdp-client/stargazers)

**If this is useful to you, please star ⭐ the project to help other PS5 owners find it**

</div>
