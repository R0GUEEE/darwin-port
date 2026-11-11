# Run this checkout's setup utility inside an existing WSL distribution.
# Example: .\setup.ps1 plan --json
$ErrorActionPreference = 'Stop'
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'WSL is required. Install a Linux distribution with wsl --install, restart if requested, then retry.'
}
$LinuxRoot = & wsl.exe --exec wslpath -a $PSScriptRoot
if ($LASTEXITCODE -ne 0 -or -not $LinuxRoot) {
    throw 'Cannot map this checkout into WSL. Initialize the default WSL distribution first.'
}
& wsl.exe --exec sh "$($LinuxRoot.Trim())/setup.sh" @args
exit $LASTEXITCODE
