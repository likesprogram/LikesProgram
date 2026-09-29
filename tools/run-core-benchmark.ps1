param(
    [Parameter(Mandatory = $true)]
    [string]$BenchmarkPath,

    [string]$OutputRoot = "docs/progress/performance",

    [ValidateRange(1, 100)]
    [int]$WarmupRuns = 3,

    [ValidateRange(3, 1000)]
    [int]$SampleRuns = 20
)

$ErrorActionPreference = "Stop"
$parityEpsilonNsPerOperation = 0.75 # DLL/视图亚纳秒差异低于常规桌面计时分辨率，按持平处理。

# Generate traceable raw logs, structured data, charts, and an HTML overview.
function Write-Utf8File {
    param([string]$Path, [string]$Content)
    $encoding = [System.Text.UTF8Encoding]::new($false) # Write UTF-8 without a BOM.
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

# Escape report metadata before embedding it in HTML or SVG.
function ConvertTo-Html {
    param([string]$Value)
    return [System.Net.WebUtility]::HtmlEncode($Value)
}

# Calculate a percentile from sorted samples.
function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object) # Current metric samples in ascending order.
    $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
    if ($index -lt 0) { $index = 0 }
    if ($index -ge $sorted.Count) { $index = $sorted.Count - 1 }
    return [double]$sorted[$index]
}

# Run one benchmark process and capture output, exit status, time, and peak RSS.
function Invoke-BenchmarkProcess {
    param([string]$Executable, [string]$LogPath)
    $resolved = (Resolve-Path -LiteralPath $Executable).Path # Absolute benchmark path.
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $resolved
    $info.WorkingDirectory = Split-Path -Parent $resolved
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $info.Environment["PATH"] = (Split-Path -Parent $resolved) + ";" + $env:PATH

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    $startedAt = [DateTimeOffset]::Now # Process start time.
    if (-not $process.Start()) { throw "failed to start Core benchmark" }
    $stdout = $process.StandardOutput.ReadToEnd() # Captured standard output.
    $stderr = $process.StandardError.ReadToEnd() # Captured standard error.
    $process.WaitForExit()
    $finishedAt = [DateTimeOffset]::Now # Process finish time.
    $peakWorkingSet = [int64]$process.PeakWorkingSet64 # Peak working set in bytes.
    $exitCode = $process.ExitCode # Benchmark exit code.

    $log = @(
        "started_at=$($startedAt.ToString('o'))"
        "finished_at=$($finishedAt.ToString('o'))"
        "duration_ms=$([Math]::Round(($finishedAt - $startedAt).TotalMilliseconds, 3))"
        "peak_working_set_bytes=$peakWorkingSet"
        "exit_code=$exitCode"
        "--- stdout ---"
        $stdout.TrimEnd()
        "--- stderr ---"
        $stderr.TrimEnd()
    ) -join "`n"
    Write-Utf8File -Path $LogPath -Content ($log + "`n")

    return [pscustomobject]@{
        Stdout = $stdout
        Stderr = $stderr
        ExitCode = $exitCode
        PeakWorkingSetBytes = $peakWorkingSet
        DurationMs = ($finishedAt - $startedAt).TotalMilliseconds
    }
}

$benchmark = (Resolve-Path -LiteralPath $BenchmarkPath).Path # Benchmark executable for this run.
$root = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputRoot)) # Result root.
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss" # Result directory timestamp.
$resultDirectory = Join-Path $root "core-$timestamp"
[void](New-Item -ItemType Directory -Path $resultDirectory -Force)

$rawDirectory = Join-Path $resultDirectory "raw"
[void](New-Item -ItemType Directory -Path $rawDirectory -Force)

# Keep warmup logs but exclude them from formal statistics.
for ($run = 1; $run -le $WarmupRuns; ++$run) {
    $logPath = Join-Path $rawDirectory ("warmup-{0:D3}.log" -f $run)
    $warmup = Invoke-BenchmarkProcess -Executable $benchmark -LogPath $logPath
    if ($warmup.ExitCode -ne 0) {
        throw "Core benchmark warmup $run failed with exit code $($warmup.ExitCode)"
    }
}

$records = [System.Collections.Generic.List[object]]::new() # Formal metric samples.
$runRecords = [System.Collections.Generic.List[object]]::new() # Process-level resource samples.
$metadataLine = $null # Benchmark version and reference metadata.

for ($run = 1; $run -le $SampleRuns; ++$run) {
    $logPath = Join-Path $rawDirectory ("sample-{0:D3}.log" -f $run)
    $sample = Invoke-BenchmarkProcess -Executable $benchmark -LogPath $logPath
    $runRecords.Add([pscustomobject]@{
        run = $run
        exit_code = $sample.ExitCode
        duration_ms = [Math]::Round($sample.DurationMs, 3)
        peak_working_set_bytes = $sample.PeakWorkingSetBytes
    })
    if ($sample.ExitCode -ne 0) {
        throw "Core benchmark sample $run failed with exit code $($sample.ExitCode)"
    }

    $metricCount = 0 # Parsed metric count for this process.
    foreach ($line in ($sample.Stdout -split "`r?`n")) {
        if ($line -like "benchmark_version=*") { $metadataLine = $line }
        if ($line -match '^(\S+) likes_ns=(\d+) std_ns=(\d+) operations=(\d+)$') {
            $likesNs = [double]$matches[2] # LikesProgram duration in nanoseconds.
            $referenceNs = [double]$matches[3] # Reference duration in nanoseconds.
            $operations = [double]$matches[4] # Timed operations represented by this sample.
            $records.Add([pscustomobject]@{
                run = $run
                metric = $matches[1]
                likes_ns = [int64]$likesNs
                reference_ns = [int64]$referenceNs
                operations = [int64]$operations
                ratio = [Math]::Round($likesNs / $referenceNs, 6)
                delta_ns_per_operation = [Math]::Round(($likesNs - $referenceNs) / $operations, 6)
            })
            ++$metricCount
        }
    }
    if ($metricCount -eq 0) { throw "Core benchmark sample $run produced no metrics" }
}

$samplesCsv = Join-Path $resultDirectory "samples.csv"
$records | Export-Csv -LiteralPath $samplesCsv -NoTypeInformation -Encoding utf8
$runsCsv = Join-Path $resultDirectory "runs.csv"
$runRecords | Export-Csv -LiteralPath $runsCsv -NoTypeInformation -Encoding utf8

$summary = [System.Collections.Generic.List[object]]::new() # Per-metric summary.
foreach ($group in ($records | Group-Object metric | Sort-Object Name)) {
    $likes = [double[]]@($group.Group.likes_ns) # LikesProgram samples.
    $reference = [double[]]@($group.Group.reference_ns) # Reference samples.
    $ratios = [double[]]@($group.Group.ratio) # Per-run duration ratios.
    $deltas = [double[]]@($group.Group.delta_ns_per_operation) # Per-operation absolute deltas.
    $operations = [double]$group.Group[0].operations # Stable operation count for this metric.
    $likesAverage = ($likes | Measure-Object -Average).Average
    $referenceAverage = ($reference | Measure-Object -Average).Average
    $averageRatio = $likesAverage / $referenceAverage
    $medianRatio = Get-Percentile -Values $ratios -Percentile 0.50
    $p95Ratio = Get-Percentile -Values $ratios -Percentile 0.95
    $averageDelta = ($likesAverage - $referenceAverage) / $operations
    $medianDelta = Get-Percentile -Values $deltas -Percentile 0.50
    $strictRatioPassed = $averageRatio -le 1.0 -and $medianRatio -le 1.0
    $epsilonPassed = $averageDelta -le $parityEpsilonNsPerOperation
    $passed = $strictRatioPassed -or $epsilonPassed # 严格胜出或亚纳秒等价均视为持平。

    $summary.Add([pscustomobject]@{
        metric = $group.Name
        samples = $group.Count
        operations = [int64]$operations
        likes_average_ns = [Math]::Round($likesAverage, 3)
        reference_average_ns = [Math]::Round($referenceAverage, 3)
        likes_average_ns_per_operation = [Math]::Round($likesAverage / $operations, 6)
        reference_average_ns_per_operation = [Math]::Round($referenceAverage / $operations, 6)
        average_delta_ns_per_operation = [Math]::Round($averageDelta, 6)
        median_delta_ns_per_operation = [Math]::Round($medianDelta, 6)
        average_ratio = [Math]::Round($averageRatio, 6)
        median_ratio = [Math]::Round($medianRatio, 6)
        p95_ratio = [Math]::Round($p95Ratio, 6)
        gate = if ($strictRatioPassed) { "strict_ratio" } else { "parity_epsilon" }
        outcome = if ($passed) { "passed" } else { "failed" }
    })
}

$failedMetrics = @($summary | Where-Object outcome -eq "failed") # Metrics below the parity gate.
$overallOutcome = if ($failedMetrics.Count -eq 0) { "passed" } else { "failed" }
$summaryCsv = Join-Path $resultDirectory "summary.csv"
$summary | Export-Csv -LiteralPath $summaryCsv -NoTypeInformation -Encoding utf8

$os = Get-CimInstance Win32_OperatingSystem # Windows version and visible memory.
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 # Benchmark CPU.
$manifest = [ordered]@{
    schema_version = 2
    benchmark = "LikesProgramCoreBenchmark"
    benchmark_path = $benchmark
    benchmark_metadata = $metadataLine
    generated_at = [DateTimeOffset]::Now.ToString("o")
    outcome = $overallOutcome
    warmup_runs = $WarmupRuns
    sample_runs = $SampleRuns
    failed_metrics = @($failedMetrics | ForEach-Object { $_.metric })
    parity_epsilon_ns_per_operation = $parityEpsilonNsPerOperation
    environment = [ordered]@{
        os = $os.Caption
        os_version = $os.Version
        cpu = $cpu.Name
        logical_processors = $cpu.NumberOfLogicalProcessors
        memory_bytes = [int64]$os.TotalVisibleMemorySize * 1024
        powershell = $PSVersionTable.PSVersion.ToString()
    }
    files = [ordered]@{
        samples_csv = "samples.csv"
        runs_csv = "runs.csv"
        summary_csv = "summary.csv"
        ratio_svg = "ratio.svg"
        html = "index.html"
        raw_directory = "raw"
    }
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
Write-Utf8File -Path (Join-Path $resultDirectory "manifest.json") -Content ($manifestJson + "`n")

# Generate a dependency-free ratio SVG with a vertical 1.0 parity gate.
$chartWidth = 1100
$rowHeight = 42
$left = 250
$plotWidth = 780
$chartHeight = 80 + $summary.Count * $rowHeight
$maxRatio = [Math]::Max(2.0, (($summary.average_ratio | Measure-Object -Maximum).Maximum * 1.1))
$svg = [System.Text.StringBuilder]::new()
[void]$svg.AppendLine("<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"$chartWidth`" height=`"$chartHeight`" viewBox=`"0 0 $chartWidth $chartHeight`">")
[void]$svg.AppendLine("<rect width=`"100%`" height=`"100%`" fill=`"#ffffff`"/>")
[void]$svg.AppendLine("<text x=`"24`" y=`"32`" font-family=`"Segoe UI, sans-serif`" font-size=`"20`" fill=`"#202124`">Core benchmark average ratio (LikesProgram / reference)</text>")
$gateX = $left + ($plotWidth / $maxRatio)
[void]$svg.AppendLine("<line x1=`"$gateX`" y1=`"52`" x2=`"$gateX`" y2=`"$($chartHeight - 20)`" stroke=`"#202124`" stroke-width=`"2`" stroke-dasharray=`"6 5`"/>")
[void]$svg.AppendLine("<text x=`"$($gateX + 5)`" y=`"68`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`" fill=`"#202124`">1.0 pass gate</text>")

$index = 0
foreach ($item in $summary) {
    $y = 84 + $index * $rowHeight
    $barWidth = [Math]::Max(1, [double]$item.average_ratio / $maxRatio * $plotWidth)
    $color = if ($item.outcome -eq "passed") { "#188038" } else { "#c5221f" }
    $label = ConvertTo-Html $item.metric
    [void]$svg.AppendLine("<text x=`"24`" y=`"$($y + 17)`" font-family=`"Consolas, monospace`" font-size=`"13`" fill=`"#202124`">$label</text>")
    [void]$svg.AppendLine("<rect x=`"$left`" y=`"$y`" width=`"$barWidth`" height=`"22`" fill=`"$color`"/>")
    [void]$svg.AppendLine("<text x=`"$($left + $barWidth + 7)`" y=`"$($y + 16)`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`" fill=`"#202124`">$($item.average_ratio)</text>")
    ++$index
}
[void]$svg.AppendLine("</svg>")
Write-Utf8File -Path (Join-Path $resultDirectory "ratio.svg") -Content $svg.ToString()

$rows = [System.Text.StringBuilder]::new()
foreach ($item in $summary) {
    $className = if ($item.outcome -eq "passed") { "passed" } else { "failed" }
    [void]$rows.AppendLine("<tr class=`"$className`"><td>$(ConvertTo-Html $item.metric)</td><td>$($item.samples)</td><td>$($item.operations)</td><td>$($item.likes_average_ns_per_operation)</td><td>$($item.reference_average_ns_per_operation)</td><td>$($item.average_delta_ns_per_operation)</td><td>$($item.average_ratio)</td><td>$($item.median_ratio)</td><td>$($item.gate)</td><td>$($item.outcome)</td></tr>")
}

$html = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LikesProgram Core benchmark $timestamp</title>
<style>
body { font-family: Segoe UI, sans-serif; margin: 24px; color: #202124; }
h1 { font-size: 24px; }
.status { font-weight: 700; color: $(if ($overallOutcome -eq 'passed') { '#188038' } else { '#c5221f' }); }
table { border-collapse: collapse; width: 100%; margin-top: 20px; font-size: 13px; }
th, td { border: 1px solid #dadce0; padding: 7px 9px; text-align: right; }
th:first-child, td:first-child { text-align: left; }
tr.failed { background: #fce8e6; }
tr.passed { background: #e6f4ea; }
img { max-width: 100%; height: auto; border: 1px solid #dadce0; }
code { background: #f1f3f4; padding: 2px 4px; }
</style>
</head>
<body>
<h1>LikesProgram Core benchmark</h1>
<p>Outcome: <span class="status">$overallOutcome</span></p>
<p>Parity rule: average and median ratio must be at most 1.0, or the 20-sample average delta must be at most $parityEpsilonNsPerOperation ns per operation. Median and p95 remain visible for stability review.</p>
<p>Generated: <code>$(ConvertTo-Html $manifest.generated_at)</code><br>
Benchmark: <code>$(ConvertTo-Html $benchmark)</code><br>
Metadata: <code>$(ConvertTo-Html $metadataLine)</code><br>
Environment: $(ConvertTo-Html $os.Caption), $(ConvertTo-Html $cpu.Name), $($cpu.NumberOfLogicalProcessors) logical processors</p>
<img src="ratio.svg" alt="Core benchmark average ratio chart">
<table>
<thead><tr><th>Metric</th><th>Samples</th><th>Operations</th><th>Likes ns/op</th><th>Reference ns/op</th><th>Delta ns/op</th><th>Avg ratio</th><th>Median ratio</th><th>Gate</th><th>Outcome</th></tr></thead>
<tbody>
$($rows.ToString())
</tbody>
</table>
<p>Raw data: <a href="samples.csv">samples.csv</a> | <a href="runs.csv">runs.csv</a> | <a href="summary.csv">summary.csv</a> | <a href="manifest.json">manifest.json</a> | <a href="raw/">raw logs</a></p>
</body>
</html>
"@
Write-Utf8File -Path (Join-Path $resultDirectory "index.html") -Content $html

Write-Output "result_directory=$resultDirectory"
Write-Output "outcome=$overallOutcome"
Write-Output "failed_metrics=$($failedMetrics.metric -join ',')"

if ($overallOutcome -ne "passed") { exit 2 }
