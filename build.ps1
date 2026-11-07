param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Format,
    [switch]$Tidy,
    [switch]$Selftest,
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$project = Join-Path $root "GameSnap\GameSnap.vcxproj"

# 自检代码只在 Debug 里编进来（见 selftest.h 的 GAMESNAP_SELFTEST）。
# 想跑 -Selftest 就必须构建 Debug，否则会出现"构建 Release、却运行上次遗留的 Debug exe"。
if ($Selftest -and $Configuration -ne 'Debug') {
    Write-Host "Selftest 需要 Debug 构建，Configuration 已从 $Configuration 改为 Debug"
    $Configuration = 'Debug'
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -find "MSBuild\**\Bin\MSBuild.exe" |
            Where-Object { $_ -and (Test-Path $_) } |
            Select-Object -First 1
        if ($found) { return $found }
    }
    $fallback = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    if (Test-Path $fallback) { return $fallback }
    throw "MSBuild not found"
}

function Find-Tool([string]$name) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\$name.exe"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\18\BuildTools\VC\Tools\Llvm\x64\bin\$name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $command = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

$msbuild = Find-MSBuild

if ($Format) {
    $clangFormat = Find-Tool "clang-format"
    if ($null -eq $clangFormat) {
        Write-Warning "clang-format not found; skip formatting"
    } else {
        Get-ChildItem (Join-Path $root "GameSnap") -File |
            Where-Object { $_.Extension -in ".h", ".cpp" } |
            Where-Object { $_.Name -ne "tonemap_cs.h" } |
            ForEach-Object { & $clangFormat --style=file --fallback-style=none -i $_.FullName }
        Write-Host "formatted GameSnap/*.h/*.cpp"
    }
}

$buildArgs = @(
    $project,
    "/p:Configuration=$Configuration",
    "/p:Platform=x64",
    "/p:EnableClangTidyCodeAnalysis=false",
    "/p:RunCodeAnalysis=false",
    "/p:VcpkgManifestInstall=false",
    "/m", "/nologo", "/v:minimal"
)
if ($Rebuild) { $buildArgs += "/t:Rebuild" }

Write-Host "building $Configuration x64..."
& $msbuild @buildArgs
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if ($Tidy) {
    $clangTidy = Find-Tool "clang-tidy"
    $llvmBin = if ($null -ne $clangTidy) { Split-Path $clangTidy -Parent } else { throw "clang-tidy not found" }
    $tidyArgs = @(
        $project,
        "/t:ClangTidy",
        "/p:Configuration=$Configuration",
        "/p:Platform=x64",
        "/p:VcpkgManifestInstall=false",
        "/p:RunCodeAnalysis=false",
        "/p:ClangTidyToolPath=$llvmBin",
        "/m", "/nologo", "/v:minimal"
    )
    Write-Host "running clang-tidy..."
    & $msbuild @tidyArgs
    if ($LASTEXITCODE -ne 0) { throw "clang-tidy failed" }
}

if ($Selftest) {
    # Configuration 已在脚本开头被强制成 Debug，这里构建与运行的是同一个二进制。
    $exe = Join-Path $root "GameSnap\x64\Debug\GameSnap.exe"
    if (-not (Test-Path $exe)) { throw "GameSnap.exe not found: $exe（Debug 构建失败？）" }
    $out = Join-Path $root "build-selftest"
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $out | Out-Null
    try {
        & $exe --selftest --out $out
        if ($LASTEXITCODE -ne 0) { throw "selftest failed" }
    } finally {
        Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    }
}

Write-Host "done"
