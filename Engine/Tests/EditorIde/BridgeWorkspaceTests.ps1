param([Parameter(Mandatory)][string]$Bridge)

$ErrorActionPreference = 'Stop'
$temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$root = Join-Path $temporary ('CueBridgeWorkspace-' + [Guid]::NewGuid().ToString('N'))
$link = Join-Path $root 'Linked'
$checks = 0

function reject_request([string[]]$Arguments, [string]$Expected)
{
    $result = & $Bridge @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or -not $result.Contains('success=false') -or -not $result.Contains($Expected))
    {
        throw "Bridge input validation failed: $Expected`n$result"
    }
    $script:checks++
}

try
{
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $workspace = Join-Path $root '日本語 Game'
    [IO.Directory]::CreateDirectory($workspace) | Out-Null
    $cmake = Join-Path $workspace 'CMakeLists.txt'
    [IO.File]::WriteAllText($cmake, 'project(OwnedProbe NONE)')
    $withoutCmake = Join-Path $root 'NoCmake'
    [IO.Directory]::CreateDirectory($withoutCmake) | Out-Null
    $base = @('--command', 'open-folder', '--request-id', '1')
    reject_request ($base + @('--solution', $cmake)) 'Unknown, duplicate or empty bridge argument'
    reject_request @('--command', 'open-solution', '--workspace', $workspace, '--request-id', '1') 'Unsupported bridge command'
    reject_request ($base + @('--workspace', 'relative')) 'absolute workspace path'
    reject_request ($base + @('--workspace', 'C:relative')) 'absolute workspace path'
    reject_request ($base + @('--workspace', (Join-Path $root 'Missing'))) 'workspace is missing'
    reject_request ($base + @('--workspace', $cmake)) 'workspace is missing'
    reject_request ($base + @('--workspace', $withoutCmake)) 'requested file is missing'
    # 有効な Root でも不正な Source 指定は IDE を起動する前に拒否する
    reject_request @('--command', 'open-source', '--workspace', $workspace, '--request-id', '1', '--source', $cmake, '--line', '0') 'Invalid source location'
    New-Item -ItemType Junction -Path $link -Target $workspace | Out-Null
    reject_request ($base + @('--workspace', $link)) 'workspace or a parent directory is a link'
    Write-Output "Visual Studio Bridge workspace input checks passed: $checks"
}
finally
{
    # Junction 本体を先に外し、所有する一時 Root の外へ再帰処理しない
    if ([IO.Directory]::Exists($link)) { [IO.Directory]::Delete($link, $false) }
    $resolved = [IO.Path]::GetFullPath($root)
    if (-not $resolved.StartsWith($temporary + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase))
    { throw 'Refusing cleanup outside the temporary directory' }
    if ([IO.Directory]::Exists($resolved)) { [IO.Directory]::Delete($resolved, $true) }
}
