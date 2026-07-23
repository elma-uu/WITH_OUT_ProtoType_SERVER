# Generates C++ headers from the .fbs schemas into Protocol/generated/cpp.
# Uses flatc.exe from this folder; falls back to PATH if not present.
#
# Usage: powershell -File Protocol/generate.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$schemaDir = Join-Path $root "schemas"
$outDir = Join-Path $root "generated\cpp"
$localFlatc = Join-Path $root "flatc.exe"
$flatc = if (Test-Path $localFlatc) { $localFlatc } else { "flatc" }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Every schema file must be passed explicitly - passing only packet.fbs
# generates a header that #includes sibling headers (common.h, login.h, ...)
# that never get written.
$schemas = Get-ChildItem -Path $schemaDir -Filter "*.fbs" | Select-Object -ExpandProperty FullName

& $flatc --cpp --cpp-std c++17 --gen-object-api --filename-suffix "" `
    -o $outDir `
    $schemas

if ($LASTEXITCODE -ne 0) {
    throw "flatc failed with exit code $LASTEXITCODE"
}

Write-Host "Generated headers in $outDir"
