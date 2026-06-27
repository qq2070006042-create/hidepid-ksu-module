$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildScriptPath = Join-Path $RepoRoot "src/build-module.sh"
$BuildScript = Get-Content -Raw -Path $BuildScriptPath
$BuildAllScriptPath = Join-Path $RepoRoot "src/build-all.sh"
$BuildAllScript = Get-Content -Raw -Path $BuildAllScriptPath

$Checks = @(
    @{
        Name = "MODULE_DIR resolves to the repository root"
        Pattern = 'MODULE_DIR="\$\(cd "\$SCRIPT_DIR/\.\." && pwd\)"'
    },
    @{
        Name = "compiled ko files are copied into the root ko directory"
        Pattern = '"\$MODULE_DIR/ko/hidepid-\$\{kmi\}\.ko"'
    },
    @{
        Name = "normal zip is written to the repository root"
        Pattern = 'OUTPUT_ZIP="\$MODULE_DIR/hidepid-ksu-module\.zip"'
    },
    @{
        Name = "stealth zip is written to the repository root"
        Pattern = 'OUTPUT_ZIP="\$MODULE_DIR/hidepid-ksu-module-stealth\.zip"'
    },
    @{
        Name = "build failures are tracked before packaging"
        Pattern = 'BUILD_FAILED=0'
    },
    @{
        Name = "missing ko files fail the build before packaging"
        Pattern = 'missing required ko files'
    },
    @{
        Name = "incomplete builds exit before packaging"
        Pattern = 'if \[ "\$BUILD_FAILED" -ne 0 \]; then'
    }
)

foreach ($Check in $Checks) {
    if ($BuildScript -notmatch $Check.Pattern) {
        throw "Failed check: $($Check.Name)"
    }
}

if ($BuildScript -match 'hidepid_module') {
    throw "Failed check: build-module.sh still references the removed src/hidepid_module layout"
}

$BuildAllChecks = @(
    @{
        Name = "build-all.sh tracks KMI build failures"
        Pattern = 'BUILD_FAILED=0'
    },
    @{
        Name = "build-all.sh exits when any requested KMI failed"
        Pattern = 'if \[ "\$BUILD_FAILED" -ne 0 \]; then'
    },
    @{
        Name = "build-all.sh reports missing ko output"
        Pattern = 'missing required ko files'
    }
)

foreach ($Check in $BuildAllChecks) {
    if ($BuildAllScript -notmatch $Check.Pattern) {
        throw "Failed check: $($Check.Name)"
    }
}

Write-Host "build-module.sh path checks passed"
