param([Parameter(Mandatory=$true)][string]$Iso,[Parameter(Mandatory=$true)][string]$Work,[switch]$Optimise)
$ErrorActionPreference='Stop'
(Get-Process -Id $PID).PriorityClass='BelowNormal'
$extra=@(); if ($Optimise) { $extra += '--optimise' }
python "$PSScriptRoot/setup.py" --iso $Iso --work $Work @extra
exit $LASTEXITCODE
