# Portable scripted-route runner. All output belongs to the caller's private work directory.
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Elf,
    [Parameter(Mandatory=$true)][string]$Iso,
    [Parameter(Mandatory=$true)][string]$Script,
    [Parameter(Mandatory=$true)][string]$Work,
    [int]$Seconds=1200,
    [string]$McSeed='',
    [string]$Env='',
    [string[]]$BusyFlag=@(),
    [string]$SlotFile='',
    [string]$PgoSweepTool='',
    [string]$PgoSweepOutput=''
)
$ErrorActionPreference='Stop'
$checkout=Split-Path $PSScriptRoot -Parent
$workPath=[IO.Path]::GetFullPath($Work)
if ($workPath -eq $checkout -or $workPath.StartsWith($checkout+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Work must be outside the source checkout'
}
if ($Seconds -le 0) { throw 'Seconds must be positive' }
foreach ($path in @($Exe,$Elf,$Iso,$Script)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" } }
if ($McSeed -and -not (Test-Path -LiteralPath $McSeed -PathType Container)) { throw 'Missing private save seed' }
if (Test-Path -LiteralPath $workPath) { throw 'Use a fresh work directory; previous evidence is never overwritten' }
$Exe=(Resolve-Path -LiteralPath $Exe).Path
$Elf=(Resolve-Path -LiteralPath $Elf).Path
$Iso=(Resolve-Path -LiteralPath $Iso).Path
$Script=(Resolve-Path -LiteralPath $Script).Path
$slot=$null
$child=$null
$sweep=$null
$saved=@{}
try {
    # Hold an exclusive shared slot through the child lifetime. Other users must opt into the same slot path.
    while ($true) {
        $busy=@($BusyFlag | Where-Object { Test-Path -LiteralPath $_ }).Count -gt 0
        if (-not $busy) {
            if (-not $SlotFile) { break }
            try { $slot=[IO.File]::Open([IO.Path]::GetFullPath($SlotFile),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None); break }
            catch [IO.IOException] { }
        }
        Start-Sleep -Seconds 5
    }
    New-Item -ItemType Directory -Path $workPath | Out-Null
    $card=Join-Path $workPath 'card'
    New-Item -ItemType Directory -Path $card | Out-Null
    if ($McSeed) { Get-ChildItem -LiteralPath $McSeed -Force | Copy-Item -Destination $card -Recurse }
    $vars=[ordered]@{}
    Get-Content (Join-Path $checkout 'launch/play_env.txt') | ForEach-Object {
        if ($_ -match '^([^#=]+)=(.*)$') { $vars[$Matches[1].Trim()]=$Matches[2] }
    }
    foreach ($pair in ($Env -split ';')) {
        if ($pair -match '^([^=]+)=(.*)$') { $vars[$Matches[1].Trim()]=$Matches[2] }
    }
    # The scripted path anchors require a map made from this user's disc.
    $isoMap=Join-Path (Split-Path $Elf -Parent) 'isomap.tsv'
    if (-not $vars.Contains('PS2X_ISOMAP') -and (Test-Path -LiteralPath $isoMap)) { $vars['PS2X_ISOMAP']=$isoMap }
    # Setup retains an ordinary executable below game/fallback; support files stay in game.
    $exeDirectory=Split-Path $Exe -Parent
    if ((Split-Path $exeDirectory -Leaf) -eq 'fallback') {
        $supportDirectory=Split-Path $exeDirectory -Parent
        $vars['PATH']=$supportDirectory+[IO.Path]::PathSeparator+[Environment]::GetEnvironmentVariable('PATH','Process')
        $knownTable=Join-Path $supportDirectory 'vu1known.bin'
        if (-not $vars.Contains('PS2X_VU1NEW_TABLE') -and (Test-Path -LiteralPath $knownTable)) { $vars['PS2X_VU1NEW_TABLE']=$knownTable }
    }
    # Quiet unattended execution and private output paths always apply.
    $vars['PS2X_CD_IMAGE']=$Iso
    $vars['PS2X_MC_ROOT']=$card
    $vars['PS2X_SCRIPT']=$Script
    $vars['PS2X_SCRIPT_RESULT']=Join-Path $workPath 'script_result.json'
    $vars['PS2X_DUMP_DIR']=$workPath
    $vars['PS2X_AUDIO']='0'
    $vars['PS2X_HIDDEN_WINDOW']='1'
    $vars['PS2X_STALL_VS']='600'
    foreach ($key in $vars.Keys) {
        $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,$vars[$key],'Process')
    }
    $vars.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" } | Set-Content (Join-Path $workPath 'env.txt')
    Copy-Item -LiteralPath $Script -Destination (Join-Path $workPath 'route.tfs')
    'elapsed_seconds,total_cpu_percent' | Set-Content (Join-Path $workPath 'cpu.csv')
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $child=Start-Process -FilePath $Exe -ArgumentList ('"'+$Elf+'"') -WorkingDirectory $workPath -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $workPath 'stdout.log') -RedirectStandardError (Join-Path $workPath 'stderr.log')
    # Retain the process handle so ExitCode remains available after a short run.
    [void]$child.Handle
    try { if (-not $child.HasExited) { $child.PriorityClass='BelowNormal' } }
    catch {
        # Preserve the logs and exit code when termination races this update.
        if (-not $child.HasExited) { throw }
    }
    if ($PgoSweepTool) {
        if (-not $PgoSweepOutput) { throw 'Explicit profile output required' }
        for ($attempt=1; $attempt -le 60 -and -not $child.HasExited; $attempt++) {
            $sweepLog=Join-Path $workPath "sweep-$attempt.log"
            $sweepErr=Join-Path $workPath "sweep-$attempt.err"
            $sweep=Start-Process -FilePath $PgoSweepTool -ArgumentList @("/pid:$($child.Id)",'/wait',('"'+$Exe+'"'),('"'+$PgoSweepOutput+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput $sweepLog -RedirectStandardError $sweepErr
            $sweepHandle=$sweep.Handle
            try { if (-not $sweep.HasExited) { $sweep.PriorityClass='BelowNormal' } } catch { }
            while (-not $sweep.HasExited) {
                if (Select-String -LiteralPath $sweepLog -SimpleMatch "Waiting for PID $($child.Id) to terminate" -Quiet -ErrorAction SilentlyContinue) { break }
                if ($timer.Elapsed.TotalSeconds -ge $Seconds) { throw 'Profile collector readiness timeout' }
                Start-Sleep -Milliseconds 200
            }
            if (-not $sweep.HasExited) { break }
            $sweep.WaitForExit()
            if ($sweep.ExitCode -eq 0) { break }
            $message=(Get-Content -Raw $sweepLog -ErrorAction SilentlyContinue)+(Get-Content -Raw $sweepErr -ErrorAction SilentlyContinue)
            if ($message -notmatch 'PG1032') { throw "Profile collector failed: $message" }
            Start-Sleep -Seconds 1
        }
        if (-not $sweep -or ($sweep.HasExited -and $sweep.ExitCode -ne 0)) { throw 'Profile collector did not attach' }
    }

    $timedOut=$false
    while (-not $child.WaitForExit(1000)) {
        if ($timer.Elapsed.TotalSeconds -ge $Seconds) { $timedOut=$true; $child.Kill(); $child.WaitForExit(); break }
        if ([int]$timer.Elapsed.TotalSeconds % 5 -eq 0) {
            try { $load=(Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction Stop).PercentProcessorTime }
            catch { $load='' }
            ('{0:F1},{1}' -f $timer.Elapsed.TotalSeconds,$load) | Add-Content (Join-Path $workPath 'cpu.csv')
        }
    }
    if ($sweep) {
        if (-not $sweep.WaitForExit(60000)) { throw 'Profile collector exit timeout' }
        "PGOSWEEP_EXIT=$($sweep.ExitCode)" | Add-Content (Join-Path $workPath 'env.txt')
        if ($sweep.ExitCode -ne 0) { throw 'Profile collection failed' }
    }
    $code=if ($timedOut) {124} else {$child.ExitCode}
    "RUN_DONE code=$code wall=$($timer.Elapsed.TotalSeconds)" | Tee-Object -FilePath (Join-Path $workPath 'run.txt')
} finally {
    # Cleanup only the process this invocation created; no process enumeration or global stops.
    if ($child -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
    if ($sweep -and -not $sweep.HasExited) { $sweep.Kill(); $sweep.WaitForExit() }
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
    if ($slot) { $slot.Dispose() }
}
exit $code
