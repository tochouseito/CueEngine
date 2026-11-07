Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 配布物名と Tag に共用する SemVer を検証し、数値識別子の曖昧な表記を拒否する
function Test-CueVersion([string]$Version)
{
    if ($Version.Length -eq 0 -or $Version.Length -gt 128) { return $false }
    if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$')
    {
        return $false
    }
    if ($Matches.ContainsKey(4))
    {
        foreach ($part in $Matches[4].Split('.'))
        {
            if ($part -cmatch '^[0-9]+$' -and $part.Length -gt 1 -and $part.StartsWith('0'))
            {
                return $false
            }
        }
    }
    return $true
}

# 呼出元が所有する Version 定義だけを読み、別 Product の公開を防ぐ
function Read-CueVersion([string]$File, [string]$Product)
{
    $definition = Get-Content -LiteralPath $File -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
    if ($definition.schemaVersion -ne 1 -or $definition.product -cne $Product -or
        -not (Test-CueVersion $definition.version))
    {
        throw "Invalid $Product version definition"
    }
    return $definition
}

# 名前空間の検証と Copy / Move の直前に使い、Junction 配下への生成を拒否する
function Assert-CuePlainPath([string]$Path)
{
    $resolved = [IO.Path]::GetFullPath($Path)
    $current = $resolved
    while ($current)
    {
        if (Test-Path -LiteralPath $current)
        {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "Package path contains a link: $current"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return $resolved
}

# 既存出力を暗黙に消さず、所有 Marker 付きの新規領域を確保する
function New-CuePackageStage([string]$OutputDirectory)
{
    $output = Assert-CuePlainPath $OutputDirectory
    if (Test-Path -LiteralPath $output) { throw "Package output already exists: $output" }
    $parent = Split-Path -Parent $output
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $stage = Join-Path $parent ('.cue-package-stage-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $marker = Join-Path $stage '.cue-package-owner'
    $stream = [IO.File]::Open($marker, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write([Text.Encoding]::UTF8.GetBytes([Guid]::NewGuid().ToString('D'))) }
    finally { $stream.Dispose() }
    return $stage
}

# 明示した File Tree だけをコピーし、コピー元の Link も追跡しない
function Copy-CuePackageTree([string]$Source, [string]$Destination)
{
    $sourceRoot = Assert-CuePlainPath $Source
    $destinationRoot = Assert-CuePlainPath $Destination
    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) { throw "Missing package source: $sourceRoot" }
    [IO.Directory]::CreateDirectory($destinationRoot) | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $sourceRoot -Force)
    {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Package source contains a link: $($item.FullName)" }
        $target = Join-Path $destinationRoot $item.Name
        if ($item.PSIsContainer) { Copy-CuePackageTree $item.FullName $target }
        else { [IO.File]::Copy($item.FullName, $target, $false) }
    }
}

# 一つの完全な JSON を LF / BOM なしで書き、署名対象の元 Byte 列を固定する
function Write-CueJson([string]$File, [object]$Value)
{
    $json = ($Value | ConvertTo-Json -Depth 20) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($File, $json + "`n", [Text.UTF8Encoding]::new($false))
}

# 元 Manifest の Byte 列を署名し、公開鍵は別の信頼設定として扱う
function Protect-CueManifest([string]$File, [string]$PrivateKeyFile)
{
    $key = [Security.Cryptography.ECDsa]::Create()
    try
    {
        $key.ImportFromPem([IO.File]::ReadAllText($PrivateKeyFile))
        if ($key.KeySize -ne 256 -or $key.ExportParameters($false).Curve.Oid.Value -cne '1.2.840.10045.3.1.7')
        {
            throw 'Package signing requires ECDSA P-256'
        }
        $signature = $key.SignData([IO.File]::ReadAllBytes($File), [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
        [IO.File]::WriteAllText($File + '.sig', [Convert]::ToBase64String($signature) + "`n", [Text.UTF8Encoding]::new($false))
    }
    finally { $key.Dispose() }
}

# 発行前の再確認にも使い、署名済みの原本と重複しない JSON Object だけを返す
function Read-CueSignedManifest([string]$File, [string]$PublicKeyFile)
{
    $manifestFile = Assert-CuePlainPath $File
    if ((Get-Item -LiteralPath $manifestFile).Length -gt 65536) { throw 'Release manifest is too large' }
    $bytes = [IO.File]::ReadAllBytes($manifestFile)
    $signatureText = [IO.File]::ReadAllText($manifestFile + '.sig')
    if ($signatureText.Length -gt 4096) { throw 'Release signature is too large' }
    $publicKey = [IO.File]::ReadAllText((Assert-CuePlainPath $PublicKeyFile))
    if ($publicKey.Contains('PRIVATE KEY')) { throw 'Only a public key may verify a release' }
    $key = [Security.Cryptography.ECDsa]::Create()
    try
    {
        $key.ImportFromPem($publicKey)
        $signature = [Convert]::FromBase64String($signatureText.Trim())
        if ($key.KeySize -ne 256 -or $key.ExportParameters($false).Curve.Oid.Value -cne '1.2.840.10045.3.1.7' -or
            $signature.Length -ne 64 -or -not $key.VerifyData($bytes, $signature,
                [Security.Cryptography.HashAlgorithmName]::SHA256,
                [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation))
        {
            throw 'Release manifest signature is invalid'
        }
    }
    finally { $key.Dispose() }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $document = [Text.Json.JsonDocument]::Parse($text)
    try
    {
        if ($document.RootElement.ValueKind -ne [Text.Json.JsonValueKind]::Object) { throw 'Release manifest must be an object' }
        $keys = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($property in $document.RootElement.EnumerateObject())
        {
            if (-not $keys.Add($property.Name)) { throw 'Release manifest has duplicate fields' }
        }
    }
    finally { $document.Dispose() }
    return $text | ConvertFrom-Json -AsHashtable
}

# File をストリームで圧縮し、大きい SDK 全体をメモリへ読み込まない
function New-CueZip([string]$SourceDirectory, [string]$ZipFile)
{
    $source = Assert-CuePlainPath $SourceDirectory
    $file = [IO.File]::Open($ZipFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $zip = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create, $false)
    $total = [long]0
    try
    {
        foreach ($item in Get-ChildItem -LiteralPath $source -Recurse -File -Force)
        {
            $null = Assert-CuePlainPath $item.FullName
            $relative = [IO.Path]::GetRelativePath($source, $item.FullName).Replace('\', '/')
            if ($relative -eq '.cue-package-owner') { continue }
            $entry = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            $inputStream = [IO.File]::OpenRead($item.FullName)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose(); $inputStream.Dispose() }
            $total += $item.Length
        }
    }
    finally { $zip.Dispose(); $file.Dispose() }
    return $total
}

# File名だけで Debug CRT と開発 Tool の混入を検出する。署名や由来の検証の代替にはしない
function Assert-CueRedistributablePayload([string]$Root)
{
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -File -Force)
    {
        if ($item.Name -imatch '^(?:msvcp[0-9]+d(?:_.+)?|vcruntime[0-9]+(?:_[0-9]+)?d|ucrtbased)\.dll$' -or
            $item.Name -imatch '^(?:cl|link|msbuild|devenv|vcpkg)\.exe$')
        {
            throw "Non-package tool or Debug CRT in payload: $($item.Name)"
        }
    }
}
