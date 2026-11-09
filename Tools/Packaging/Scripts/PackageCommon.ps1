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
    if ((Get-Item -LiteralPath $manifestFile).Length -gt 262144) { throw 'Release manifest is too large' }
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
        Assert-CueJsonUniqueFields $document.RootElement
    }
    finally { $document.Dispose() }
    return $text | ConvertFrom-Json -AsHashtable
}

# 入れ子の Part も含め、解釈側によって意味が変わる重複 Key を拒否する
function Assert-CueJsonUniqueFields([Text.Json.JsonElement]$Element)
{
    if ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object)
    {
        $keys = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($property in $Element.EnumerateObject())
        {
            if (-not $keys.Add($property.Name)) { throw 'Release manifest has duplicate fields' }
            Assert-CueJsonUniqueFields $property.Value
        }
    }
    elseif ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array)
    {
        foreach ($item in $Element.EnumerateArray()) { Assert-CueJsonUniqueFields $item }
    }
}

# JSON Number の整数だけを受け入れ、文字列や小数の暗黙変換を避ける
function Test-CuePositiveSize([object]$Value, [long]$Maximum = 1099511627776)
{
    return ($Value -is [int] -or $Value -is [long] -or $Value -is [System.Numerics.BigInteger]) -and
        $Value -gt 0 -and $Value -le $Maximum
}

# 元 ZIP を固定 Buffer で分割する。既存 File は上書きせず、失敗時は元 ZIP と部分生成物を保持する
function Split-CuePackage([string]$ZipFile, [long]$PartSizeBytes = 1073741824,
    [long]$SinglePackageLimitBytes = 2147483647)
{
    if (-not (Test-CuePositiveSize $PartSizeBytes 2147483647) -or
        -not (Test-CuePositiveSize $SinglePackageLimitBytes 2147483647)) { throw 'Invalid package part limits' }
    $source = Assert-CuePlainPath $ZipFile
    $size = (Get-Item -LiteralPath $source).Length
    if (-not (Test-CuePositiveSize $size)) { throw 'Package size exceeds the supported limit' }
    if ($size -le $SinglePackageLimitBytes) { return @() }
    $count = [long][Math]::Ceiling($size / [double]$PartSizeBytes)
    if ($count -gt 512) { throw 'Package requires more than 512 parts' }
    $targets = @(for ($index = 1; $index -le $count; $index++) { $source + '.' + $index.ToString('000') })
    foreach ($target in $targets)
    {
        $null = Assert-CuePlainPath $target
        if (Test-Path -LiteralPath $target) { throw 'A package part already exists' }
    }
    $parts = [Collections.Generic.List[object]]::new()
    $inputStream = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $buffer = [byte[]]::new(1048576)
    try
    {
        foreach ($target in $targets)
        {
            $remaining = [Math]::Min($PartSizeBytes, $size - $inputStream.Position)
            $partLength = $remaining
            $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
            $outputStream = $null
            try
            {
                $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                while ($remaining -gt 0)
                {
                    $read = $inputStream.Read($buffer, 0, [int][Math]::Min($buffer.Length, $remaining))
                    if ($read -eq 0) { throw 'Package changed during splitting' }
                    $outputStream.Write($buffer, 0, $read)
                    $hash.AppendData($buffer, 0, $read)
                    $remaining -= $read
                }
                $parts.Add([ordered]@{ fileName = [IO.Path]::GetFileName($target);
                    sha256 = [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant(); size = $partLength })
            }
            finally { if ($null -ne $outputStream) { $outputStream.Dispose() }; $hash.Dispose() }
        }
        if ($inputStream.Position -ne $inputStream.Length) { throw 'Package changed during splitting' }
    }
    finally { $inputStream.Dispose() }
    return $parts.ToArray()
}

# 公開前に Part ごとの Hash と結合順の Hash を同時に検証し、巨大な結合コピーを作らない
function Assert-CuePackageParts([Collections.IDictionary]$Manifest, [string]$Root)
{
    if ($Manifest.packageParts -isnot [array] -or $Manifest.packageParts.Count -lt 1 -or
        $Manifest.packageParts.Count -gt 512) { throw 'Invalid package part count' }
    $combined = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $buffer = [byte[]]::new(1048576)
    $total = [long]0
    $assets = [Collections.Generic.List[string]]::new()
    try
    {
        for ($index = 0; $index -lt $Manifest.packageParts.Count; $index++)
        {
            $part = $Manifest.packageParts[$index]
            $expectedName = $Manifest.packageFileName + '.' + ($index + 1).ToString('000')
            if ($part -isnot [Collections.IDictionary] -or $part.Count -ne 3 -or
                $part.Keys.Where({ @('fileName', 'sha256', 'size') -cnotcontains $_ }).Count -ne 0 -or
                $part.fileName -cne $expectedName -or $part.sha256 -isnot [string] -or
                $part.sha256 -cnotmatch '^[0-9a-fA-F]{64}$' -or
                -not (Test-CuePositiveSize $part.size 2147483647)) { throw 'Invalid package part metadata' }
            $file = Assert-CuePlainPath (Join-Path $Root $part.fileName)
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
                (Get-Item -LiteralPath $file).Length -ne $part.size) { throw 'Package part is missing or has an invalid size' }
            $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
            $stream = [IO.File]::OpenRead($file)
            try
            {
                while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0)
                {
                    $combined.AppendData($buffer, 0, $read)
                    $hash.AppendData($buffer, 0, $read)
                }
                if ([Convert]::ToHexString($hash.GetHashAndReset()) -ine $part.sha256) { throw 'Package part hash mismatch' }
            }
            finally { $stream.Dispose(); $hash.Dispose() }
            $total += $part.size
            if ($total -gt 1099511627776) { throw 'Combined package exceeds the supported limit' }
            $assets.Add($file)
        }
        if ($total -ne $Manifest.packageSize -or
            [Convert]::ToHexString($combined.GetHashAndReset()) -ine $Manifest.packageSha256)
        {
            throw 'Combined package no longer matches its signed manifest'
        }
    }
    finally { $combined.Dispose() }
    return $assets.ToArray()
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
            if ($item.Length -gt 1099511627776 - $total) { throw 'Expanded package exceeds the supported limit' }
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
