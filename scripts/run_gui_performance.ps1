param(
    [int[]]$SizesMiB = @(1, 10, 30),
    [int[]]$TabCounts = @(1, 3),
    [ValidateRange(4, 500)][int]$Samples = 80,
    [string]$OutputDirectory = 'build/gui-performance'
)

# Build first. Run this with an interactive Windows desktop and no concurrent
# builds/tests. The probe brings its own disposable MainWindow to the foreground.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$presets = Get-Content -LiteralPath (Join-Path $workspace 'build/generators/CMakePresets.json') -Raw | ConvertFrom-Json
$probe = Join-Path $workspace 'build/Release/MuffinGuiPerformanceProbe.exe'
if (-not (Test-Path -LiteralPath $probe)) {
    throw 'Build MuffinGuiPerformanceProbe with the conan-release preset first.'
}
$destination = [System.IO.Path]::GetFullPath((Join-Path $workspace $OutputDirectory))
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$originalPath = $env:PATH
$originalPlugins = $env:QT_PLUGIN_PATH
$originalQpa = $env:QT_QPA_PLATFORM
$originalForeground = $env:MUFFIN_GUI_REQUIRE_FOREGROUND
try {
    $env:QT_PLUGIN_PATH = $presets.testPresets[0].environment.QT_PLUGIN_PATH
    $env:PATH = $presets.configurePresets[0].environment.QT_HOST_PATH + '\bin;' + $originalPath
    $env:QT_QPA_PLATFORM = 'windows'
    $env:MUFFIN_GUI_REQUIRE_FOREGROUND = '1'
    foreach ($flag in @('MUFFIN_GUI_NATIVE_INPUT', 'MUFFIN_GUI_INPUT_ONLY', 'MUFFIN_GUI_TYPING_ONLY')) {
        if ([Environment]::GetEnvironmentVariable($flag, 'Process')) {
            throw "Unset $flag before running the complete GUI phase matrix."
        }
    }
    foreach ($size in $SizesMiB) {
        foreach ($tabs in $TabCounts) {
            if ($size -lt 1 -or $size -gt 30 -or $tabs -lt 1 -or $tabs -gt 3) {
                throw 'Sizes must be 1-30 MiB and tab counts 1-3.'
            }
            $result = Join-Path $destination "gui-${size}MiB-${tabs}tabs.json"
            # Windows PowerShell represents native stderr as ErrorRecord even
            # for harmless Qt font warnings. Preserve it and check the exit code.
            $ErrorActionPreference = 'Continue'
            & $probe $result $size $tabs $Samples *> "$result.log"
            $ErrorActionPreference = 'Stop'
            if ($LASTEXITCODE -ne 0) { throw "GUI probe failed: $result (exit $LASTEXITCODE)" }
            $record = Get-Content -LiteralPath $result -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($record.foreground_wait_timed_out) { throw "Timed out waiting for foreground; retain: $result" }
            if (($record.PSObject.Properties.Name -contains 'windows_foreground') -and -not $record.windows_foreground) {
                throw "No Windows foreground window; retain as processing diagnostic only: $result"
            }
            if (($record.PSObject.Properties.Name -contains 'native_input_complete') -and -not $record.native_input_complete) {
                throw "Incomplete native input; exclude: $result"
            }
            if (($record.PSObject.Properties.Name -contains 'draft_drain') -and -not $record.draft_drain.complete) {
                throw "Recovery drafts did not drain; retain and investigate: $result"
            }
            if (-not $record.active_window -or -not $record.native_window_exposed) {
                throw "Window lost activation/exposure; exclude and repeat this run: $result"
            }
            foreach ($phaseRecord in $record.phases) {
                if (($phaseRecord.PSObject.Properties.Name -contains 'windows_foreground_throughout') -and
                    -not $phaseRecord.windows_foreground_throughout) {
                    throw "Lost Windows foreground during $($phaseRecord.phase); exclude: $result"
                }
                if (($phaseRecord.PSObject.Properties.Name -contains 'active_throughout') -and
                    (-not $phaseRecord.active_throughout -or -not $phaseRecord.exposed_throughout)) {
                    throw "Window lost activation/exposure during $($phaseRecord.phase); exclude and repeat: $result"
                }
            }
        }
    }
} finally {
    $env:PATH = $originalPath
    $env:QT_PLUGIN_PATH = $originalPlugins
    $env:QT_QPA_PLATFORM = $originalQpa
    [Environment]::SetEnvironmentVariable('MUFFIN_GUI_REQUIRE_FOREGROUND', $originalForeground, 'Process')
}
