# 正規 Project Template

`Cue.ProjectTools` が既知の File だけを読み、Project / Scene / Object / Script Instance の ID を新規発行する。
`Empty3D` は Camera と未割当の空 Behaviour、`RotateCube` は一つの Cube と Camera を持つ。
Samples の手編集値、Build / Intermediate、生成済み Binary はコピーしない。

Template Source の `.in` は生成時だけ既知 Token を置換する。
Project 設定は Template からコピーせず、検証した作成要求から `.cueproj` v2 を生成する。
共通の `.gitignore` は Project Root に配置し、生成物と PC 固有の IDE 設定を Git の対象から外す。
