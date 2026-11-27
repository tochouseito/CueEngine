param(
    [Parameter(Mandatory)][string]$ContentRoot,
    [Parameter(Mandatory)][string]$OutputFile,
    [Parameter(Mandatory)][string]$StartupScene,
    [Parameter(Mandatory)][string]$ToolCache)

$ErrorActionPreference = 'Stop'
$texconvVersion = 'may2026'
$texconvUrl = "https://github.com/microsoft/DirectXTex/releases/download/$texconvVersion/texconv.exe"
$texconvSha256 = 'dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06'
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$culture = [Globalization.CultureInfo]::InvariantCulture
$idPattern = '^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$'
$whitespace = [char[]]" `t`r"

# Cook 対象の正本を読み、リンク先の Source を暗黙に取り込まない
function Read-CueAssetFields([IO.FileInfo]$File, [string[]]$Required, [string[]]$Optional = @())
{
    if ($File.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or $File.Length -gt 4096)
    {
        throw "Invalid asset metadata file: $($File.FullName)"
    }
    $fields = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    $bytes = [IO.File]::ReadAllBytes($File.FullName)
    $content = $utf8.GetString($bytes)
    if ($content.StartsWith([char]0xFEFF)) { $content = $content.Substring(1) }
    foreach ($raw in ($content -split '\r?\n'))
    {
        $line = $raw.Trim($whitespace)
        if ($line -ceq '') { continue }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Invalid asset metadata line: $($File.FullName)" }
        $key = $line.Substring(0, $separator).Trim($whitespace)
        $value = $line.Substring($separator + 1).Trim($whitespace)
        if (($key -cnotin $Required -and $key -cnotin $Optional) -or [string]::IsNullOrEmpty($value) -or
            -not $fields.TryAdd($key, $value))
        {
            throw "Unknown, empty or repeated asset field '$key': $($File.FullName)"
        }
    }
    foreach ($key in $Required)
    {
        if (-not $fields.ContainsKey($key)) { throw "Missing asset field '$key': $($File.FullName)" }
    }
    if ($fields['version'] -cne '1' -or $fields['assetId'] -cnotmatch $idPattern -or
        $fields['assetId'] -ceq '00000000-0000-0000-0000-000000000000')
    {
        throw "Unsupported asset version or invalid Asset ID: $($File.FullName)"
    }
    return ,$fields
}

# Tool を利用する Windows x64 Build でだけ、公式 Release と固定 Hash を検証して取得する
function Get-CueTexconv()
{
    $directory = Join-Path $ToolCache "DirectXTex/$texconvVersion"
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $tool = Join-Path $directory 'texconv.exe'
    if (-not [IO.File]::Exists($tool))
    {
        $download = Join-Path $directory ("texconv-{0}.download" -f [Guid]::NewGuid().ToString('N'))
        try
        {
            Invoke-WebRequest -Uri $texconvUrl -OutFile $download -TimeoutSec 90
            if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ine $texconvSha256)
            {
                throw 'DirectXTex texconv download SHA-256 mismatch'
            }
            try { [IO.File]::Move($download, $tool, $false) }
            catch [IO.IOException] { if (-not [IO.File]::Exists($tool)) { throw } }
        }
        finally { [IO.File]::Delete($download) }
    }
    if ((Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash -ine $texconvSha256)
    {
        throw 'DirectXTex texconv cache SHA-256 mismatch'
    }
    return $tool
}

# Cook を始める前に ID と型を確定し、製品内の参照先が揃うことを確認する
$assets = [Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
$directories = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
$directories.Push([IO.DirectoryInfo]::new($ContentRoot))
while ($directories.Count -ne 0)
{
    foreach ($entry in $directories.Pop().EnumerateFileSystemInfos())
    {
        if ($entry -is [IO.DirectoryInfo])
        {
            if (-not $entry.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) { $directories.Push($entry) }
            continue
        }
        if ($entry.Extension -ieq '.cuemeta')
        {
            $fields = Read-CueAssetFields $entry @('version', 'assetId', 'type', 'colorSpace')
            $source = [IO.FileInfo]::new($entry.FullName.Substring(0, $entry.FullName.Length - '.cuemeta'.Length))
            if ($fields['type'] -cne 'Texture' -or $fields['colorSpace'] -cnotin @('sRGB', 'Linear') -or
                $source.Extension -ine '.png' -or -not $source.Exists -or
                $source.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint))
            {
                throw "Texture source or color space is invalid: $($entry.FullName)"
            }
            $asset = @{ Kind = [uint32]1; Source = $source; ColorSpace = $fields['colorSpace'] }
        }
        elseif ($entry.Extension -ieq '.cuemat')
        {
            $fields = Read-CueAssetFields $entry @('version', 'assetId', 'type', 'baseColor') @('baseColorTexture')
            $components = $fields['baseColor'].Split(',')
            if ($fields['type'] -cne 'Material' -or $components.Length -ne 4)
            {
                throw "Material type or baseColor is invalid: $($entry.FullName)"
            }
            $canonical = foreach ($component in $components)
            {
                [float]$value = 0
                if ($component.Trim($whitespace).StartsWith('+') -or
                    -not [float]::TryParse($component, [Globalization.NumberStyles]::Float, $culture, [ref]$value) -or
                    -not [float]::IsFinite($value) -or $value -lt 0 -or $value -gt 1)
                {
                    throw "Material baseColor component is invalid: $($entry.FullName)"
                }
                $value.ToString('R', $culture)
            }
            $textureId = $null
            if ($fields.TryGetValue('baseColorTexture', [ref]$textureId) -and $textureId -cnotmatch $idPattern)
            {
                throw "Material texture ID is invalid: $($entry.FullName)"
            }
            $text = "version=1`nassetId=$($fields['assetId'])`ntype=Material`nbaseColor=$($canonical -join ',')`n"
            if ($textureId) { $text += "baseColorTexture=$textureId`n" }
            $asset = @{ Kind = [uint32]2; Material = $utf8.GetBytes($text); TextureId = $textureId }
        }
        else { continue }
        if ($assets.ContainsKey($fields['assetId']))
        {
            throw "Duplicate Asset ID: $($fields['assetId'])"
        }
        $assets.Add($fields['assetId'], $asset)
        if ($assets.Count -gt 65536) { throw 'Too many assets for a single Cue asset pack' }
    }
}
# 最初は製品の起動 Scene が参照する Material と、その Base Color Texture を収録する
$selected = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
foreach ($line in [IO.File]::ReadLines($StartupScene, $utf8))
{
    $separator = $line.IndexOf('=')
    if ($separator -lt 1 -or $line.Substring(0, $separator).Trim($whitespace) -cne 'material.assetId') { continue }
    $materialId = $line.Substring($separator + 1).Trim($whitespace)
    if (-not $assets.ContainsKey($materialId) -or $assets[$materialId].Kind -ne 2)
    {
        throw "Startup Scene Material is missing or has the wrong type: $materialId"
    }
    [void]$selected.Add($materialId)
    $textureId = $assets[$materialId].TextureId
    if ($textureId)
    {
        if (-not $assets.ContainsKey($textureId) -or $assets[$textureId].Kind -ne 1)
        {
            throw "Material texture is missing or has the wrong type: $textureId"
        }
        [void]$selected.Add($textureId)
    }
}

# 一意な途中生成 File だけを使い、完成した Pack を原子的に公開する
$outputDirectory = [IO.Path]::GetDirectoryName($OutputFile)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$nonce = [Guid]::NewGuid().ToString('N')
$temporaryPack = Join-Path $outputDirectory ".assets-$nonce.tmp"
$workDirectory = Join-Path $outputDirectory ".assets-$nonce"
$temporaryTextures = [Collections.Generic.List[string]]::new()
try
{
    $textureTool = $null
    foreach ($id in $selected)
    {
        $asset = $assets[$id]
        if ($asset.Kind -eq 2)
        {
            $asset.Size = [uint64]$asset.Material.Length
            continue
        }
        if (-not $textureTool)
        {
            $textureTool = Get-CueTexconv
            [IO.Directory]::CreateDirectory($workDirectory) | Out-Null
        }
        # 素材名の衝突と WIC の入力規則を避けるため、作業 File の名前は永続 ID で固定する
        $inputFile = Join-Path $workDirectory "$id.png"
        $outputTexture = Join-Path $workDirectory "$id.dds"
        $temporaryTextures.Add($inputFile)
        $temporaryTextures.Add($outputTexture)
        [IO.File]::Copy($asset.Source.FullName, $inputFile, $false)
        $format = 'R8G8B8A8_UNORM'
        $gamma = '--ignore-srgb'
        if ($asset.ColorSpace -ceq 'sRGB')
        {
            $format = 'R8G8B8A8_UNORM_SRGB'
            $gamma = '-srgb'
        }
        $process = & $textureTool -nologo -ft DDS -dx10 -m 1 -f $format $gamma -o $workDirectory -- $inputFile
        if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($outputTexture))
        {
            throw "Texture cook failed: $($asset.Source.FullName)`n$($process -join "`n")"
        }
        $asset.Cooked = [IO.FileInfo]::new($outputTexture)
        $asset.Size = [uint64]$asset.Cooked.Length
        if ($asset.Size -le 148 -or $asset.Size -gt (148 + 16384L * 16384 * 4))
        {
            throw "Cooked DDS has an invalid size: $($asset.Source.FullName)"
        }
    }

    $stream = [IO.FileStream]::new($temporaryPack, [IO.FileMode]::CreateNew)
    try
    {
        $writer = [IO.BinaryWriter]::new($stream, $utf8, $true)
        try
        {
            # AssetPack.cpp と共通の契約。索引を Little Endian で書き、Data は DDS のまま連結する
            $writer.Write([Text.Encoding]::ASCII.GetBytes('CUEPACK1'))
            $writer.Write([uint32]$selected.Count)
            $writer.Write([uint32]0)
            [uint64]$offset = 16 + 56L * $selected.Count
            foreach ($id in $selected)
            {
                $asset = $assets[$id]
                $writer.Write([Text.Encoding]::ASCII.GetBytes($id))
                $writer.Write($asset.Kind)
                $writer.Write($offset)
                $writer.Write($asset.Size)
                $offset += $asset.Size
            }
            foreach ($id in $selected)
            {
                $asset = $assets[$id]
                if ($asset.Kind -eq 2) { $writer.Write($asset.Material); continue }
                $inputStream = $asset.Cooked.OpenRead()
                try { $inputStream.CopyTo($stream) }
                finally { $inputStream.Dispose() }
            }
            $writer.Flush()
        }
        finally { $writer.Dispose() }
        $stream.Flush($true)
    }
    finally { $stream.Dispose() }
    if (-not [IO.File]::Exists($OutputFile) -or
        (Get-FileHash -LiteralPath $temporaryPack -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $OutputFile -Algorithm SHA256).Hash)
    {
        [IO.File]::Move($temporaryPack, $OutputFile, $true)
    }
}
finally
{
    foreach ($file in $temporaryTextures) { [IO.File]::Delete($file) }
    if ([IO.Directory]::Exists($workDirectory)) { [IO.Directory]::Delete($workDirectory, $false) }
    [IO.File]::Delete($temporaryPack)
}
