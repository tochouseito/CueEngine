---
title: インストール
sidebar_position: 1
---

## 必要な環境

- Windows x64 と DirectX 12 に対応したグラフィックス環境。
- アプリのビルドには Visual Studio 2026 の **C++ によるデスクトップ開発**、MSVC x64 ツールセット、Windows SDK、CMake 4.2 以上が必要です。Visual Studio に付属する CMake を使う場合もバージョンを確認してください。

## CueHub をインストールする

1. [ダウンロード](/download) で Windows x64 用の CueHub Setup を入手します。Preview を試す場合は、Preview 欄の Setup を選びます。
2. ダウンロードした `CueEngine.Hub-win-Setup.exe` を実行します。
3. Windows のスタートメニューから **CueEngine Hub** を起動します。

## Hub から Engine を導入する

1. **インストール** 画面の右上にある **Engine をインストール** を押します。Preview を探す場合は **設定** の **Preview を表示・更新候補に含める** を有効にします。
2. 別ウィンドウの **正式リリース** または **Preview** を選び、利用するバージョンの **インストール** を押します。
3. 導入したバージョンが一覧に表示されたら、[最初のアプリを作る](./first-app.md) に進みます。

Hub は発行者の署名とファイルのハッシュを検証してから Engine を登録します。公開版一覧を利用できない場合は、同じ Release の Engine ZIP、`engine-release.json`、`engine-release.json.sig` を同じフォルダに置き、**リストに追加 → パッケージから導入** で選択できます。

導入済み Engine は検索と **すべて／正式リリース／Preview** で絞り込めます。修復やアンインストールは各 Engine の **管理** メニューから行います。

## 既存 Project の Engine を切り替える

既存 Project は `.cueproj` の `engineVersion` で利用版を固定しています。

1. Project を開いている Editor を終了し、Hub で切り替え先の Engine を導入します。
2. Hub の **プロジェクト** 一覧で対象を選び、**Engine バージョンを変更** を押します。
3. 導入済みの Engine を選び、**変更** を押します。`.cueproj` と Hub の登録情報が更新されます。
4. 一覧から **Editor で開く** を押します。

この操作は Engine の指定だけを置き換え、互換性確認やデータ変換は行いません。Engine 未指定の旧形式 Project の変換は未対応です。新規 Project では作成時に選んだ版が自動で保存されます。

変更ボタンのない旧版 Hub では、`.cueproj` の `engineVersion` を手動で変更し、**プロジェクト → 追加** で同じ `.cueproj` を再登録してください。
