param(
    [Parameter(Mandatory = $true)]
    [string]$Benchmark,

    [string]$OutputRoot = "docs/progress/performance",

    [ValidateRange(1, 100)]
    [int]$Repeats = 5
)

$ErrorActionPreference = "Stop"

# Escape XML/HTML special characters in generated reports.
function ConvertTo-HtmlText {
    param([string]$Value)
    [System.Net.WebUtility]::HtmlEncode($Value)
}

# Write generated files as UTF-8 without BOM.
function Write-Utf8File {
    param(
        [string]$Path,
        [string]$Content
    )
    $encoding = New-Object System.Text.UTF8Encoding($false) # UTF-8 without BOM
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

# Keep protocol workloads and lower-bound references visually distinct.
function Get-SeriesColor {
    param([string]$Name)
    $colors = @{
        "http1_build_parse" = "#006d77"
        "http2_build_parse" = "#d1495b"
        "http3_build_parse" = "#edae49"
        "session_send" = "#3d5a80"
        "http1_std_string_copy" = "#59a14f"
        "http2_std_vector_copy" = "#8cd17d"
        "http3_std_vector_copy" = "#b6992d"
        "session_direct_transport" = "#9c755f"
    }

    if ($colors.ContainsKey($Name)) {
        $colors[$Name]
    }
    else {
        "#6b7280"
    }
}

$benchmarkPath = (Resolve-Path -LiteralPath $Benchmark).Path
$outputRootPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputRoot))
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$resultDirectory = Join-Path $outputRootPath "http-codec-$timestamp"
[System.IO.Directory]::CreateDirectory($resultDirectory) | Out-Null

$measurements = New-Object 'System.Collections.Generic.List[object]'
$resources = New-Object 'System.Collections.Generic.List[object]'
$linePattern = '^name=([^ ]+) reference=([^ ]+) iterations=([0-9]+) bytes_per_iteration=([0-9]+) total_ns=([0-9]+) ns_per_operation=([0-9.]+) operations_per_second=([0-9.]+) mib_per_second=([0-9.]+) checksum=([0-9]+)$'

for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $benchmarkPath
    $startInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($benchmarkPath)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw "failed to start benchmark" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $peakWorkingSetBytes = 0L # Highest sampled working set for this repeat.
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            $peakWorkingSetBytes = [Math]::Max($peakWorkingSetBytes, $process.WorkingSet64)
        }
        catch {
            # The process may exit between HasExited and Refresh.
        }
        Start-Sleep -Milliseconds 10
    }
    $process.WaitForExit()
    $stopwatch.Stop()

    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $exitCode = $process.ExitCode

    $rawLog = "stdout:`n$stdout`nstderr:`n$stderr"
    Write-Utf8File (Join-Path $resultDirectory ("benchmark-run-{0}.log" -f $repeat)) $rawLog

    $resources.Add([PSCustomObject]@{
        Repeat = $repeat
        ExitCode = $exitCode
        ElapsedMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
        CpuMs = [Math]::Round($process.TotalProcessorTime.TotalMilliseconds, 3)
        PeakWorkingSetBytes = $peakWorkingSetBytes
    })

    if ($exitCode -ne 0) {
        throw "benchmark repeat $repeat failed with exit code $exitCode"
    }
    if ($stdout -notmatch '(?m)^build_type=release\r?$') {
        throw "benchmark repeat $repeat is not a Release build"
    }

    foreach ($line in ($stdout -split '\r?\n')) {
        if ($line -notmatch $linePattern) {
            continue
        }

        $measurements.Add([PSCustomObject]@{
            Repeat = $repeat
            Name = $Matches[1]
            Reference = $Matches[2]
            Iterations = [UInt64]$Matches[3]
            BytesPerIteration = [UInt64]$Matches[4]
            TotalNs = [Int64]$Matches[5]
            NsPerOperation = [Double]$Matches[6]
            OperationsPerSecond = [Double]$Matches[7]
            MiBPerSecond = [Double]$Matches[8]
            Checksum = [UInt64]$Matches[9]
        })
    }
}

if ($measurements.Count -eq 0) { throw "benchmark produced no measurement rows" }

$measurements | Export-Csv -LiteralPath (Join-Path $resultDirectory "results.csv") -NoTypeInformation -Encoding utf8
$resources | Export-Csv -LiteralPath (Join-Path $resultDirectory "resources.csv") -NoTypeInformation -Encoding utf8

$summary = foreach ($group in ($measurements | Group-Object Name | Sort-Object Name)) {
    $ops = $group.Group.OperationsPerSecond | Measure-Object -Minimum -Maximum -Average
    $latency = $group.Group.NsPerOperation | Measure-Object -Minimum -Maximum -Average
    $mib = $group.Group.MiBPerSecond | Measure-Object -Minimum -Maximum -Average
    [PSCustomObject]@{
        Name = $group.Name
        Reference = $group.Group[0].Reference
        Repeats = $group.Count
        MinOperationsPerSecond = [Math]::Round($ops.Minimum, 3)
        AverageOperationsPerSecond = [Math]::Round($ops.Average, 3)
        MaxOperationsPerSecond = [Math]::Round($ops.Maximum, 3)
        AverageNsPerOperation = [Math]::Round($latency.Average, 3)
        AverageMiBPerSecond = [Math]::Round($mib.Average, 3)
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $resultDirectory "summary.csv") -NoTypeInformation -Encoding utf8

$gitHead = (& git rev-parse HEAD).Trim()
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$computer = Get-CimInstance Win32_ComputerSystem
$metadata = [ordered]@{
    Timestamp = (Get-Date).ToString("o")
    GitHead = $gitHead
    Benchmark = $benchmarkPath
    Repeats = $Repeats
    OperatingSystem = $os.Caption
    OperatingSystemVersion = $os.Version
    Cpu = $cpu.Name.Trim()
    LogicalProcessors = $computer.NumberOfLogicalProcessors
    MemoryBytes = [UInt64]$computer.TotalPhysicalMemory
    PowerShell = $PSVersionTable.PSVersion.ToString()
    ResourceSampleIntervalMs = 10
    ComparisonNote = "Memory copies and direct dispatch are lower bounds, not full HTTP competitors."
}
Write-Utf8File (Join-Path $resultDirectory "environment.json") ($metadata | ConvertTo-Json -Depth 4)

# Generate the average throughput chart with operations/s on the x-axis.
$plotWidth = 760
$labelWidth = 190
$barHeight = 22
$rowHeight = 38
$chartHeight = 90 + $summary.Count * $rowHeight
$maxOps = ($summary.AverageOperationsPerSecond | Measure-Object -Maximum).Maximum
$throughputSvg = New-Object System.Text.StringBuilder
[void]$throughputSvg.AppendLine("<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"1100`" height=`"$chartHeight`" viewBox=`"0 0 1100 $chartHeight`">")
[void]$throughputSvg.AppendLine("<rect width=`"100%`" height=`"100%`" fill=`"#ffffff`"/>")
[void]$throughputSvg.AppendLine("<text x=`"20`" y=`"30`" font-family=`"Segoe UI, sans-serif`" font-size=`"18`" font-weight=`"600`">HTTP codec average throughput</text>")
[void]$throughputSvg.AppendLine("<text x=`"$labelWidth`" y=`"56`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">x-axis: operations per second</text>")

$rowIndex = 0
foreach ($row in $summary) {
    $y = 72 + $rowIndex * $rowHeight
    $width = if ($maxOps -gt 0) { [Math]::Round($plotWidth * $row.AverageOperationsPerSecond / $maxOps, 2) } else { 0 }
    $color = Get-SeriesColor $row.Name
    $name = ConvertTo-HtmlText $row.Name
    $value = "{0:N0}" -f $row.AverageOperationsPerSecond
    [void]$throughputSvg.AppendLine("<text x=`"20`" y=`"$($y + 16)`" font-family=`"Consolas, monospace`" font-size=`"12`">$name</text>")
    [void]$throughputSvg.AppendLine("<rect x=`"$labelWidth`" y=`"$y`" width=`"$width`" height=`"$barHeight`" fill=`"$color`"/>")
    [void]$throughputSvg.AppendLine("<text x=`"$($labelWidth + $width + 8)`" y=`"$($y + 16)`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">$value ops/s</text>")
    ++$rowIndex
}
[void]$throughputSvg.AppendLine("</svg>")
Write-Utf8File (Join-Path $resultDirectory "throughput.svg") $throughputSvg.ToString()

# Normalize each protocol series to its first repeat for stability review.
$protocolNames = @("http1_build_parse", "http2_build_parse", "http3_build_parse", "session_send")
$stabilityWidth = 760
$stabilityHeight = 360
$plotLeft = 70
$plotTop = 50
$plotHeight = 240
$stabilitySvg = New-Object System.Text.StringBuilder
[void]$stabilitySvg.AppendLine("<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"1100`" height=`"$stabilityHeight`" viewBox=`"0 0 1100 $stabilityHeight`">")
[void]$stabilitySvg.AppendLine("<rect width=`"100%`" height=`"100%`" fill=`"#ffffff`"/>")
[void]$stabilitySvg.AppendLine("<text x=`"20`" y=`"28`" font-family=`"Segoe UI, sans-serif`" font-size=`"18`" font-weight=`"600`">Repeat stability</text>")
[void]$stabilitySvg.AppendLine("<line x1=`"$plotLeft`" y1=`"$plotTop`" x2=`"$plotLeft`" y2=`"$($plotTop + $plotHeight)`" stroke=`"#111827`"/>")
[void]$stabilitySvg.AppendLine("<line x1=`"$plotLeft`" y1=`"$($plotTop + $plotHeight)`" x2=`"$($plotLeft + $stabilityWidth)`" y2=`"$($plotTop + $plotHeight)`" stroke=`"#111827`"/>")
[void]$stabilitySvg.AppendLine("<text x=`"330`" y=`"335`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">x-axis: repeat</text>")
[void]$stabilitySvg.AppendLine("<text x=`"8`" y=`"180`" transform=`"rotate(-90 8 180)`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">normalized operations/s</text>")

$legendIndex = 0
foreach ($name in $protocolNames) {
    $rows = $measurements | Where-Object Name -eq $name | Sort-Object Repeat
    if ($rows.Count -eq 0) { continue }
    $baseline = $rows[0].OperationsPerSecond
    $points = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in $rows) {
        $x = $plotLeft + (($row.Repeat - 1) * $stabilityWidth / [Math]::Max(1, $Repeats - 1))
        $normalized = $row.OperationsPerSecond / $baseline
        $clamped = [Math]::Max(0.5, [Math]::Min(1.5, $normalized))
        $y = $plotTop + (1.5 - $clamped) * $plotHeight
        $points.Add(("{0:F2},{1:F2}" -f $x, $y))
    }
    $color = Get-SeriesColor $name
    [void]$stabilitySvg.AppendLine("<polyline points=`"$($points -join ' ')`" fill=`"none`" stroke=`"$color`" stroke-width=`"2`"/>")
    $legendY = 70 + $legendIndex * 26
    [void]$stabilitySvg.AppendLine("<line x1=`"860`" y1=`"$legendY`" x2=`"885`" y2=`"$legendY`" stroke=`"$color`" stroke-width=`"3`"/>")
    [void]$stabilitySvg.AppendLine("<text x=`"895`" y=`"$($legendY + 4)`" font-family=`"Consolas, monospace`" font-size=`"12`">$(ConvertTo-HtmlText $name)</text>")
    ++$legendIndex
}
[void]$stabilitySvg.AppendLine("</svg>")
Write-Utf8File (Join-Path $resultDirectory "stability.svg") $stabilitySvg.ToString()

# Plot peak working set per repeat to reveal short-run memory drift.
$resourceHeight = 110 + $resources.Count * 42
$maxPeak = ($resources.PeakWorkingSetBytes | Measure-Object -Maximum).Maximum
$resourceSvg = New-Object System.Text.StringBuilder
[void]$resourceSvg.AppendLine("<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"1100`" height=`"$resourceHeight`" viewBox=`"0 0 1100 $resourceHeight`">")
[void]$resourceSvg.AppendLine("<rect width=`"100%`" height=`"100%`" fill=`"#ffffff`"/>")
[void]$resourceSvg.AppendLine("<text x=`"20`" y=`"30`" font-family=`"Segoe UI, sans-serif`" font-size=`"18`" font-weight=`"600`">Benchmark peak working set</text>")
[void]$resourceSvg.AppendLine("<text x=`"150`" y=`"56`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">x-axis: MiB</text>")
foreach ($row in $resources) {
    $y = 72 + ($row.Repeat - 1) * 42
    $width = if ($maxPeak -gt 0) { [Math]::Round(760 * $row.PeakWorkingSetBytes / $maxPeak, 2) } else { 0 }
    $mib = $row.PeakWorkingSetBytes / 1MB
    [void]$resourceSvg.AppendLine("<text x=`"20`" y=`"$($y + 17)`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">repeat $($row.Repeat)</text>")
    [void]$resourceSvg.AppendLine("<rect x=`"150`" y=`"$y`" width=`"$width`" height=`"24`" fill=`"#457b9d`"/>")
    [void]$resourceSvg.AppendLine("<text x=`"$($width + 160)`" y=`"$($y + 17)`" font-family=`"Segoe UI, sans-serif`" font-size=`"12`">$("{0:F2}" -f $mib) MiB</text>")
}
[void]$resourceSvg.AppendLine("</svg>")
Write-Utf8File (Join-Path $resultDirectory "resources.svg") $resourceSvg.ToString()

$summaryRows = foreach ($row in $summary) {
    "<tr><td>$(ConvertTo-HtmlText $row.Name)</td><td>$(ConvertTo-HtmlText $row.Reference)</td><td>$($row.Repeats)</td><td>$($row.MinOperationsPerSecond)</td><td>$($row.AverageOperationsPerSecond)</td><td>$($row.MaxOperationsPerSecond)</td><td>$($row.AverageNsPerOperation)</td><td>$($row.AverageMiBPerSecond)</td></tr>"
}
$resourceRows = foreach ($row in $resources) {
    "<tr><td>$($row.Repeat)</td><td>$($row.ExitCode)</td><td>$($row.ElapsedMs)</td><td>$($row.CpuMs)</td><td>$([Math]::Round($row.PeakWorkingSetBytes / 1MB, 3))</td></tr>"
}
$rawLinks = foreach ($repeat in 1..$Repeats) {
    "<li><a href=`"benchmark-run-$repeat.log`">benchmark-run-$repeat.log</a></li>"
}

$html = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LikesProgramHttp benchmark $timestamp</title>
<style>
body { font-family: "Segoe UI", sans-serif; margin: 24px; color: #111827; }
h1, h2 { letter-spacing: 0; }
table { border-collapse: collapse; width: 100%; margin-bottom: 24px; }
th, td { border: 1px solid #d1d5db; padding: 7px 9px; text-align: right; }
th:first-child, td:first-child, th:nth-child(2), td:nth-child(2) { text-align: left; }
img { display: block; max-width: 100%; border: 1px solid #d1d5db; margin: 12px 0 24px; }
code { background: #f3f4f6; padding: 2px 4px; }
</style>
</head>
<body>
<h1>LikesProgramHttp benchmark</h1>
<p>Timestamp: <code>$timestamp</code>; Git HEAD: <code>$gitHead</code>; repeats: <code>$Repeats</code>.</p>
<p>Memory copies and direct dispatch are lower bounds, not full HTTP competitor implementations.</p>
<h2>Throughput</h2>
<img src="throughput.svg" alt="Average operations per second">
<h2>Repeat stability</h2>
<img src="stability.svg" alt="Normalized operations per second by repeat">
<h2>Resources</h2>
<img src="resources.svg" alt="Peak working set by repeat">
<h2>Summary</h2>
<table>
<thead><tr><th>Name</th><th>Reference</th><th>Repeats</th><th>Min ops/s</th><th>Average ops/s</th><th>Max ops/s</th><th>Average ns/op</th><th>Average MiB/s</th></tr></thead>
<tbody>$($summaryRows -join "`n")</tbody>
</table>
<h2>Process samples</h2>
<table>
<thead><tr><th>Repeat</th><th>Exit code</th><th>Elapsed ms</th><th>CPU ms</th><th>Peak working set MiB</th></tr></thead>
<tbody>$($resourceRows -join "`n")</tbody>
</table>
<h2>Raw data</h2>
<ul>
<li><a href="results.csv">results.csv</a></li>
<li><a href="summary.csv">summary.csv</a></li>
<li><a href="resources.csv">resources.csv</a></li>
<li><a href="environment.json">environment.json</a></li>
$($rawLinks -join "`n")
</ul>
</body>
</html>
"@
Write-Utf8File (Join-Path $resultDirectory "index.html") $html

Write-Output $resultDirectory
