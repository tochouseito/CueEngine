param([string]$VcpkgRoot = $env:VCPKG_ROOT)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vcpkg_environment.ps1')
$env:VCPKG_ROOT = get_vcpkg_root -Root $VcpkgRoot
$repositoryRoot = Split-Path -Parent $PSScriptRoot

# Program Files の Tool 配置に Download／Cache を書き込まない
$env:VCPKG_DOWNLOADS = Join-Path $repositoryRoot 'out/vcpkg/downloads'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $repositoryRoot 'out/vcpkg/binary-cache'
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS, $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null

& (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') install --triplet x64-windows `
    "--x-manifest-root=$repositoryRoot/ThirdParty" `
    "--x-install-root=$repositoryRoot/ThirdParty/vcpkg_installed"
exit $LASTEXITCODE
