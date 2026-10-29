# Shared configure/build/test gates. Requires CMake/CTest 3.21 for JUnit results.
function Invoke-GateCommand {
    param([string]$Command, [string[]]$Arguments)
    $null = Get-Command $Command -ErrorAction Stop
    $previousPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 maps native stderr to ErrorRecords; retain diagnostics
        # and judge the actual exit code even under the caller's Stop preference.
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = $null
        $output = & $Command @Arguments 2>&1
        $commandExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    $output | Out-Host
    if ($null -eq $commandExit) { throw "$Command did not return an exit code" }
    return $commandExit
}
function Invoke-CheckedBuild {
    param([string]$Root, [string]$Build, [string]$Compiler)
    $configureExit = Invoke-GateCommand 'cmake' @('-G','Ninja','-S',$Root,'-B',$Build,'-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_CXX_COMPILER=$Compiler")
    if ($configureExit -ne 0) { throw "CMake configure failed (exit=$configureExit)" }
    $buildExit = Invoke-GateCommand 'cmake' @('--build',$Build)
    if ($buildExit -ne 0) { throw "CMake build failed (exit=$buildExit)" }
}
function Invoke-CheckedTests {
    param([string]$Build, [string[]]$ExtraArgs = @())
    $report = Join-Path $Build 'ctest-gate.xml'
    # Prevent a failed command from making a previous run's report appear current.
    if (Test-Path -LiteralPath $report) { Remove-Item -LiteralPath $report -ErrorAction Stop }
    $testExit = Invoke-GateCommand 'ctest' (@('--test-dir',$Build,'--no-tests=error','-V','--output-junit',$report) + $ExtraArgs)
    if ($testExit -ne 0) { throw "CTest failed (exit=$testExit)" }
    if (-not (Test-Path -LiteralPath $report)) { throw 'CTest did not produce a result report' }
    [xml]$xml = Get-Content -LiteralPath $report -Raw -ErrorAction Stop
    $cases = @($xml.SelectNodes('//testcase'))
    $skips = @($xml.SelectNodes('//testcase[skipped]'))
    $failures = @($xml.SelectNodes('//testcase[failure or error]'))
    if ($cases.Count -eq 0 -or $cases.Count -eq $skips.Count -or $failures.Count -gt 0) {
        throw "Invalid test result: total=$($cases.Count) skipped=$($skips.Count) failed=$($failures.Count)"
    }
    # Some hardware tests report an in-process SKIP rather than a CTest skip.
    $internalAllSkip = @($cases | Where-Object {
        $text = [string]$_.'system-out'
        $text -match '\[SKIP\]' -and $text -notmatch '\[PASS\]'
    }).Count
    if ($internalAllSkip -eq $cases.Count) { throw 'Every test reported an in-process SKIP' }
    # The CI contract deliberately has no GPU DLL; every other whole-test skip is a missing gate.
    $unexpectedSkipOnly = @($cases | Where-Object {
        $text = [string]$_.'system-out'
        $text -match '\[SKIP\]' -and $text -notmatch '\[PASS\]' -and $_.name -ne 'gpu_bridge'
    })
    if ($unexpectedSkipOnly.Count -gt 0) { throw ('Unexpected skip-only tests: ' + (($unexpectedSkipOnly | ForEach-Object {$_.name}) -join ', ')) }

    return [pscustomobject]@{ Total=$cases.Count; Skipped=$skips.Count; Failed=$failures.Count; InternalSkipOnly=$internalAllSkip }
}
