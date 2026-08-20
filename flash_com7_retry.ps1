# Retry flash helper for ESP32-P4 USB-Serial/JTAG (COM7 Write timeout recovery).
param(
    [string]$Port = "COM7",
    [int]$MaxAttempts = 6
)

$ErrorActionPreference = "Continue"
$proj = "c:\bake\xiaozhi-p4-epdainaozhong0109\xiaozhi-p4-epdainaozhong"
$py = "C:\esp_alm\tool\python_env\idf5.4_py3.11_env\Scripts\python.exe"
$log = "c:\bake\xiaozhi-p4-epdainaozhong0109\flash_retry.txt"

function Stop-ComUsers {
    # Do NOT match bare $Port — this script's own CommandLine contains -Port COM7 and would self-kill.
    $self = $PID
    Get-CimInstance Win32_Process | Where-Object {
        $_.ProcessId -ne $self -and $_.CommandLine -and (
            $_.CommandLine -match 'serial_monitor\.py|idf_monitor|esp_idf_monitor|miniterm|python.*-m esptool|-m esptool'
        )
    } | ForEach-Object {
        Write-Host "kill $($_.ProcessId)"
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

Set-Location $proj
"" | Set-Content $log

for ($i = 1; $i -le $MaxAttempts; $i++) {
    Write-Host "==== flash attempt $i/$MaxAttempts ===="
    Stop-ComUsers
    Start-Sleep -Seconds 3

    # Prefer mid baud; fall back slower on later attempts.
    $baud = if ($i -le 2) { 460800 } elseif ($i -le 4) { 115200 } else { 9600 }

    $args = @(
        "-m", "esptool", "--chip", "esp32p4", "-p", $Port, "-b", "$baud",
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "32MB",
        "0x2000", "build/bootloader/bootloader.bin",
        "0x8000", "build/partition_table/partition-table.bin",
        "0x10d000", "build/ota_data_initial.bin",
        "0x200000", "build/xiaozhi.bin",
        "0xc00000", "build/generated_assets.bin"
    )

    & $py @args 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -eq 0) {
        Write-Host "FLASH OK on attempt $i"
        exit 0
    }

    Write-Host "attempt $i failed (exit=$LASTEXITCODE) — waiting for FLASH_WINDOW / USB settle"
    # Align with firmware FLASH_WINDOW (8-12s after crash reboot).
    Start-Sleep -Seconds 8
}

Write-Host "FLASH FAILED after $MaxAttempts attempts"
exit 1
