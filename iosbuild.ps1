# Build an unsigned iOS IPA from Windows PowerShell using the Linux toolchain in WSL.
# Example: .\iosbuild.ps1 ipa examples/windows-native --out build-out
param(
    [Parameter(Mandatory=$true, Position=0)][string]$Command,
    [Parameter(Mandatory=$true, Position=1)][string]$Target,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Options
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'WSL is required. Install and initialize a Linux distribution first.'
}
$LinuxRoot = (& wsl.exe --exec wslpath -a $PSScriptRoot)
if ($LASTEXITCODE -ne 0 -or -not $LinuxRoot) {
    throw 'Cannot map this checkout into WSL. Initialize the default WSL distribution first.'
}
$LinuxRoot = $LinuxRoot.Trim()
# Keep SDK arguments in WSL path form, e.g. /mnt/c/ios-sdk/iPhoneOS.sdk.
# Relative target/output paths resolve from the repository root in WSL.
& wsl.exe --cd $LinuxRoot --exec python3 "$LinuxRoot/iosbuild.py" $Command $Target @Options
exit $LASTEXITCODE
