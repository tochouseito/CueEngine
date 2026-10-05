# 明示指定、環境変数、Visual Studio 同梱版の順で vcpkg を解決する
function get_vcpkg_root
{
    param([string]$Root = $env:VCPKG_ROOT)

    if ([string]::IsNullOrWhiteSpace($Root))
    {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere)
        {
            $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installation)
            {
                $Root = Join-Path $installation 'VC/vcpkg'
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($Root) -or
        !(Test-Path -LiteralPath (Join-Path $Root 'vcpkg.exe')) -or
        !(Test-Path -LiteralPath (Join-Path $Root 'scripts/buildsystems/vcpkg.cmake')))
    {
        throw 'vcpkg.exe と Toolchain が必要です。VCPKG_ROOT を指定して scripts/prepare_dependencies.ps1 を実行してください。'
    }
    return (Resolve-Path -LiteralPath $Root).Path
}
