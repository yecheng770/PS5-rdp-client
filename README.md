# [English](#english) | [简体中文](#简体中文) | [日本語](#日本語) | [Français](#français) | [Русский](#русский)

- - -

## English

**PSrdp — turn your PS5 into a large Windows remote-desktop screen.**

Connect your PS5 to a Windows PC on your LAN and the console screen *becomes* your
PC desktop: gamepad as mouse, on-screen keyboard for typing, PC audio coming out of
the PS5 speakers. A **native system app** — not a browser wrapper, not an emulator,
no cloud service in between.

![Resolution](https://img.shields.io/badge/Resolution-1080P%20%7C%202K%20%7C%204K-green)
![Framerate](https://img.shields.io/badge/Framerate-59.6fps%401080P-orange)
![License](https://img.shields.io/badge/License-GPL--3.0-blue)

### Highlights

**🖥️ Any resolution you want** — not just presets. Type whatever you like; the
common **1080P, 2K and 4K modes are all verified on real hardware**, and unusual
resolutions such as 2560×1440 or 3440×1440 ultrawide work as well. Full-screen
1080P repaints at a measured **59.6 fps**, writing straight to the PS5 video
output — barely noticeable latency for everyday work.

**🎮 Gamepad + mouse + keyboard, all three at the same time**

- **Gamepad**: left stick = pointer, L2/R2 = left/right click, custom key mappings
- **Mouse**: a USB mouse on the PS5 is picked up automatically where the
  system grants it (depends on console/firmware; not universally available)
- **Keyboard**: plug a physical keyboard, or use the built-in on-screen keyboard
  with five language layouts
- All three inputs stay live simultaneously

**🔊 Native audio** — every sound the PC makes arrives on the PS5 as 48 kHz
stereo in real time.

**⚙️ Also on board** — connection history (last 5 hosts), remote disk
redirection (RDPDR), a five-language interface (English / 简中 / 日本語 /
Français / Русский), idle decorations, and the author signature in the
bottom-right corner.

### Where it shines vs. where it doesn't

Great for **work: office documents, code, terminals, reading, browsing static
pages**. NOT suitable for **video playback or gaming** — the transport is
bitmap-based (full-screen repaint at ~59.6 fps), which is rock solid for desktop
work but physically can't keep up with motion video. That limitation is an
architectural property, not a bug; smooth video/game streaming belongs to the
upcoming GFX line.

### Install

You need a homebrew-capable PS5 and a Windows PC with Remote Desktop enabled
(Pro: Settings → System → Remote Desktop).

1. Grab `PPSA90042-*.ffpfs` from [Releases](../../releases) and import it with
   your loader (ShadowMount+, etaHEN…). **PSrdp** appears on the home screen.
2. Launch PSrdp and configure your PC **inside the app** — gamepad and the
   on-screen keyboard are enough, no browser needed anywhere in the flow.
3. Connect. The PS5 screen is now your Windows desktop.

Nothing is pre-configured — you type everything yourself on first launch.

### Build from source

```bash
cd 源码
cp .env.example .env      # put your own PS5_HOST in
make app                  # builds, signs, packages into dist/
```

The first build downloads the official `ps5-payload-sdk` v0.42 and verifies its
SHA256 automatically.

### License

GPL-3.0 — full text in [`LICENSE`](LICENSE). Third-party notices:
[`源码/THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md).
Not affiliated with Sony Interactive Entertainment; runs on modified
(homebrew-enabled) firmware only.

- - -

## 简体中文

**PSrdp — 把你的 PS5 变成 Windows 远程桌面大屏。**

连上局域网里那台 Windows，游戏机的画面就是电脑的桌面：手柄当鼠标、
软键盘打字、声音从 PS5 音响出来。**系统级原生应用**——不是浏览器套壳、
不是模拟器、不需要云端中转。

![分辨率](https://img.shields.io/badge/分辨率-1080P%20%7C%202K%20%7C%204K-green)
![帧率](https://img.shields.io/badge/帧率-59.6fps%401080P-orange)
![许可](https://img.shields.io/badge/许可-GPL--3.0-blue)

### 核心卖点

**🖥️ 任意自定义分辨率** —— 不止预设，分辨率随便填。常用的
**1080P、2K、4K 全部经过真机逐档调试验证**，2560×1440、3440×1440
带鱼屏等非标分辨率同样直连。1080P 全屏重画实测 **59.6fps**，画面直写
PS5 视频输出，日常办公几乎感觉不到延迟。

**🎮 手柄/鼠标/键盘 三套输入并存**

- **手柄**：左摇杆=指针，L2/R2=鼠标左右键，按键可自定义映射
- **鼠标**：PS5 上接 USB 鼠标，系统授予权限的机型/固件下自动生效
  （依设备而定，非全部可用）
- **键盘**：外接物理键盘直接打字；没有键盘用内置软键盘（五语布局）
- 三种输入同时在线，随你切换

**🔊 音频原生回传** —— Windows 的一切声音实时以 48kHz 立体声从 PS5 播放。

**⚙️ 除此之外** —— 连接历史存最近 5 组、远程设备重定向（磁盘映射 RDPDR）、
五国语言界面（简中/English/日本語/Français/Русский）、待机装饰动画、
右下角作者签名与仓库地址。

### 适用与不适用

**最适合**办公、写代码、跑终端、看文档、浏览静态网页。
**不适合**看视频、玩游戏——位图传输方案（每帧整屏重画 59.6fps）
对桌面操作极稳，但动态视频跟不上是物理上限，不是 bug。流畅视频/游戏
属于后续图形线版本（GFX），单独发布。

### 安装

需要可跑自制应用的 PS5 + 开启远程桌面的 Windows 电脑
（专业版：设置→系统→远程桌面）。

1. 下载 [Release](../../releases) 的 `PPSA90042-*.ffpfs`，用你的 loader
   （ShadowMount+、etaHEN 等）导入，主页出现 **PSrdp**
2. 启动 PSrdp，**在应用内**用手柄+软键盘填 Windows 的 IP、账号、密码，
   全流程不需要浏览器
3. 连接成功，PS5 画面立刻变成 Windows 桌面

首次启动不预置任何信息，全部自己填。

### 从源码编译

```bash
cd 源码
cp .env.example .env      # PS5_HOST 填你自己的
make app                  # 编译+签名+打包，产物在 dist/
```

首次构建自动下载 ps5-payload-sdk v0.42 并做 SHA256 校验，无需手动配置。

### 许可

GPL-3.0（根目录 [`LICENSE`](LICENSE) 为全文）。第三方依赖与素材出处
详见 [`源码/THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md)。
本项目与 Sony Interactive Entertainment 无关，仅在已破解固件的
PS5 上运行。

作者：诚 · https://github.com/yecheng770/psrdp

- - -

## 日本語

**PSrdp — PS5 を Windows のリモートデスクトップ大画面に変える。**

LAN 内の Windows PC に接続すると、PS5 の画面がそのまま PC のデスクトップ
になります。パッドでポインタ操作、オンスクリーンキーボードで文字入力、
音は PS5 から。**ネイティブアプリ**であり、ブラウザのラッパーでも
エミュレータでもありません。

任意の解像度に対応（**1080P / 2K / 4K は実機で検証済み**、1080P で
59.6fps 実測）。パッド・マウス・キーボードの三入力を同時にサポートし、
48kHz ステレオの音声をリアルタイム再生します。

オフィス作業・コーディング・ターミナル・資料閲覧に最適。動画再生や
ゲームには不向きです（ビットマップ転送のため、実機の仕様上限です）。

インストール：[Release](../../releases) から `PPSA90042-*.ffpfs` を
ダウンロードし、お使いのローダー（ShadowMount+ / etaHEN など）で導入。
**アプリ内で** Windows 側の IP・アカウントを設定（ブラウザ不要）。

ライセンス：GPL-3.0（[`LICENSE`](LICENSE)）。本プロジェクトは
Sony Interactive Entertainment と関係ありません。

- - -

## Français

**PSrdp — transformez votre PS5 en grand écran de bureau distant Windows.**

Connectez la PS5 à un PC Windows du réseau local : l'écran de la console
devient le bureau du PC. Manette en guise de souris, clavier à l'écran,
le son du PC sort des haut-parleurs de la PS5. **Application native** —
pas un habillage de navigateur, pas un émulateur, aucun service cloud.

Résolution libre, avec **1080P / 2K / 4K validées sur vraie console**
(59,6 fps mesurés en 1080P). Manette + souris + clavier simultanés,
audio 48 kHz en stéréo temps réel.

Parfait pour le travail : documents, code, terminal, lecture. **Inadapté
à la vidéo et aux jeux** (transport bitmap, limite physique).

Installation : téléchargez `PPSA90042-*.ffpfs` depuis
[Release](../../releases), importez-le via votre loader
(ShadowMount+ / etaHEN…), configurez **dans l'application** (sans navigateur).

Licence : GPL-3.0 ([`LICENSE`](LICENSE)). Sans lien avec Sony
Interactive Entertainment.

- - -

## Русский

**PSrdp — превратите PS5 в большой экран удалённого рабочего стола Windows.**

Подключите PS5 к компьютеру с Windows в локальной сети: экран консоли
станет рабочим столом ПК. Геймпад вместо мыши, экранная клавиатура,
звук компьютера через PS5. **Нативное приложение** — не браузерная
обёртка, не эмулятор, без облака.

Свободный выбор разрешения; **1080P / 2K / 4K проверены на реальной
консоли** (59,6 к/с в 1080P). Геймпад + мышь + клавиатура одновременно,
звук 48 кГц стерео в реальном времени.

Подходит для работы: документы, код, терминал, чтение. **Не подходит
для видео и игр** (битмап-транспорт, физический предел).

Установка: скачайте `PPSA90042-*.ffpfs` со страницы
[Release](../../releases), импортируйте через свой загрузчик
(ShadowMount+ / etaHEN…), настройка **внутри приложения** (без браузера).

Лицензия: GPL-3.0 ([`LICENSE`](LICENSE)). Не связано с Sony
Interactive Entertainment.
