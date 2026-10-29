$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\verification_gates.ps1')
$script:checks = 0
function Check($ok) { $script:checks++; if (-not $ok) { throw "gate check $script:checks failed" } }
$script:calls = @()
$script:configureCode = 0
$script:buildCode = 0
function cmake {
    $script:calls += ($args -join ' ')
    if ($args[0] -eq '--build') { $global:LASTEXITCODE = $script:buildCode }
    else { $global:LASTEXITCODE = $script:configureCode }
    'diagnostic without any conventional error keyword'
}
$script:configureCode = 7
$failed = $false
try { Invoke-CheckedBuild 'source' 'build' 'compiler' } catch { $failed = $true }
Check $failed; Check ($script:calls.Count -eq 1)
$script:configureCode = 0; $script:buildCode = 19; $script:calls = @()
$failed = $false
try { Invoke-CheckedBuild 'source' 'build' 'compiler' } catch { $failed = $true }
Check $failed; Check ($script:calls.Count -eq 2)
$script:buildCode = 0; $script:calls = @()
Invoke-CheckedBuild 'source' 'build' 'compiler'
Check ($script:calls.Count -eq 2)
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('rlplayer-gates-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$script:fixture = ''; $script:testCode = 0
function ctest {
    $index = [array]::IndexOf($args, '--output-junit')
    if ($script:fixture) { [System.IO.File]::WriteAllText($args[$index+1], $script:fixture) }
    $global:LASTEXITCODE = $script:testCode
    'mock ctest'
}
foreach ($fixture in @('', '<testsuite/>', '<testsuite><testcase><skipped/></testcase></testsuite>',
    '<testsuite><testcase><failure/></testcase></testsuite>',
    '<testsuite><testcase><system-out>[SKIP] unavailable</system-out></testcase></testsuite>')) {
    $script:fixture = $fixture
    $failed = $false
    try { Invoke-CheckedTests $root | Out-Null } catch { $failed = $true }
    Check $failed
}
$script:fixture = '<testsuite><testcase><system-out>[PASS] assertions</system-out></testcase><testcase><skipped/></testcase></testsuite>'
$r = Invoke-CheckedTests $root
Check ($r.Total -eq 2); Check ($r.Skipped -eq 1)
$script:testCode = 8
$failed = $false
try { Invoke-CheckedTests $root | Out-Null } catch { $failed = $true }
Check $failed
function auditNative { Write-Error 'expected synthetic native stderr'; $global:LASTEXITCODE=0 }
Check ((Invoke-GateCommand 'auditNative' @()) -eq 0)
$failed=$false
try { Invoke-GateCommand 'rlplayer_nonexistent_build_tool' @() | Out-Null } catch { $failed=$true }
Check $failed
$script:testCode=0
$script:fixture='<testsuite><testcase name="healthy"><system-out>[PASS]</system-out></testcase><testcase name="ffmpeg_matrix"><system-out>[SKIP] missing packaged tool</system-out></testcase></testsuite>'
$failed=$false
try {Invoke-CheckedTests $root|Out-Null}catch{$failed=$true}
Check $failed
$script:fixture=$script:fixture.Replace('ffmpeg_matrix','gpu_bridge')
$r=Invoke-CheckedTests $root
Check ($r.InternalSkipOnly -eq 1)
Write-Output "[PASS] verification gate checks=$script:checks (no build/service/device invoked)"
