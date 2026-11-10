---
title: 最初のアプリを作る
sidebar_position: 2
---

[CueHub と Engine のインストール](./install.md) が済んだら、空の Project に Cube と C++ Script を追加し、Cube が回るアプリを作ります。

## Project を作る

1. Hub の **プロジェクト** 画面で、右上の **新しいプロジェクト** を押します。
2. 作成ページで使用する Engine、プロジェクト名、アプリ識別名、保存先（親フォルダ）を入力し、**作成** を押します。指定した保存先の下にプロジェクト名のフォルダを新規作成します。保存先が存在しない場合は、途中の階層も含めて自動作成します。実際の作成先は画面に表示されます。テンプレートの選択はありません。
3. 作成した Project を一覧で選び、**Editor で開く** を押します。

## Editor で Cube を追加する

1. Editor で起動シーン `Main` を開きます。**Objects → Add Cube** で Cube を追加します。
2. **Script → New Script...** で名前を `RotateCube` として作成します。`Source/RotateCube.cpp` と型登録が生成され、ビルド対象へ自動的に加わります。`Game.cpp` は不要です。
3. **Script → Open Visual Studio** でゲームのプロジェクトフォルダーを開きます。`Source/RotateCube.cpp` の先頭に `#include <chrono>` と `#include <Math/Quaternion.h>` を追加し、`RotateCube` の `update()` を下記の実装へ置き換えて保存します。

```cpp
[[nodiscard]] cue::Result<void> update(cue::GameObject &a_object,
                                     const cue::ScriptUpdateContext &a_context) override
{
    auto transform = a_object.transform();
    if (!transform.has_value())
        return cue::Result<void>::failure(*transform.try_error());

    const auto seconds = std::chrono::duration<float>(a_context.deltaTime).count();
    auto delta = cue::math::rotation_axis_angle(cue::math::float3{0, 1, 0},
                                               cue::math::degrees_to_radians(90.0f) * seconds);
    if (!delta.has_value())
        return cue::Result<void>::failure(*delta.try_error());

    auto rotation = cue::math::normalize(*delta.try_value() * transform.try_value()->rotation);
    if (!rotation.has_value())
        return cue::Result<void>::failure(*rotation.try_error());

    transform.try_value()->rotation = *rotation.try_value();
    return a_object.set_transform(*transform.try_value());
}
```

4. **Script → Build Scripts** を実行し、ビルドが終わるまで待ちます。
5. Cube を選び、Inspector の **Add Script** で **RotateCube** を選びます。
6. **File → Save Scene** で保存します。**Play** を押すと **Scene View** 内で Cube の回転を確認できます。確認後は **Stop** を押します。

Engine Preview 5 以降では、ゲームは別プロセスで実行しながら Editor 内に表示します。ゲームがクラッシュした場合も Play を終了し、編集中の Scene を保持します。以前の Preview では別ウィンドウで表示します。

Play 中も Inspector で Transform と Script の設定を変更できます。これらは実行中の値だけに反映し、**Stop** 後は編集用の Scene に戻ります。Play 中の変更は Scene に保存されません。

Scene View の画像上では、右マウスボタンを押しながら **WASD / QE** で移動、右ドラッグで向きを変更、ホイールで前後移動できます。**Scene camera** で Scene の Camera 表示へ戻ります。この操作で Scene に保存した Camera は変更されません。

:::note 起動シーン

現在の Preview では、製品ビルドには Project の起動シーンが使われます。この手順では新規 Project の起動シーン `Main` を編集します。

:::

## Windows アプリをビルドする

1. **Build → Build Settings...** を開き、**Windows**、**x64**、**Release** を選びます。
2. **Build** を押し、完了まで待ちます。失敗した場合は **Build Log** で詳細を確認できます。
3. **Run** で生成アプリを開き、追加した Cube が回り続けることを確認します。生成されたアプリは、Project の `Build/Windows/x64/Release/App` にあります。
4. Editor を閉じた後もアプリが動くことを確認し、最後にアプリのウィンドウを閉じます。
