param(
  [switch]$NoDeploy,
  [string]$ToolchainSetup
)

$ErrorActionPreference = "Stop"

function Convert-ExternalOutputToText {
  param(
    [byte[]]$Bytes
  )

  $utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
  try {
    return $utf8Strict.GetString($Bytes)
  } catch {
    return [System.Text.Encoding]::GetEncoding(949).GetString($Bytes)
  }
}

function Write-Utf8NoBomFile {
  param(
    [string]$Path,
    [string]$Text
  )

  $encoding = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectDir
$patchDir = Split-Path -Parent $projectDir
$gameDir = Split-Path -Parent $patchDir

$buildDir = Join-Path $projectDir "build"
$rawLogPath = Join-Path $buildDir "build_log.raw"
$buildLogPath = Join-Path $projectDir "build_log.txt"
$buildTmpLogPath = Join-Path $projectDir "build_tmp_log.txt"
$outputDllPath = Join-Path $buildDir "dxgi.dll"
$gameDllPath = Join-Path $gameDir "dxgi.dll"
$gameDinput8Path = Join-Path $gameDir "dinput8.dll"
$gameVersionPath = Join-Path $gameDir "version.dll"

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
Remove-Item $rawLogPath, $buildLogPath, $buildTmpLogPath -ErrorAction SilentlyContinue

$verifyScript = Join-Path $projectDir "verify_encoding.ps1"
if (Test-Path $verifyScript) {
  & $verifyScript -ProjectRoot $projectDir
}
& (Join-Path $projectDir "verify_addresses.ps1") -ProjectRoot $projectDir

Write-Host "[GhostsKor] Starting Build Process..."

if ($ToolchainSetup) {
  $vcvarsPath = (Resolve-Path -LiteralPath $ToolchainSetup -ErrorAction Stop).Path
  if ([System.IO.Path]::GetExtension($vcvarsPath) -notin @('.bat', '.cmd')) {
    throw "ToolchainSetup must be an existing .bat or .cmd environment setup file."
  }
  Write-Host "[GhostsKor] Using toolchain setup: $vcvarsPath"
} else {
  $vswherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswherePath)) {
    throw "Visual Studio Installer\vswhere.exe not found. Use -ToolchainSetup for a standalone MSVC environment."
  }
  $vsPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
  if (-not $vsPath) {
    throw "Visual Studio with C++ workload not found."
  }
  Write-Host "[GhostsKor] Found VS at: $vsPath"
  $vcvarsPath = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvarsPath)) {
    throw "vcvars64.bat not found."
  }
}

$sources = @(
  "textreplacer.cpp",
  "ObjectiveRuntime.cpp",
  "TranslationStore.cpp",
  "texthook.cpp",
  "detour.cpp",
  "dllmain.cpp",
  "GameBuild.cpp",
  "GameAddresses.cpp",
  "GameTweaks.cpp",
  "DXGIWrapper.cpp",
  "D3D11Hook.cpp",
  "BindingResolver.cpp",
  "GamepadGlyphAtlas.cpp",
  "KoreanRenderer.cpp",
  "KoreanAtlas.cpp",
  "BinkHook.cpp",
  "FSHook.cpp",
  "vendor\minhook\src\buffer.c",
  "vendor\minhook\src\hook.c",
  "vendor\minhook\src\trampoline.c",
  "vendor\minhook\src\hde\hde32.c",
  "vendor\minhook\src\hde\hde64.c"
)

$clArgs = @(
  "/nologo",
  "/LD",
  "/MT",
  "/O2",
  "/EHa",
  "/utf-8",
  "/std:c++17",
  "/DGHOSTSKOR_OBJECTIVE_REVERSE=1",
  "/DGHOSTSKOR_LOGGING=0",
  "/DGHOSTSKOR_RUNTIME_DIAG=0"
) + $sources + @(
  "/I.",
  "/Ivendor/nlohmann",
  "/Ivendor/minhook/include",
  "user32.lib",
  "psapi.lib",
  "dxguid.lib",
  "d3d11.lib",
  "dxgi.lib",
  "d3dcompiler.lib",
  "/Fe:build\dxgi.dll",
  "/Fo:build\",
  "/link",
  "/DEF:exports.def",
  "/PDB:build\dxgi.pdb"
)

$quotedCl = ($clArgs | ForEach-Object {
    if ($_ -match '[\s"]') {
      '"' + ($_ -replace '"', '\"') + '"'
    } else {
      $_
    }
  }) -join " "

$resFile = Join-Path $buildDir "resources.res"

$cmdScript = @(
  "call ""$vcvarsPath"" >nul",
  "rc.exe /nologo /fo ""$resFile"" resources.rc > ""$rawLogPath"" 2>&1",
  "cl.exe $quotedCl ""$resFile"" >> ""$rawLogPath"" 2>&1"
) -join " && "

cmd.exe /d /c $cmdScript
$compileExitCode = $LASTEXITCODE

if (Test-Path $rawLogPath) {
  $rawBytes = [System.IO.File]::ReadAllBytes($rawLogPath)
  $logText = Convert-ExternalOutputToText -Bytes $rawBytes
  Write-Utf8NoBomFile -Path $buildLogPath -Text $logText
  Write-Utf8NoBomFile -Path $buildTmpLogPath -Text $logText
  Remove-Item $rawLogPath -ErrorAction SilentlyContinue
} else {
  Write-Utf8NoBomFile -Path $buildLogPath -Text ""
  Write-Utf8NoBomFile -Path $buildTmpLogPath -Text ""
}

if ($compileExitCode -ne 0) {
  Write-Host "[ERROR] Compilation Failed!"
  Get-Content $buildLogPath -Encoding UTF8
  exit $compileExitCode
}

Write-Host "[SUCCESS] Build Complete!"
Write-Host "[OUTPUT] build\dxgi.dll"
Write-Host ""

if ($NoDeploy) {
  Write-Host "[INFO] NoDeploy requested. Skipping deployment."
  exit 0
}

Write-Host "[GhostsKor] Cleaning old DLLs..."
Remove-Item $gameDinput8Path -ErrorAction SilentlyContinue
Remove-Item $gameVersionPath -ErrorAction SilentlyContinue

Write-Host "[GhostsKor] Deploying dxgi.dll to Game Directory..."
if (-not (Test-Path $gameDir)) {
  throw "Game directory not found: $gameDir"
}
Copy-Item -Path $outputDllPath -Destination $gameDllPath -Force

Write-Host "[SUCCESS] Installed dxgi.dll!"
Write-Host "[TARGET] $gameDllPath"
Write-Host "[INFO] Ready to Run Game."
