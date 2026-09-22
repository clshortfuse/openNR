param(
    [Parameter(Mandatory = $false)]
    [string] $CsvPath = (Join-Path $PSScriptRoot 'dlssnr-kernels.csv'),

    [Parameter(Mandatory = $false)]
    [string] $TracePath = (Join-Path $PSScriptRoot 'dlssnr-trace.json'),

    [Parameter(Mandatory = $false)]
    [string] $SpeedscopePath = (Join-Path $PSScriptRoot 'dlssnr-kernels.speedscope.json'),

    [Parameter(Mandatory = $false)]
    [string] $SummaryPath = (Join-Path $PSScriptRoot 'dlssnr-profile-summary.json')
)

$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Convert-ToDouble {
    param([string] $Value)
    return [double]::Parse($Value, [Globalization.NumberStyles]::Float, $invariant)
}

function Convert-ToInt64 {
    param([string] $Value)
    return [int64]::Parse($Value, [Globalization.NumberStyles]::Integer, $invariant)
}

if (-not (Test-Path -LiteralPath $CsvPath -PathType Leaf)) {
    throw "Missing kernel profile CSV: $CsvPath"
}
if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
    throw "Missing kernel trace JSON: $TracePath"
}

$rows = @(
    Import-Csv -LiteralPath $CsvPath |
        Where-Object {
            $_.kernel -ne 'dropped_records' -and
            $_.phase -match '^evaluation-\d+$'
        }
)
if ($rows.Count -eq 0) {
    throw 'Kernel profile CSV contains no evaluation rows'
}

$steadyRows = @(
    $rows |
        Where-Object {
            $_.phase -ne 'evaluation-001' -and
            (Convert-ToInt64 $_.gpu_timestamp_count) -gt 0
        }
)
if ($steadyRows.Count -eq 0) {
    throw 'Kernel profile CSV contains no steady-state GPU timestamps'
}

$entries = @(
    $steadyRows |
        Group-Object runtime_module, kernel |
        ForEach-Object {
            $first = $_.Group[0]
            $calls = [int64](
                $_.Group |
                    ForEach-Object { Convert-ToInt64 $_.gpu_timestamp_count } |
                    Measure-Object -Sum
            ).Sum
            $totalUs = [double](
                $_.Group |
                    ForEach-Object { Convert-ToDouble $_.gpu_total_us } |
                    Measure-Object -Sum
            ).Sum
            [pscustomobject]@{
                RuntimeModule = [int]$first.runtime_module
                Kernel = [string]$first.kernel
                Calls = $calls
                TotalUs = $totalUs
                MeanUs = if ($calls -eq 0) { 0.0 } else { $totalUs / $calls }
            }
        } |
        Sort-Object TotalUs -Descending
)
$totalUs = [double]($entries | Measure-Object TotalUs -Sum).Sum
if ($totalUs -le 0.0) {
    throw 'Steady-state timestamp total is zero'
}

$timestampLaunches = [int64](
    $rows |
        ForEach-Object { Convert-ToInt64 $_.gpu_timestamp_count } |
        Measure-Object -Sum
).Sum
$nvapiLaunches = [int64](
    $rows |
        ForEach-Object { Convert-ToInt64 $_.nvapi_launch_count } |
        Measure-Object -Sum
).Sum
$steadyTimestampLaunches = [int64](
    $steadyRows |
        ForEach-Object { Convert-ToInt64 $_.gpu_timestamp_count } |
        Measure-Object -Sum
).Sum

$summary = [ordered]@{
    schema = 2
    classification = 'D3D12 GPU timestamp profile'
    sourceCsv = [IO.Path]::GetFileName($CsvPath)
    sourceTrace = [IO.Path]::GetFileName($TracePath)
    e2eProfile = [IO.Path]::GetFileName($SpeedscopePath)
    e2eProfileFormat = 'Speedscope evented'
    excludesEvaluation1FromHotRanking = $true
    rawArtifactsRequired = $false
    timestampLaunches = $timestampLaunches
    nvapiLaunches = $nvapiLaunches
    launchCountMatches = $timestampLaunches -eq $nvapiLaunches
    steadyStateTimestampLaunches = $steadyTimestampLaunches
    steadyStateGpuTotalUs = [math]::Round($totalUs, 3)
    runtimeModuleCount = @($entries.RuntimeModule | Sort-Object -Unique).Count
    uniqueKernelCount = @($entries.Kernel | Sort-Object -Unique).Count
    interpretation = 'The Speedscope Time Order view preserves each evaluation launch sequence. Use an uninstrumented run for final frame time.'
    hotEntries = @(
        $entries | Select-Object -First 30 | ForEach-Object {
            [ordered]@{
                runtimeModule = $_.RuntimeModule
                kernel = $_.Kernel
                calls = $_.Calls
                totalUs = [math]::Round($_.TotalUs, 3)
                meanUs = [math]::Round($_.MeanUs, 3)
                percent = [math]::Round(100.0 * $_.TotalUs / $totalUs, 4)
            }
        }
    )
}
[IO.File]::WriteAllText(
    $SummaryPath,
    ($summary | ConvertTo-Json -Depth 6) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

$trace = Get-Content -LiteralPath $TracePath -Raw | ConvertFrom-Json
$traceEvents = @(
    $trace.traceEvents |
        Where-Object {
            $_.ph -eq 'X' -and
            $_.args.phase -match '^evaluation-\d+$' -and
            [double]$_.dur -ge 0.0
        }
)
if ($traceEvents.Count -eq 0) {
    throw 'Kernel trace contains no complete evaluation events'
}
if ($traceEvents.Count -ne $timestampLaunches) {
    throw "Trace/timestamp launch mismatch: $($traceEvents.Count) versus $timestampLaunches"
}

$frames = [Collections.Generic.List[object]]::new()
$frameIndices = @{}
function Get-FrameIndex {
    param([string] $Name)

    if (-not $frameIndices.ContainsKey($Name)) {
        $frameIndices[$Name] = $frames.Count
        $frames.Add([ordered]@{ name = $Name })
    }
    return [int]$frameIndices[$Name]
}

$eventsByPhase = [ordered]@{}
foreach ($traceEvent in $traceEvents) {
    $phase = [string]$traceEvent.args.phase
    if (-not $eventsByPhase.Contains($phase)) {
        $eventsByPhase[$phase] = [Collections.Generic.List[object]]::new()
    }
    $eventsByPhase[$phase].Add($traceEvent)
}

$profiles = [Collections.Generic.List[object]]::new()
$profileMetadata = [Collections.Generic.List[object]]::new()
foreach ($phase in $eventsByPhase.Keys) {
    $phaseEvents = @($eventsByPhase[$phase] | Sort-Object { [double]$_.ts })
    $firstTimestamp = [double]$phaseEvents[0].ts
    $profileDuration = [double](
        $phaseEvents |
            ForEach-Object {
                ([double]$_.ts - $firstTimestamp) + [double]$_.dur
            } |
            Measure-Object -Maximum
    ).Maximum
    if ($profileDuration -lt 0.0) {
        throw "Negative profile duration for $phase"
    }

    $profileEvents = [Collections.Generic.List[object]]::new()
    $rootFrame = Get-FrameIndex 'DLSS-NR GPU evaluation'
    $evaluationFrame = Get-FrameIndex $phase
    $profileEvents.Add([ordered]@{ type = 'O'; at = 0.0; frame = $rootFrame })
    $profileEvents.Add([ordered]@{ type = 'O'; at = 0.0; frame = $evaluationFrame })

    $previousEnd = 0.0
    foreach ($traceEvent in $phaseEvents) {
        $start = [double]$traceEvent.ts - $firstTimestamp
        $duration = [double]$traceEvent.dur
        $end = $start + $duration
        if ($start + 0.000001 -lt $previousEnd) {
            throw "$phase contains overlapping launch intervals at $start us"
        }

        $moduleName = [string]$traceEvent.cat
        if ([string]::IsNullOrWhiteSpace($moduleName)) {
            $moduleName = 'runtime-module-unknown'
        }
        $moduleFrame = Get-FrameIndex $moduleName
        $kernelFrame = Get-FrameIndex ([string]$traceEvent.name)
        $profileEvents.Add([ordered]@{ type = 'O'; at = $start; frame = $moduleFrame })
        $profileEvents.Add([ordered]@{ type = 'O'; at = $start; frame = $kernelFrame })
        $profileEvents.Add([ordered]@{ type = 'C'; at = $end; frame = $kernelFrame })
        $profileEvents.Add([ordered]@{ type = 'C'; at = $end; frame = $moduleFrame })
        $previousEnd = $end
    }

    $profileEvents.Add([ordered]@{ type = 'C'; at = $profileDuration; frame = $evaluationFrame })
    $profileEvents.Add([ordered]@{ type = 'C'; at = $profileDuration; frame = $rootFrame })
    $profiles.Add([ordered]@{
        type = 'evented'
        name = "$phase · $($phaseEvents.Count) launches · $($profileDuration.ToString('N3', $invariant)) us GPU span"
        unit = 'microseconds'
        startValue = 0.0
        endValue = $profileDuration
        events = @($profileEvents)
    })
    $profileMetadata.Add([ordered]@{
        phase = $phase
        launches = $phaseEvents.Count
        gpuSpanUs = [math]::Round($profileDuration, 3)
    })
}

$activeProfileIndex = 0
for ($index = 0; $index -lt $profileMetadata.Count; ++$index) {
    if ($profileMetadata[$index].phase -eq 'evaluation-002') {
        $activeProfileIndex = $index
        break
    }
}
$speedscope = [ordered]@{
    '$schema' = 'https://www.speedscope.app/file-format-schema.json'
    name = 'DLSS-NR end-to-end GPU kernel timelines'
    activeProfileIndex = $activeProfileIndex
    exporter = 'RenoDX DLSS-NR standalone repro'
    shared = [ordered]@{ frames = @($frames) }
    profiles = @($profiles)
}
[IO.File]::WriteAllText(
    $SpeedscopePath,
    ($speedscope | ConvertTo-Json -Depth 12) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

Write-Output "profile_summary=$SummaryPath"
Write-Output "speedscope_profile=$SpeedscopePath"
Write-Output "profiles=$($profiles.Count) timestamp_launches=$timestampLaunches nvapi_launches=$nvapiLaunches steady_gpu_us=$([math]::Round($totalUs, 3))"
if ($timestampLaunches -ne $nvapiLaunches) {
    throw "Timestamp/NVAPI launch mismatch: $timestampLaunches versus $nvapiLaunches"
}
