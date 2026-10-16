param(
    [ValidateRange(4, 500)][int]$Samples = 80,
    [ValidateRange(1, 10)][int]$Repetitions = 2,
    [ValidateRange(1, 3600)][int]$ForegroundWaitSeconds = 120,
    [string]$OutputDirectory = ('build/input-comparison-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
)

# Keep orchestration and validation in one implementation. The Python entry
# point can also run directly without invoking a PowerShell script.
$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'run_input_comparison.py') --samples $Samples --repetitions $Repetitions `
    --foreground-wait-seconds $ForegroundWaitSeconds --output-directory $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Comparison incomplete; retained logs and comparison.json describe the failure.' }
