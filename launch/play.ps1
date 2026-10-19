param([Parameter(Mandatory=$true)][string]$GameFolder)
$ErrorActionPreference='Stop'
$folder=(Resolve-Path $GameFolder).Path
$disc=Get-Content (Join-Path $folder 'disc.json') -Raw | ConvertFrom-Json
foreach($line in Get-Content "$PSScriptRoot/play_env.txt") {
    if ($line -match '^([A-Z0-9_]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
}
$env:PS2X_CD_IMAGE=$disc.iso
if ($disc.static_vu1) { $env:PS2X_VU1_AOT='1'; $env:PS2X_VU1_AOT_STATIC='1' }
if (-not $env:PS2X_SCRIPT) {
    if (-not $disc.mc_root) { throw 'Rerun setup to configure a persistent memory card' }
    $env:PS2X_MC_ROOT=$disc.mc_root
    & python "$PSScriptRoot/../setup/cards.py" --disc (Join-Path $folder 'disc.json')
    if ($LASTEXITCODE -ne 0) { throw 'Memory card backup failed' }
}
Push-Location $folder
try { & (Join-Path $folder 'ps2EntryRunner.exe') $disc.elf; exit $LASTEXITCODE } finally { Pop-Location }
