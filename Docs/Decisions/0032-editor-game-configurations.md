# ADR-0032: Editor と独立した Game 構成の選択

- 状態: 採用
- 日付: 2026-11-09
- 関連: ADR-0016、ADR-0020、ADR-0031

## 選択と適用

Editor の Debug Play の隣に Game の Debug / Development / Release 選択欄を置く。
既定は Development。Editor / PlayHost / 内蔵 Engine の構成を変更せず、Game DLL の構成だけを選択する。

- Build Scripts は選択した構成で Game DLL / 対応 Static Library を生成する
- Play は同じ構成の検証済み Manifest / DLL を実行し、別構成の成果物へ戻らない
- Debug Play は選択した構成で Build し、同じ DLL の読み込み前に Debugger を接続する
- Game DLL は現行 CMake の全構成で PDB を生成する。最適化された構成では変数・実行位置の観測に制約がある
- 製品アプリの構成は Build Settings で別に選択する。Game 選択から製品の Target は変更しない

Play / Metadata 取得 / Build / IDE 操作 / 保存確認中は構成変更を拒否する。
Build の対象は要求開始時に採取し、実行途中の UI 表示値で変更しない。
構成変更時は以前の DLL と型情報を破棄し、未 Build なら Build required を表示する。
未保存の Scene と Undo の正本は変更しない。

## Project ローカル設定

`<Project>/Intermediate/Editor/Settings.cueuser` に保存する。

```text
formatVersion=1
projectId=<Project ID>
gameConfiguration=Development
```

Project を開くときに一度読み、変更操作時だけ原子的に置換する。Frame ごとの File I/O は行わない。
File 未存在は Development。不正な形式 / 別 Project / 未知構成は通知して既定値を使う。
保存失敗時は以前の選択と接続を保つ。Project 切替では候補 Project の設定と一緒に公開する。
設定を `.cueproj` や Scene に混ぜず、共有 Project の保存・履歴から独立させる。

## 異なる構成の DLL 境界

既存 C ABI の configuration 値を維持し、Debug=1 / Development=2 / Release=3 を表す。
Editor は選択構成を PlayClient の所有値にし、Metadata / Play / Debug Play の子へ明示する。
PlayHost は DLL の宣言構成を選択した期待値と照合する。未知構成と不一致を拒否する。
ABI Version、Table Size、Pointer Size、Callback、Metadata、Thread、Instance 寿命の検証は維持する。

- 明示した DLL 読込だけが別構成を許可する
- 構成を省略した従来の読込と製品の `from_static` は同構成の照合を維持する
- STL / Result / C++ 例外 / Allocator を DLL の C ABI へ流さない
- Instance の確保・破棄と例外処理は DLL 内に閉じる
- Debug CRT は Game 開発 PC の Visual Studio 環境を前提とし、配布 Engine に Debug CRT を同梱しない

Engine / Editor のソース構成と Game の最適化を独立して調査できるようにする。
