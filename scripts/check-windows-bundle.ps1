# check-windows-bundle.ps1 — run the packaged Windows bundle with only the Windows system folders on PATH.
# Fails when a DLL the bundle needs is missing from it (the MSIX package never searches PATH either)
# or when start-up blocks, e.g. on a DLL's error dialog.
param(
    [string]$Bundle = "",
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
if (-not $Bundle) {
    $root = Resolve-Path (Join-Path $PSScriptRoot "..")
    $Bundle = Join-Path $root "dist\northstar-win64"
}
$targets = @((Join-Path $Bundle "northstar.exe"), (Join-Path $Bundle "app\northstar-ui.exe"))
foreach ($target in $targets) {
    if (-not (Test-Path -LiteralPath $target)) {
        throw "Bundle executable not found: $target"
    }
}
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class NsBundleErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@
[NsBundleErrorMode]::SetErrorMode(0x8003) | Out-Null
$systemPath = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
foreach ($target in $targets) {
    $info = New-Object System.Diagnostics.ProcessStartInfo $target, "--version"
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = $env:SystemRoot
    $info.EnvironmentVariables["PATH"] = $systemPath
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Get-Process -Name northstar, northstar-ui -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
        throw "$target --version did not exit within $TimeoutSeconds s; a DLL is probably showing an error dialog"
    }
    $process.WaitForExit()
    $output = ($stdout.Result + $stderr.Result).Trim()
    if ($process.ExitCode -ne 0) {
        throw ("{0} --version exited with 0x{1:X8}: {2}" -f $target, $process.ExitCode, $output)
    }
    "{0}: {1}" -f $target, $output
}
"bundle-check-ok"
