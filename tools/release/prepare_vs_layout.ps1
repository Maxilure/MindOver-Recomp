# Prepare the Microsoft offline layout used when building a Windows release.
# The workload and SDK must match the install arguments in launcher/setup.cpp.
param(
    [Parameter(Mandatory)]
    [string]$Layout
)

$ErrorActionPreference = 'Stop'
$Layout = [System.IO.Path]::GetFullPath($Layout)
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('mor-vs-' + [guid]::NewGuid())
$bootstrapper = Join-Path $work 'vs_buildtools.exe'
New-Item -Path $work -ItemType Directory | Out-Null
try {
    Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $bootstrapper
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation(?:,|$)') {
        throw 'The Visual Studio bootstrapper does not have a valid Microsoft signature.'
    }
    $layoutArguments = @('--layout', ('"' + $Layout + '"'), '--lang', 'en-US', '--quiet', '--wait',
                         '--add', 'Microsoft.VisualStudio.Workload.VCTools',
                         '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                         '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100')
    $process = Start-Process -FilePath $bootstrapper -ArgumentList $layoutArguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $Layout 'Catalog.json')) -or
        -not (Test-Path -LiteralPath (Join-Path $Layout 'vs_buildtools.exe'))) {
        throw "Visual Studio layout failed (exit code $($process.ExitCode))."
    }
} finally {
    Remove-Item -LiteralPath $bootstrapper -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -ErrorAction SilentlyContinue
}
