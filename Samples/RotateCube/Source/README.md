# RotateCube Source

`RotateCube.cpp` に回転する Behaviour と型登録を配置する。
各 Script の `CUE_DEFINE_SCRIPT(...)` が型登録を定義し、対応表を読む CMake が接続入口を一つだけ生成する。
Script の追加は Editor の `Script > New Script...` から行える。手書きの `Game.cpp` は不要とする。
