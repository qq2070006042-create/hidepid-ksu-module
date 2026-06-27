$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildScriptPath = Join-Path $RepoRoot "src/build-module.sh"
$BuildScript = Get-Content -Raw -Path $BuildScriptPath

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

Write-Host "build-module.sh path checks passed"
