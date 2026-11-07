param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$PublisherPublicKeyFile
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
& (Join-Path $PSScriptRoot 'Test-ReleaseTag.ps1') -Tag $Tag
if ($Repository -cnotmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid release repository' }
$root = Assert-CuePlainPath $PackageDirectory
$product = if ($Tag.StartsWith('engine-v', [StringComparison]::Ordinal)) { 'engine' } else { 'hub' }
$manifestFile = Join-Path $root "$product-release.json"
$manifest = Read-CueSignedManifest $manifestFile $PublisherPublicKeyFile
if ($manifest.tag -cne $Tag -or $manifest.product -cne $product -or -not (Test-CueVersion $manifest.version) -or
    $Tag -cne "$product-v$($manifest.version)")
{
    throw 'Release manifest does not match the requested tag'
}
$expectedFields = if ($product -eq 'engine') {
    @('schemaVersion','product','version','tag','platform','architecture','configuration','buildId','packageFileName','packageSha256','packageSize','expandedSize')
} else {
    @('schemaVersion','product','version','tag','packageFileName','packageSha256','packageSize','feedFileName','feedSha256','feedSize')
}
if ($manifest.Count -ne $expectedFields.Count -or $manifest.Keys.Where({ $expectedFields -cnotcontains $_ }).Count -ne 0 -or
    $manifest.schemaVersion -ne 1 -or $manifest.packageFileName -cnotmatch '^[A-Za-z0-9._+-]+$' -or $manifest.packageSize -le 0)
{
    throw 'Release manifest schema is invalid'
}
if ($product -eq 'engine' -and ($manifest.platform -cne 'Windows' -or $manifest.architecture -cne 'x64' -or
    $manifest.configuration -cne 'Development' -or $manifest.expandedSize -le 0 -or
    $manifest.packageFileName -cne "CueEngine-$($manifest.version)-windows-x64.zip"))
{
    throw 'Engine release contract is invalid'
}
foreach ($name in @($manifestFile, ($manifestFile + '.sig'), (Join-Path $root $manifest.packageFileName)))
{
    if (-not (Test-Path -LiteralPath $name -PathType Leaf)) { throw 'A signed release asset is missing' }
}
$package = Join-Path $root $manifest.packageFileName
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ine $manifest.packageSha256 -or
    (Get-Item -LiteralPath $package).Length -ne $manifest.packageSize)
{
    throw 'Release package no longer matches its signed manifest'
}
$assets = @($manifestFile, ($manifestFile + '.sig'), $package)
if ($product -eq 'hub')
{
    if ($manifest.feedFileName -cne 'releases.win.json' -or $manifest.feedSize -le 0) { throw 'Hub feed metadata is invalid' }
    $feedFile = Join-Path $root $manifest.feedFileName
    if ((Get-FileHash -LiteralPath $feedFile -Algorithm SHA256).Hash -ine $manifest.feedSha256 -or
        (Get-Item -LiteralPath $feedFile).Length -ne $manifest.feedSize) { throw 'Hub feed no longer matches its signed manifest' }
    foreach ($item in Get-ChildItem -LiteralPath $root -File)
    {
        if ($item.Name -match '(?:-Setup\.exe$|^releases\..+\.json$|\.nupkg$)' -and $item.FullName -notin $assets)
        {
            $assets += $item.FullName
        }
    }
}

# 一度公開した Tag の Asset を差し替えない。失敗した Draft も明示確認なしに上書きしない
$previous = & gh release view $Tag --repo $Repository --json tagName 2>$null
if ($LASTEXITCODE -eq 0) { throw 'This release already exists; do not replace an existing version' }
$notesFile = Join-Path $root 'release-notes.md'
$notes = "CueEngine $product $($manifest.version)`n`nTag: $Tag`n`n"
$notes += '配布物、Manifest、署名を同じ Release として公開する。利用手順と既知の制約は同じ Tag の Docs を参照。'
[IO.File]::WriteAllText($notesFile, $notes, [Text.UTF8Encoding]::new($false))
$isPreview = $manifest.version.Split('+')[0].Contains('-')
$arguments = @('release', 'create', $Tag) + $assets + @('--repo', $Repository, '--verify-tag', '--draft', '--title', "$product $($manifest.version)", '--notes-file', $notesFile)
if ($isPreview) { $arguments += '--prerelease' }
& gh @arguments
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed; no public release was completed' }
$previewArgument = '--prerelease=' + $isPreview.ToString().ToLowerInvariant()
$latestArgument = '--latest=' + ($product -eq 'engine' -and -not $isPreview).ToString().ToLowerInvariant()
& gh release edit $Tag --repo $Repository --draft=false $previewArgument $latestArgument
if ($LASTEXITCODE -ne 0) { throw 'Release remains a draft; public completion was not confirmed' }
Write-Output "Published $Repository $Tag"
