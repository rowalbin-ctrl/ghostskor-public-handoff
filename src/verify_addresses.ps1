param([string]$ProjectRoot = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$patternSource = Get-Content -LiteralPath (Join-Path $ProjectRoot 'GameAddressPatterns.inl') -Raw
$functionNames = [regex]::Matches($patternSource, '\{"(\w+)"') | ForEach-Object { $_.Groups[1].Value }
if ($functionNames.Count -ne 31) { throw 'Expected 31 reviewed function patterns; update this check when the catalog changes.' }
$functionAlternation = ($functionNames | ForEach-Object { [regex]::Escape($_) }) -join '|'
$baseNames = '(?:s_moduleBase_sp|s_moduleBase|moduleBase|irfBase|itlBase|sub1Base|hdtBase|base|mb)'
$strippedTokens = '(?s)//[^\r\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
$problems = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $ProjectRoot -File | Where-Object { $_.Extension -in '.cpp','.h','.inl' } | ForEach-Object {
  $path = $_.FullName
  $code = Get-Content -LiteralPath $path -Raw
  $code = [regex]::Replace($code, $strippedTokens, ' ')
  if ($code -match "\b$baseNames\s*\+\s*IW6Offsets::(?:$functionAlternation)\b") {
    $problems.Add("$($_.Name): bypasses the validated function resolver")
  }
  foreach ($match in [regex]::Matches($code, "\b$baseNames\s*\+\s*0x([0-9a-fA-F]+)")) {
    if ([Convert]::ToUInt64($match.Groups[1].Value, 16) -gt 0xFFFF) {
      $problems.Add("$($_.Name): inline image RVA; move it into the reviewed profile")
    }
  }
}
if ($problems.Count) { throw ($problems -join [Environment]::NewLine) }
Write-Host 'Address audit passed: known function calls use the resolver; large inline image RVAs are centralized.'
