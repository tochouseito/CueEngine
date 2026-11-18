---
title: 最初のアプリを作る
sidebar_position: 2
---

[CueHub と Engine のインストール](./install.md) が済んだら、空の Project に Cube と C++ Script を追加し、Cube が回るアプリを作ります。
以下のコード例は、引数なしの `void start()` と `void update()` に対応した Engine SDK を対象にしています。

## Project を作る

1. Hub の **プロジェクト** 画面で、右上の **新しいプロジェクト** を押します。
2. 作成ページで使用する Engine、プロジェクト名、アプリ識別名、保存先（親フォルダ）を入力し、**作成** を押します。指定した保存先の下にプロジェクト名のフォルダを新規作成します。保存先が存在しない場合は、途中の階層も含めて自動作成します。実際の作成先は画面に表示されます。テンプレートの選択はありません。
3. 作成した Project を一覧で選び、**Editor で開く** を押します。

## Editor で Cube を追加する

1. Editor で起動シーン `Main` を開きます。**Edit → Add Cube** で Cube を追加します。
2. **Edit → New Script...** で名前を `RotateCube` として作成します。`Source/RotateCube.h` にクラス定義、`Source/RotateCube.cpp` に実装と型登録が生成されます。
3. **Script → Open Visual Studio** でゲームのプロジェクトフォルダーを開きます。`Source/RotateCube.cpp` の `namespace game` 内にある `RotateCube::update()` を下記の実装へ置き換えて保存します。

```cpp
void RotateCube::update()
{
    transform.rotate.y += 90.0f * delta_time_seconds();
}
```

`start()` と `update()` は引数なしの `void` です。旧 SDK から移行する場合は、ヘッダーと実装の両方を更新し、成功を返す `return` を削除してゲームコードを再ビルドしてください。旧 `GameObject&` が必要な操作は `self()` の結果から行えます。更新引数の `deltaTime` と `tick` は `delta_time()` と `update_tick()` に置き換えられます。取得した `GameObject` はその Callback の間に使ってください。

4. **Script → Build Scripts** を実行し、ビルドが終わるまで待ちます。
5. Cube を選び、Inspector の **Add Script** で **RotateCube** を選びます。
6. **File → Save Scene** で保存します。**Play** を押すと **Scene View** 内で Cube の回転を確認できます。確認後は **Stop** を押します。

Engine Preview 5 以降では、ゲームは別プロセスで実行しながら Editor 内に表示します。ゲームがクラッシュした場合も Play を終了し、編集中の Scene を保持します。以前の Preview では別ウィンドウで表示します。

Play 中も Inspector で Transform と Script の設定を変更できます。これらは実行中の値だけに反映し、**Stop** 後は編集用の Scene に戻ります。Play 中の変更は Scene に保存されません。

Scene View の画像上では、右マウスボタンを押しながら **WASD / QE** で移動、右ドラッグで向きを変更、ホイールで前後移動できます。**Scene camera** で Scene の Camera 表示へ戻ります。この操作で Scene に保存した Camera は変更されません。

:::note 起動シーン

現在の Preview では、製品ビルドには Project の起動シーンが使われます。この手順では新規 Project の起動シーン `Main` を編集します。

:::

## void update() で操作の失敗を扱う

検索や明示的な操作の多くは `Result` を返します。たとえば回転に失敗したフレームの処理を直ちに中断するなら、`try_error()` で失敗を取り出し、`Error` を投げます。Engine が例外を受け取り、Play に失敗を伝えます。

```cpp
void RotateCube::update()
{
    auto rotated = transform.rotate.y.add(90.0f * delta_time_seconds());
    if (const auto* error = rotated.try_error())
    {
        throw *error;
    }
}
```

一方、シーンに任意のオブジェクトが存在しない場合などは、想定している失敗だけをその場で扱えます。次の例では `OptionalMarker` がなければ今回の更新を見送り、同名オブジェクトが複数あるなどの失敗は Engine に渡します。

```cpp
void RotateCube::update()
{
    auto marker = find_by_name("OptionalMarker");
    if (const auto* error = marker.try_error())
    {
        if (error->operation == "World.find.not_found")
        {
            return;
        }
        throw *error;
    }
    transform.rotate.y += 90.0f * delta_time_seconds();
}
```

`+=` での回転が失敗した場合、Engine へ届くのは Callback の終了時です。直後の処理も中断したいときは、最初の例のように `add()` の戻り値を確認します。

## Windows アプリをビルドする

1. **Build → Build Settings...** を開き、**Windows**、**x64**、**Release** を選びます。
2. **Build** を押し、完了まで待ちます。失敗した場合は **Build Log** で詳細を確認できます。
3. **Run** で生成アプリを開き、追加した Cube が回り続けることを確認します。生成されたアプリは、Project の `Build/Windows/x64/Release/App` にあります。
4. Editor を閉じた後もアプリが動くことを確認し、最後にアプリのウィンドウを閉じます。
