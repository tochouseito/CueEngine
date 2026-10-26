param(
    [string]$Executable = 'out/build/windows-vs2026-performance/Study/Development/CueEditorTimingProbe.exe',
    [string]$PresentMon = 'out/tools/PresentMon/PresentMon-2.6.0-x64.exe',
    [string]$OutputDirectory = 'out/validation/M06-PerformanceStudy/PresentationPaths',
    [Parameter(Mandatory = $true)]
    [string[]]$Cases,
    [int]$Repeats = 3,
    [int]$Warmup = 120,
    [int]$Frames = 600
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$taskExecutable = (Resolve-Path -LiteralPath $Executable).Path
$taskPresentMon = (Resolve-Path -LiteralPath $PresentMon).Path
$taskOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$taskCaptureRoot = [System.IO.Path]::GetFullPath((Join-Path $taskRoot 'out/validation/M06-PerformanceStudy/PresentationPaths'))

# Capture は指定済み Directory 内、対象は CueEngine の研究用 Executable に限定する
if ([System.IO.Path]::GetFileName($taskExecutable) -ne 'CueEditorTimingProbe.exe' -or
    (!$taskOutput.Equals($taskCaptureRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
     !$taskOutput.StartsWith($taskCaptureRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)))
{
    throw 'Capture target or output directory is outside the CueEngine study boundary'
}
if ($Repeats -lt 1 -or $Repeats -gt 10 -or $Frames -lt 1 -or $Warmup -lt 0 -or $Warmup + $Frames -gt 1800)
{
    throw 'Invalid measurement bounds'
}
if (Get-Process -Name 'CueEditorTimingProbe' -ErrorAction SilentlyContinue)
{
    throw 'Another CueEditorTimingProbe is running; name filtering would mix processes'
}
[System.IO.Directory]::CreateDirectory($taskOutput) | Out-Null

# この Capture を使った値は、ETW を付けない通常比較とは別に扱う
[ordered]@{
    timestamp_utc = [DateTime]::UtcNow.ToString('o')
    executable = $taskExecutable
    executable_sha256 = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash
    presentmon = $taskPresentMon
    presentmon_sha256 = (Get-FileHash -LiteralPath $taskPresentMon -Algorithm SHA256).Hash
    repeats = $Repeats
    warmup = $Warmup
    frames = $Frames
    cases = $Cases
    note = 'Each PresentMon CSV includes startup and warmup. ETW measurement overhead is present; no input latency instrumentation. Capture contains only CueEditorTimingProbe output.'
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskOutput 'Environment.json') -Encoding utf8

for ($taskRepeat = 1; $taskRepeat -le $Repeats; ++$taskRepeat)
{
    $taskCases = @($Cases)
    if ($taskRepeat % 2 -eq 0)
    {
        [array]::Reverse($taskCases)
    }
    foreach ($taskCase in $taskCases)
    {
        if ($taskCase -notmatch '^[a-zA-Z0-9-]+$')
        {
            throw 'Invalid case label'
        }
        $taskStem = Join-Path $taskOutput "$taskCase-$taskRepeat"
        if ((Test-Path -LiteralPath ($taskStem + '.presentmon.csv')) -or (Test-Path -LiteralPath ($taskStem + '.ready')))
        {
            throw "Capture already exists; select a new output subdirectory: $taskStem"
        }
        $taskSession = 'CueEngineStudy-' + [Guid]::NewGuid().ToString('N')
        $taskCapture = $null
        $taskProbe = $null
        try
        {
            # Host 初期化前に PID を取得し、名前照会権限に依存せず CueEngine だけを追跡する
            $taskProbeArguments = @('--case', $taskCase, '--warmup', $Warmup, '--frames', $Frames, '--repeat', $taskRepeat,
                '--output', ('"' + $taskStem + '.engine.csv"'), '--capture-ready', ('"' + $taskStem + '.ready"'))
            $taskProbe = Start-Process -FilePath $taskExecutable -ArgumentList $taskProbeArguments -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput ($taskStem + '.engine.stdout.log') -RedirectStandardError ($taskStem + '.engine.stderr.log')
            $taskArguments = @('--process_id', $taskProbe.Id, '--output_file', ('"' + $taskStem + '.presentmon.csv"'),
                '--session_name', $taskSession, '--timed', '40', '--terminate_after_timed', '--terminate_on_proc_exit',
                '--no_console_stats', '--no_track_input', '--write_display_metadata', '--track_etw_status')
            $taskCapture = Start-Process -FilePath $taskPresentMon -ArgumentList $taskArguments -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput ($taskStem + '.presentmon.stdout.log') -RedirectStandardError ($taskStem + '.presentmon.stderr.log')
            $taskReady = $false
            $taskDeadline = [DateTime]::UtcNow.AddSeconds(5)
            while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskCapture.HasExited)
            {
                & "$env:SystemRoot/System32/logman.exe" query $taskSession -ets *> ($taskStem + '.session.log')
                if ($LASTEXITCODE -eq 0)
                {
                    $taskReady = $true
                    break
                }
                Start-Sleep -Milliseconds 100
                $taskCapture.Refresh()
            }
            if (!$taskReady)
            {
                throw "PresentMon session failed to start; inspect $taskStem.presentmon.stderr.log"
            }
            Write-Output "Capturing presentation path: $taskCase, repeat $taskRepeat"
            [System.IO.File]::WriteAllText($taskStem + '.ready', 'ready')
            if (!$taskProbe.WaitForExit(40000))
            {
                throw 'CueEditorTimingProbe exceeded its bounded measurement time'
            }
            if ($taskProbe.ExitCode -ne 0)
            {
                throw "CueEditorTimingProbe failed with exit $($taskProbe.ExitCode)"
            }
            # 名前照会権限がない環境では Process 終了通知を取れないため、自分の ETW Session を明示停止する
            & $taskPresentMon --session_name $taskSession --terminate_existing_session *> ($taskStem + '.stop.log')
            if (!$taskCapture.WaitForExit(10000))
            {
                throw 'PresentMon did not flush and stop after target exit'
            }
            if ($taskCapture.ExitCode -ne 0)
            {
                throw "PresentMon failed with exit $($taskCapture.ExitCode)"
            }
            [ordered]@{ case = $taskCase; repeat = $taskRepeat; probe_pid = $taskProbe.Id; session = $taskSession;
                probe_exit = $taskProbe.ExitCode; presentmon_exit = $taskCapture.ExitCode } |
                ConvertTo-Json | Set-Content -LiteralPath ($taskStem + '.capture.json') -Encoding utf8
        }
        finally
        {
            # 失敗時も、今回作成した固有 Session と Process だけを閉じる
            if ($taskProbe -and !$taskProbe.HasExited)
            {
                $taskProbe.Kill()
                $taskProbe.WaitForExit()
            }
            if ($taskCapture -and !$taskCapture.HasExited)
            {
                & $taskPresentMon --session_name $taskSession --terminate_existing_session *> ($taskStem + '.cleanup.log')
                if (!$taskCapture.WaitForExit(5000))
                {
                    $taskCapture.Kill()
                    $taskCapture.WaitForExit()
                }
            }
        }
    }
}
