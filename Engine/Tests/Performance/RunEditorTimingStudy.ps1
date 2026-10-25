param(
    [string]$Executable = 'out/build/windows-vs2026-performance/Study/Development/CueEditorTimingProbe.exe',
    [string]$OutputDirectory = 'out/validation/M06-PerformanceStudy/Runs',
    [int]$Repeats = 3,
    [int]$Warmup = 120,
    [int]$Frames = 600,
    [string]$BaselineCase = 'baseline',
    [switch]$CommandBreakdown,
    [switch]$ImprovementExperiments,
    [string[]]$Cases = @()
)

$ErrorActionPreference = 'Stop'
$taskExecutable = (Resolve-Path -LiteralPath $Executable).Path
$taskOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($taskOutput) | Out-Null

# 個人情報を収集せず、性能値の解釈に必要な OS / CPU / GPU Driver だけ記録する
$taskEnvironment = [ordered]@{
    os = @(Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    gpu_inventory = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate)
    timestamp_utc = [DateTime]::UtcNow.ToString('o')
    executable = $taskExecutable
    executable_sha256 = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash
    note = 'Visible window; warmup excluded; no minimized run; GUI occlusion and power state not controlled'
}
$taskEnvironment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'Environment.json') -Encoding utf8

# 基準条件を各反復の前後に置き、条件の順序も反転して時間経過の偏りを確認する
$taskGeneratedCsv = [System.Collections.Generic.List[string]]::new()
for ($taskRepeat = 1; $taskRepeat -le $Repeats; ++$taskRepeat)
{
    $taskIsEnd = $false
    $taskCases = @('readout15', 'minimal', 'minimal-readout15', 'minimal-gpuoff', 'minimal-pixoff', 'tearing')
    if ($ImprovementExperiments)
    {
        # 同じ詳細 UI と描画内容で、個別の変更と組合せを比較する
        $taskCases = @('statunlock', 'freqcache', 'nthstats', 'gpustats15', 'readout15', 'combined', 'combined-readout15')
    }
    if ($Cases.Count -gt 0)
    {
        # Reverse が入力配列を変更し、次の反復まで順序を持ち越すことを防ぐ
        $taskCases = @($Cases)
    }
    if ($taskRepeat % 2 -eq 0)
    {
        [array]::Reverse($taskCases)
    }
    $taskOrderedCases = if ($CommandBreakdown) { @($BaselineCase) } else { @($BaselineCase) + $taskCases + @($BaselineCase) }
    foreach ($taskCase in $taskOrderedCases)
    {
        $taskPhase = if ($taskCase -eq $BaselineCase -and $taskIsEnd) { 'after' } else { 'before' }
        if ($taskCase -ne $BaselineCase)
        {
            $taskPhase = 'case'
        }
        $taskCsv = Join-Path $taskOutput "$taskCase-$taskRepeat-$taskPhase.csv"
        Write-Output "Measuring $taskCase, repeat $taskRepeat, $Warmup warmup + $Frames frames"
        # Native GPU 待機で Probe 内の期限を確認できない場合も、自分が起動した Process だけ停止する
        $taskProbe = Start-Process -FilePath $taskExecutable -WindowStyle Hidden -PassThru -ArgumentList @(
            '--case', $taskCase, '--warmup', $Warmup, '--frames', $Frames, '--repeat', $taskRepeat,
            '--output', ('"' + $taskCsv + '"')
        ) -RedirectStandardOutput ($taskCsv + '.stdout.log') -RedirectStandardError ($taskCsv + '.stderr.log')
        if (!$taskProbe.WaitForExit(45000))
        {
            $taskProbe.Kill()
            $taskProbe.WaitForExit()
            throw "Measurement timed out: $taskCase"
        }
        Get-Content -LiteralPath ($taskCsv + '.stdout.log')
        if ($taskProbe.ExitCode -ne 0)
        {
            Get-Content -LiteralPath ($taskCsv + '.stderr.log')
            throw "Measurement failed: $taskCase, exit $($taskProbe.ExitCode)"
        }
        $taskGeneratedCsv.Add($taskCsv)
        $taskIsEnd = $true
    }
    $taskIsEnd = $false
}

# 数値の集計は全 Process 停止後にだけ実施し、測定中の Frame に File IO や Sort を入れない
$taskRows = foreach ($taskCsv in $taskGeneratedCsv)
{
    # 同じ出力先に残る過去の別条件を今回の集計へ混ぜない
    $taskFile = Get-Item -LiteralPath $taskCsv
    foreach ($taskGroup in (Import-Csv -LiteralPath $taskFile.FullName | Group-Object metric))
    {
        $taskValues = @($taskGroup.Group | ForEach-Object { [double]$_.nanoseconds / 1000000.0 } | Sort-Object)
        if ($taskValues.Count -eq 0)
        {
            continue
        }
        $taskStats = $taskValues | Measure-Object -Average -Maximum
        [pscustomobject]@{
            run = $taskFile.BaseName
            metric = $taskGroup.Name
            count = $taskValues.Count
            avg_ms = $taskStats.Average
            p95_ms = $taskValues[[int][Math]::Ceiling(0.95 * $taskValues.Count) - 1]
            max_ms = $taskStats.Maximum
        }
    }
}
$taskRows | Export-Csv -LiteralPath (Join-Path $taskOutput 'Summary.csv') -NoTypeInformation -Encoding utf8
$taskRows | Where-Object metric -In @('Render', 'Present', 'GraphRecord', 'Readout', 'ImGuiRecordWait') | Format-Table -AutoSize
