$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$boardRoot = Join-Path $projectRoot 'main\boards\ep-chat-p4-ml307'
$testBinary = Join-Path ([IO.Path]::GetTempPath()) ('p4_route_admission_' + [guid]::NewGuid().ToString('N') + '.exe')
try {
    & $compiler -std=c++17 -Wall -Wextra -Werror `
        '-I' (Join-Path $PSScriptRoot 'face_route_stubs') `
        '-I' $boardRoot '-I' (Join-Path $projectRoot 'main\debug') `
        (Join-Path $PSScriptRoot 'face_route_admission_production_test.cc') `
        (Join-Path $boardRoot 'face_route_v2.cc') '-o' $testBinary
    if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { throw "Characterization failed: $LASTEXITCODE" }
    # Exit zero means observations reproduced, not that the product gap is fixed.
} finally {
    if (Test-Path -LiteralPath $testBinary) { Remove-Item -LiteralPath $testBinary -Force }
}
