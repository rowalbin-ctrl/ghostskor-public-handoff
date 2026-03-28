param(
  [string]$ProjectRoot = (Split-Path -Parent $MyInvocation.MyCommand.Path)
)

$ErrorActionPreference = "Stop"
$utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
$failures = New-Object System.Collections.Generic.List[string]

function New-CodePointString {
  param(
    [int[]]$CodePoints
  )

  return -join ($CodePoints | ForEach-Object { [char]$_ })
}

$textExtensions = @(
  ".bat", ".cmd", ".ps1", ".cpp", ".c", ".h", ".hpp", ".inl", ".md",
  ".txt", ".json", ".csv", ".ini", ".def"
)

$selfPath = (Resolve-Path $MyInvocation.MyCommand.Path).Path

$mojibakePatterns = @(
  [string][char]0xFFFD,
  (New-CodePointString @(0x0076, 0x0069, 0x0072, 0x0074, 0x0075, 0x0061, 0x006C, 0x003F, 0xBB68, 0x0065, 0x0061, 0x006C)),
  (New-CodePointString @(0x0053, 0x004C, 0x0043, 0x003F, 0xBB5F, 0x0065, 0x0079)),
  (New-CodePointString @(0x003F, 0xC88F, 0xAE6E)),
  (New-CodePointString @(0x79FB, 0xC493, 0xB384)),
  (New-CodePointString @(0x91C9, 0xBDBE, 0xC613, 0x8E30, 0xAFA8, 0xBC76)),
  (New-CodePointString @(0x73E5, 0xB347, 0xB9B0)),
  (New-CodePointString @(0xF9E1, 0xC5A0, 0xC4E3))
)

$files = Get-ChildItem -Path $ProjectRoot -Recurse -File | Where-Object {
  $textExtensions -contains $_.Extension.ToLowerInvariant()
}

foreach ($file in $files) {
  if ($file.FullName -eq $selfPath) {
    continue
  }

  $bytes = [System.IO.File]::ReadAllBytes($file.FullName)

  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    $failures.Add("BOM detected: $($file.FullName)")
  }

  try {
    $text = $utf8Strict.GetString($bytes)
  } catch {
    $failures.Add("Invalid UTF-8: $($file.FullName)")
    continue
  }

  foreach ($pattern in $mojibakePatterns) {
    if ($text.Contains($pattern)) {
      $failures.Add("Mojibake pattern found in: $($file.FullName)")
      break
    }
  }
}

if ($failures.Count -gt 0) {
  $failures | ForEach-Object { Write-Host $_ }
  exit 1
}

Write-Host "Encoding verification passed for $($files.Count) files."
