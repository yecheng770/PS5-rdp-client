<div align="center">

# 🎮 PSrdp — PS5 リモートデスクトップクライアント

### PS5 を Windows PC の大画面に変える

[English](README.md) · [简体中文](README.zh-CN.md) · **日本語**

![解像度](https://img.shields.io/badge/1080P%20%7C%202K%20%7C%204K-実機検証済-green)
![FPS](https://img.shields.io/badge/59.6fps_%401080p-実測-orange)
![ライセンス](https://img.shields.io/badge/License-GPL--3.0-lightgrey)

**[⬇️ Releases からダウンロード](https://github.com/yecheng770/PS5-rdp-client/releases)**

</div>

## これは何

PSrdp は PS5 を Windows PC の大画面ディスプレイに変えるアプリです：
ソファからゲームパッドで PC のデスクトップを操作し、PC の音は PS5 から再生。
**PS5 ネイティブアプリ** — ブラウザを使わず、クラウドも経由しません。

> 🏠 LAN でもリモートのホスト名でも接続可 · データは一切アップロードされない · 無料・オープンソース

## 機能

| | |
|---|---|
| **解像度** | 任意の値を入力可 — **1080P / 2K / 4K は実機で検証済み**（1080p で 59.6 fps 実測） |
| **操作** | ゲームパッド（左スティック=ポインタ、L2/R2=クリック）· USB マウス · オンスクリーンキーボード |
| **音声** | PC のサウンドは 48kHz ステレオでリアルタイムに PS5 から再生 |
| **UI 言語** | 日本語 · English · 简体中文 · Français · Русский |
| **履歴** | 直近 5 台のホストを記憶 — 次回は即接続 |

## おすすめの用途

📄 資料閲覧 · 💻 コーディング · ⌨️ ターミナル · 🌐 ウェブ閲覧 · 🖱️ 日常のデスクトップ作業

動画再生・ゲームには不向きです（ビットマップ転送の仕様上の制限で、バグではありません。
今後の GFX ラインで解決予定）。

## インストール（3 ステップ）

**必要なもの**：自作アプリが動く PS5 + リモートデスクトップが有効な Windows PC（Pro 版）。

1. [Releases](https://github.com/yecheng770/PS5-rdp-client/releases) から最新の `psrdp.ffpfs` をダウンロード（約 10 MB）
2. お使いのローダー（ShadowMount+ / etaHEN など）でインポート — ホーム画面に **PSrdp** が現れる
3. アプリを起動し、**アプリ内で** PC の IP（またはホスト名）・ユーザー名・パスワードを入力 —
   ゲームパッドとオンスクリーンキーボードだけで完結、ブラウザ不要

事前設定は一切なく、初回起動時に自分で入力します。

<details>
<summary><b>仕様 / ソースからビルド</b></summary>

| | |
|---|---|
| 転送方式 | ビットマップ全フレーム書き換え、1080p 実測 59.6 fps |
| 音声 | RDP オーディオチャンネル、48kHz S16 ステレオ |
| プロトコル | FreeRDP 3.x（Windows の RDP サーバーに全対応） |
| 環境 | PS5 改造ファーム（FW 13.42 で検証） |
| ネットワーク | RDP 既定 3389（変更可）、LAN でもリモートでも、クラウド不要 |

### ビルド

Linux マシン（Ubuntu 22.04+）が必要です：

```bash
cd 源码
cp .env.example .env      # PS5_HOST に PS5 の IP を入力
make app                  # ビルド・署名・パッケージ化して dist/ へ
```

初回ビルドで公式 `ps5-payload-sdk` v0.42 を自動ダウンロードし SHA256 を検証します。

</details>

## 関連リポジトリ

- 🌐 **PSrdp GFX** — ハードウェアアクセラレーションによる映像・ゲーム対応ライン（開発中）

## ライセンス

GPL-3.0 · [`LICENSE`](LICENSE) · サードパーティ表記：
[`THIRD-PARTY-NOTICES.md`](源码/THIRD-PARTY-NOTICES.md)

**Sony Interactive Entertainment とは無関係です** — 改造済みファームの
PS5 でのみ動作します。

<div align="center">

[![Release](https://img.shields.io/github/v/release/yecheng770/PS5-rdp-client?label=%F0%9F%93%A6%20%E3%83%80%E3%82%A6%E3%83%B3%E3%83%AD%E3%83%BC%E3%83%89&color=orange)](https://github.com/yecheng770/PS5-rdp-client/releases)
[![Stars](https://img.shields.io/github/stars/yecheng770/PS5-rdp-client?style=social)](https://github.com/yecheng770/PS5-rdp-client/stargazers)

**役に立ったら Star ⭐ をいただけると、多くの PS5 ユーザーの目に留まります**

</div>
