param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [string]$Configuration = "Release",
    # 只决定产物文件名；真正的目标架构由构建所用工具链决定，两者必须由调用者保持一致。
    [ValidateSet("x64", "arm64")]
    [string]$Architecture = "x64",
    # CI 在 MSYS2 中先执行 cmake --install 并补齐 MinGW 运行时 DLL，再调用本脚本打包。
    # 此时 staging 已经过 PE 依赖校验，不能再清空重装，否则补齐的 DLL 会丢失。
    [switch]$UseExistingStaging
)

$ErrorActionPreference = "Stop"

# 所有可删除的 staging 都固定在用户明确传入的构建目录内部。解析绝对路径并验证父子关系后
# 才清理旧产物，避免拼错变量时递归删除仓库或其他用户目录。
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$staging = [System.IO.Path]::GetFullPath((Join-Path $resolvedBuild "windows-staging"))
$artifactDirectory = [System.IO.Path]::GetFullPath((Join-Path $resolvedBuild "artifacts"))
if (-not $staging.StartsWith($resolvedBuild + [System.IO.Path]::DirectorySeparatorChar,
                             [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean a staging path outside the build directory: $staging"
}

New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null

if (-not $UseExistingStaging) {
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    New-Item -ItemType Directory -Path $staging -Force | Out-Null

    # cmake --install 会在原生 Windows 构建中执行 Qt 官方部署脚本，收集 qwindows、qsqlite、
    # TLS 后端和运行库。任何一步失败都由 PowerShell 的 Stop 策略立即终止，不生成残缺安装器。
    cmake --install $resolvedBuild --config $Configuration --prefix $staging
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --install failed with exit code $LASTEXITCODE."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $staging "Shellium.exe"))) {
    throw "Shellium.exe is missing from the staging directory."
}

$versionLine = Select-String -LiteralPath (Join-Path $repositoryRoot "CMakeLists.txt") -Pattern 'project\(Shellium VERSION ([0-9.]+)' | Select-Object -First 1
if ($null -eq $versionLine) {
    throw "Cannot read the project version from CMakeLists.txt."
}
$version = $versionLine.Matches[0].Groups[1].Value
# x64 沿用历史文件名，已有下载链接和发布说明不受影响。
$portableSuffix = if ($Architecture -eq "x64") { "win64" } else { "win-$Architecture" }
$zipPath = Join-Path $artifactDirectory "Shellium-$version-$portableSuffix.zip"
$setupPath = Join-Path $artifactDirectory "Shellium-$version-setup-$Architecture.exe"

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
# 从 staging 内部归档，便携 ZIP 解压后直接得到 Shellium.exe 和插件目录，不携带开发机绝对
# 路径或多余的 windows-staging 父目录。finally 保证打包失败也会恢复调用者当前目录。
Push-Location -LiteralPath $staging
try {
    cmake -E tar cf $zipPath --format=zip -- .
    if ($LASTEXITCODE -ne 0) {
        throw "Portable ZIP creation failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$makensis = Get-Command makensis.exe -ErrorAction Stop
$nsisArguments = @(
    "/DSTAGING_DIR=$staging",
    "/DSHELLIUM_VERSION=$version",
    "/DOUTPUT_FILE=$setupPath",
    (Join-Path $repositoryRoot "packaging/windows/Shellium.nsi")
)
& $makensis.Source $nsisArguments
if ($LASTEXITCODE -ne 0) {
    throw "NSIS creation failed with exit code $LASTEXITCODE."
}

# 校验和必须最后生成；正式发布流程在此之前插入 SignTool 签名与 RFC 3161 时间戳步骤。
$hashFile = Join-Path $artifactDirectory "Shellium-$version-SHA256SUMS.txt"
$hashLines = foreach ($file in @($zipPath, $setupPath)) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([System.IO.Path]::GetFileName($file))"
}
Set-Content -LiteralPath $hashFile -Value $hashLines -Encoding ascii
Write-Host "Windows artifacts created in: $artifactDirectory"
