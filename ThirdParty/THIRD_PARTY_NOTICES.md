# Third-party notices

## Dear ImGui

- 出自: https://github.com/ocornut/imgui
- 上流 Tag: `v1.92.9b-docking`
- vcpkg Port: `imgui` Version `1.92.9`
- Registry baseline: `19780d9cdf84d0944cf9a318666703b89ab6629c`
- Feature: `docking-experimental`、`win32-binding`、`dx12-binding`
- License: MIT、全文は [Licenses/ImGui-MIT.txt](Licenses/ImGui-MIT.txt)
- 対象: Core、Demo、公式 Win32／DirectX 12 Backend、同梱 Font／補助実装

第三者 Source は変更せず、vcpkg の公式 Port を利用する。Dear ImGui を含む Editor Binary または Source を再配布する場合は、上記 License の Copyright と許諾文を同梱する。Source 内の補助実装に付属する Notice も保持する。

## vcpkg

- 出自: https://github.com/microsoft/vcpkg
- 取得に利用した Tool: `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`（Visual Studio 同梱）
- Registry の Build 補助 Port: `vcpkg-cmake`（`2025-08-07`）、`vcpkg-cmake-config`（`2026-07-21`）。Manifest baseline で固定
- License: MIT、全文は [Licenses/vcpkg-MIT.txt](Licenses/vcpkg-MIT.txt)

`vcpkg-cmake-config` に付属する Notice は [Licenses/VcpkgCMakeConfig-MIT.txt](Licenses/VcpkgCMakeConfig-MIT.txt) に保持する。

vcpkg Tool と Build 補助 Port は開発時に利用し、Editor Binary には含めない。これらの Source／Script を再配布する場合は Copyright と許諾文を保持する。
