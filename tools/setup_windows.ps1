# feito por YWZsaXRv
#
# Installs the Windows build tools the launcher's Setup tab checks for, via winget.
# Only the missing ones are installed, so it is safe to run again any time.
#

param(
    [switch] $dry_run,
    [switch] $yes
)

$ErrorActionPreference = 'Stop'

function find_program {
    param([string] $name)
    $app = Get-Command $name -ErrorAction SilentlyContinue
    if ($app) { return $app.Source }
    return $null
}

# native tools (python, winget) write warnings to stderr, which 'Stop' turns into a throw
function run_native {
    param([string] $exe, [string[]] $arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $exe @arguments 2>&1 | Out-String).Trim()
    } catch {
        return ''
    } finally {
        $ErrorActionPreference = $previous
    }
}

# Out-Host keeps the output on screen instead of the pipeline, so what comes back is only the exit code
function run_install {
    param([string] $exe, [string[]] $arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments | Out-Host
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
}

function first_line {
    param([string] $text)
    if (-not $text) { return '' }
    return $text.Split("`n")[0].Trim()
}

function check_clang {
    $exe = find_program 'clang'
    if (-not $exe) {
        $in_llvm = Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe'
        if (Test-Path $in_llvm) { $exe = $in_llvm }
    }
    if (-not $exe) { return $null }
    $text = run_native $exe @('--version')
    if ($text -match '(\d+)\.\d+' -and [int]$Matches[1] -ge 20) { return first_line $text }
    return $null
}

function check_cmake {
    $exe = find_program 'cmake'
    if (-not $exe) { return $null }
    $text = run_native $exe @('--version')
    if ($text -match '(\d+)\.(\d+)') {
        $major = [int]$Matches[1]
        $minor = [int]$Matches[2]
        if ($major -gt 3 -or ($major -eq 3 -and $minor -ge 25)) { return first_line $text }
    }
    return $null
}

function check_ninja {
    $exe = find_program 'ninja'
    if (-not $exe) { return $null }
    return first_line (run_native $exe @('--version'))
}

function check_python {
    $exe = find_program 'python'
    if (-not $exe) { return $null }
    $text = run_native $exe @('--version')
    if ($text -match 'Python 3') { return first_line $text }
    return $null
}

function check_git {
    $exe = find_program 'git'
    if (-not $exe) { return $null }
    return first_line (run_native $exe @('--version'))
}

function check_glslc {
    $exe = find_program 'glslc'
    if (-not $exe -and $env:VULKAN_SDK) {
        $in_sdk = Join-Path $env:VULKAN_SDK 'Bin\glslc.exe'
        if (Test-Path $in_sdk) { $exe = $in_sdk }
    }
    if (-not $exe) { return $null }
    return first_line (run_native $exe @('--version'))
}

function check_msvc {
    $program_files_x86 = ${env:ProgramFiles(x86)}
    if (-not $program_files_x86) { return $null }
    $vswhere = Join-Path $program_files_x86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $path = run_native $vswhere @('-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
    $path = first_line $path
    if ($path -and (Test-Path (Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'))) { return $path }
    return $null
}

function show_result {
    param([string] $label, [string] $detail, [string] $hint = 'missing')
    $color = if ($detail) { 'Green' } else { 'Red' }
    $value = if ($detail) { $detail } else { $hint }
    Write-Host ('  {0,-30} {1}' -f $label, $value) -ForegroundColor $color
}

$tools = @(
    @{ label = 'clang 20 or newer';       package = 'LLVM.LLVM';                           check = { check_clang } }
    @{ label = 'CMake 3.25 or newer';     package = 'Kitware.CMake';                       check = { check_cmake } }
    @{ label = 'Ninja';                   package = 'Ninja-build.Ninja';                   check = { check_ninja } }
    @{ label = 'Python 3';                package = 'Python.Python.3.12';                  check = { check_python } }
    @{ label = 'Git';                     package = 'Git.Git';                             check = { check_git } }
    @{ label = 'glslc (Vulkan SDK)';      package = 'KhronosGroup.VulkanSDK';              check = { check_glslc } }
    @{ label = 'Visual Studio C++ (x64)'; package = 'Microsoft.VisualStudio.2022.BuildTools'; check = { check_msvc };
       override = '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' }
)

function run_checks {
    param([string] $title)
    $missing = @()
    Write-Host $title
    Write-Host ''
    foreach ($tool in $tools) {
        $detail = & $tool.check
        show_result $tool.label $detail
        if (-not $detail) { $missing += $tool }
    }
    # the Vulkan loader ships with the graphics driver, so winget has nothing to install
    $vulkan = Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll')
    if ($vulkan) {
        show_result 'Vulkan loader' 'vulkan-1.dll'
    } else {
        show_result 'Vulkan loader' $null 'update your graphics driver'
    }
    return ,$missing
}

Write-Host ''
Write-Host 'Mind over Recomp - Windows build tools'
Write-Host ''

$winget = find_program 'winget'
if (-not $winget) {
    Write-Host 'winget was not found. Install "App Installer" from the Microsoft Store, then run this again.'
    exit 1
}

$missing = run_checks 'Checking:'
Write-Host ''
if (-not $missing) { exit 0 }

Write-Host 'Missing tools:'
Write-Host ''
foreach ($tool in $missing) {
    Write-Host ('  {0,-30} winget install {1}' -f $tool.label, $tool.package)
}
Write-Host ''

if ($dry_run) {
    Write-Host 'Dry run: nothing was installed.'
    exit 0
}

if (-not $yes) {
    $answer = Read-Host 'Install these now? [y/N]'
    if ($answer -notmatch '^[yY]') {
        Write-Host 'Cancelled: nothing was installed.'
        exit 0
    }
}

# winget returns this when the package is already there, not an error for us
$already_installed = -1978335189
$failed = @()
foreach ($tool in $missing) {
    $arguments = @('install', '--id', $tool.package, '-e',
        '--accept-package-agreements', '--accept-source-agreements')
    if ($tool.ContainsKey('override')) { $arguments += @('--override', $tool.override) }
    Write-Host ''
    Write-Host "Installing $($tool.label)..."
    $code = run_install $winget $arguments
    if ($code -eq 0 -or $code -eq $already_installed) {
        Write-Host "$($tool.label) installed"
    } else {
        Write-Host "$($tool.label) failed"
        $failed += $tool.label
    }
}

Write-Host ''
$null = run_checks 'Checking again:'

Write-Host ''
if ($failed) {
    Write-Host ('Still missing: {0}' -f ($failed -join ', '))
    exit 1
}

Write-Host 'Done. Restart the launcher so it sees the new tools.'
