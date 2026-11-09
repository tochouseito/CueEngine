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
if ($product -eq 'engine' -and $manifest.schemaVersion -eq 2) { $expectedFields += 'packageParts' }
if ($manifest.Count -ne $expectedFields.Count -or $manifest.Keys.Where({ $expectedFields -cnotcontains $_ }).Count -ne 0 -or
    ($manifest.schemaVersion -isnot [int] -and $manifest.schemaVersion -isnot [long]) -or
    ($manifest.schemaVersion -ne 1 -and ($product -ne 'engine' -or $manifest.schemaVersion -ne 2)) -or
    $manifest.packageFileName -isnot [string] -or $manifest.packageFileName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._+-]+$' -or
    $manifest.packageSha256 -isnot [string] -or $manifest.packageSha256 -cnotmatch '^[0-9a-fA-F]{64}$' -or
    -not (Test-CuePositiveSize $manifest.packageSize))
{
    throw 'Release manifest schema is invalid'
}
if ($product -eq 'engine' -and ($manifest.platform -cne 'Windows' -or $manifest.architecture -cne 'x64' -or
    $manifest.configuration -cne 'Development' -or -not (Test-CuePositiveSize $manifest.expandedSize) -or
    $manifest.buildId -isnot [string] -or $manifest.buildId -cnotmatch '^[0-9A-Za-z._+-]{1,128}$' -or
    $manifest.packageFileName -cne "CueEngine-$($manifest.version)-windows-x64.zip"))
{
    throw 'Engine release contract is invalid'
}
foreach ($name in @($manifestFile, ($manifestFile + '.sig')))
{
    if (-not (Test-Path -LiteralPath $name -PathType Leaf)) { throw 'A signed release asset is missing' }
}
$package = Join-Path $root $manifest.packageFileName
$assets = @($manifestFile, ($manifestFile + '.sig'))
if ($product -eq 'engine' -and $manifest.schemaVersion -eq 2)
{
    if (Test-Path -LiteralPath $package) { throw 'A multipart release must not contain the logical ZIP asset' }
    $assets += @(Assert-CuePackageParts $manifest $root)
}
else
{
    $null = Assert-CuePlainPath $package
    if (-not (Test-CuePositiveSize $manifest.packageSize 2147483647) -or
        -not (Test-Path -LiteralPath $package -PathType Leaf) -or
        (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ine $manifest.packageSha256 -or
        (Get-Item -LiteralPath $package).Length -ne $manifest.packageSize)
    {
        throw 'Release package no longer matches its signed manifest'
    }
    $assets += $package
}
if ($product -eq 'hub')
{
    if ($manifest.feedFileName -cne 'releases.win.json' -or -not (Test-CuePositiveSize $manifest.feedSize 2147483647) -or
        $manifest.feedSha256 -isnot [string] -or $manifest.feedSha256 -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'Hub feed metadata is invalid' }
    $feedFile = Assert-CuePlainPath (Join-Path $root $manifest.feedFileName)
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
# 多数の絶対 Path を一度に渡して Windows の Command line 上限を超えないようにする
$isMultipart = $product -eq 'engine' -and $manifest.schemaVersion -eq 2
$initialAssets = if ($isMultipart) { @($manifestFile, ($manifestFile + '.sig')) } else { $assets }
$arguments = @('release', 'create', $Tag) + $initialAssets + @('--repo', $Repository, '--verify-tag', '--draft', '--title', "$product $($manifest.version)", '--notes-file', $notesFile)
if ($isPreview) { $arguments += '--prerelease' }
& gh @arguments
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed; no public release was completed' }
if ($isMultipart)
{
    # File を個別に添付し、Upload 失敗時は公開せず Draft を診断用に残す
    foreach ($asset in $assets[2..($assets.Count - 1)])
    {
        & gh release upload $Tag $asset --repo $Repository
        if ($LASTEXITCODE -ne 0) { throw 'A package part upload failed; release remains a draft' }
    }
}
$previewArgument = '--prerelease=' + $isPreview.ToString().ToLowerInvariant()
$latestArgument = '--latest=' + ($product -eq 'engine' -and -not $isPreview).ToString().ToLowerInvariant()
& gh release edit $Tag --repo $Repository --draft=false $previewArgument $latestArgument
if ($LASTEXITCODE -ne 0) { throw 'Release remains a draft; public completion was not confirmed' }
Write-Output "Published $Repository $Tag"
