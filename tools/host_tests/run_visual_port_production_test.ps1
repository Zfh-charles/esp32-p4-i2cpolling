$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$stubRoot = Join-Path $PSScriptRoot 'visual_port_stubs'
$testBinary = Join-Path ([IO.Path]::GetTempPath()) ('p4_visual_port_' + [guid]::NewGuid().ToString('N') + '.exe')
try {
    foreach ($boardEnabled in @(1, 0)) {
        & $compiler -std=c++17 -Wall -Wextra -Werror `
            "-DCONFIG_BOARD_TYPE_EP_CHAT_P4_ML307=$boardEnabled" `
            '-I' $stubRoot '-I' (Join-Path $projectRoot 'main') `
            (Join-Path $PSScriptRoot 'visual_port_production_test.cc') `
            (Join-Path $projectRoot 'main\ports\visual_port.cc') '-o' $testBinary
        if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
        & $testBinary
        if ($LASTEXITCODE -ne 0) { throw "Host assertions failed: $LASTEXITCODE" }
        Write-Output "VisualPort production-source forwarding PASS board=$boardEnabled (device effects stubbed)"
    }
} finally {
    # Exact unique file created by this invocation only; no recursive deletion.
    if (Test-Path -LiteralPath $testBinary) { Remove-Item -LiteralPath $testBinary -Force }
}
