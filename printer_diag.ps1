# HP LaserJet Professional P1102w - Enterprise Printer Deployment Script
# Author: mjdeiter
# Version: 1.5 (LOCKED / AUDITED)
#
# PURPOSE
# - Enterprise-style, deterministic printer deployment
# - Silent driver staging (pinned to hp1100.inf ONLY)
# - Spooler health validation
# - Network reachability check
# - Idempotent printer + port creation
# - Timestamped logging
# - Final on-screen status summary
#
# DRIVER POLICY (NON-NEGOTIABLE)
# - Driver source is FIXED
# - NO recursion
# - NO auto-discovery
# - NO fallback logic
#
# EXPECTED LAYOUT:
#   printer_diag.ps1
#   drivers\
#     └─ HP_P1102w_clean\
#         ├─ hp1100.inf
#         ├─ hp1100.cat
#         ├─ *.dll
#   logs\
#
# Run as Administrator

$ErrorActionPreference = 'Stop'

$PrinterIP   = "192.168.4.68"
$PrinterPort = 9100
$PrinterName = "HP LaserJet Professional P1102w"
$PortName    = "IP_$PrinterIP"
$DriverName  = "HP LaserJet Professional P1102w"

$DriverRoot  = Join-Path $PSScriptRoot "drivers\HP_P1102w_clean"
$DriverInf   = Join-Path $DriverRoot "hp1100.inf"

$LogDir = Join-Path $PSScriptRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
$LogFile = Join-Path $LogDir ("printer_diag_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))

function Log {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Add-Content -Path $LogFile -Value $line
    Write-Host $Message
}

function Pause-And-Exit {
    param([int]$Code = 0)
    Write-Host ""
    Write-Host "===== FINAL STATUS SUMMARY =====" -ForegroundColor Cyan
    Get-Content $LogFile | Select-Object -Last 15
    Write-Host "Full log saved to: $LogFile" -ForegroundColor Yellow
    Read-Host "Press Enter to exit"
    exit $Code
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent() `
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Log "ERROR: Script must be run as Administrator."
    Pause-And-Exit 1
}

Log "Script started (v1.5)."
Log "Target printer IP: $PrinterIP"

function Ensure-Spooler {
    Log "Ensuring Print Spooler is running..."
    $svc = Get-Service Spooler -ErrorAction Stop
    if ($svc.Status -ne 'Running') {
        Log "Starting Print Spooler..."
        Start-Service Spooler
        Start-Sleep -Seconds 3
    }

    $ready = $false
    1..10 | ForEach-Object {
        Start-Sleep -Seconds 1
        try {
            Get-Printer | Out-Null
            $ready = $true
            return
        } catch {}
    }

    if (-not $ready) {
        Log "ERROR: Spooler RPC interface not available."
        throw "Spooler not ready"
    }

    Log "Print Spooler is ready."
}

function Ensure-Driver {
    Log "Checking for printer driver '$DriverName'..."
    if (Get-PrinterDriver -Name $DriverName -ErrorAction SilentlyContinue) {
        Log "Driver already installed."
        return
    }

    Log "Driver not present. Staging from pinned INF."

    if (-not (Test-Path $DriverInf)) {
        Log "ERROR: Required driver INF not found:"
        Log "       $DriverInf"
        Pause-And-Exit 1
    }

    Log "Staging driver: $DriverInf"
    pnputil /add-driver "$DriverInf" /install | Out-Null
    Start-Sleep -Seconds 3

    if (-not (Get-PrinterDriver -Name $DriverName -ErrorAction SilentlyContinue)) {
        Log "ERROR: Driver staging completed but driver not registered."
        Pause-And-Exit 1
    }

    Log "Driver staged successfully."
}

try {
    Ensure-Spooler

    Log "Checking printer network reachability..."
    if (Test-Connection -ComputerName $PrinterIP -Count 2 -Quiet) {
        Log "Printer responds to ping."
    } else {
        Log "WARNING: Printer did not respond to ping (may be asleep)."
    }

    Ensure-Driver

    if (Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue) {
        Log "Printer already exists in Windows. No action required."
        Pause-And-Exit 0
    }

    Log "Printer missing. Performing controlled installation."

    Ensure-Spooler

    if (-not (Get-PrinterPort -Name $PortName -ErrorAction SilentlyContinue)) {
        Log "Creating printer port $PortName"
        Add-PrinterPort -Name $PortName -PrinterHostAddress $PrinterIP -PortNumber $PrinterPort
    } else {
        Log "Printer port already exists."
    }

    Add-Printer -Name $PrinterName -PortName $PortName -DriverName $DriverName

    Log "Printer installation completed successfully."
    Pause-And-Exit 0

} catch {
    Log "FATAL ERROR: $($_.Exception.Message)"
    Pause-And-Exit 1
}
