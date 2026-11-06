---
title: 最初のアプリを作る
sidebar_position: 2
---

[CueHub と Engine のインストール](./install.md) が済んだら、RotateCube テンプレートを使い、C++ Script で Cube が回るアプリを作ります。

## Project を作る

1. Hub の **Projects** タブで Project 名と保存先を選びます。
2. 導入済みの Engine バージョンと **RotateCube** テンプレートを選択し、**Project を作成** を押します。
3. 作成した Project を一覧で選び、**Editor で開く** を押します。

## Editor で Cube を追加する

1. Editor でテンプレートの起動シーン `Main` を開きます。**Script → Build Scripts** を実行し、ビルドが終わるまで待ちます。
2. **Objects → Add Cube** で Cube を追加し、Inspector の **Position** で、最初の Cube と重ならない場所へ移動します。
3. Inspector の **Add Script** で **Rotate Cube** を選びます。**Speed (degrees/sec)** に回転速度を入力して Enter で確定し、**Rotate** を有効にします。
4. **File → Save Scene** で保存します。**Play** を押すと別ウィンドウで回転を確認できます。確認後は **Stop** を押します。

![Editor から Play を開始し、別ウィンドウで Cube を表示した例](./images/rotate-cube-play.png)

:::note 起動シーン

現在の Preview では、製品ビルドには Project の起動シーンが使われます。この手順ではテンプレートが起動シーンに指定している `Main` を編集します。

:::

## Windows アプリをビルドする

1. **Build → Build Settings...** を開き、**Windows**、**x64**、**Release** を選びます。
2. **Build** を押し、完了まで待ちます。失敗した場合は **Build Log** で詳細を確認できます。
3. **Run** で生成アプリを開き、追加した Cube が回り続けることを確認します。生成されたアプリは、Project の `Build/Windows/x64/Release/Player` にあります。
4. Editor を閉じた後もアプリが動くことを確認し、最後にアプリのウィンドウを閉じます。
