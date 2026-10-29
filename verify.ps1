# verify.ps1 - RLPlayer quick self-check (no audio device is opened)
# NOTE: keep this file UTF-8 WITH BOM. PowerShell 5.1 reads a BOM-less UTF-8 file as GBK
#       and fails to parse the Chinese comments ("Missing type name after '['").
$ErrorActionPreference='Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:Path = 'C:\Program Files\AMD\ROCm\7.2\bin;' + $env:Path
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$B = Join-Path $Root 'build'
$rows = @()
function Step($n, $ok, $d) { $script:rows += [pscustomobject]@{ Item=$n; Result=$(if($ok){'PASS'}else{'FAIL'}); Detail=$d } }
# 2026-10-24：端口一律动态取（下面自检会起 rtaweb / rtanet_rx）。
# 为什么：固定 8907/8908/45902 时，上一次自检残留的进程会让新实例 bind 失败并悄悄退出，
# 自检于是报「FAIL」——实际被测对象是健康的（1.3.15 就这么误报过一次 DirectSDM e2e）。
function Get-FreePort { $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0); $l.Start(); $p = $l.LocalEndpoint.Port; $l.Stop(); return $p }
Write-Host '=== RLPlayer self-check (silent: no audio device opened) ==='

# 1/2: stop immediately on a failed configure/build; stale binaries are never tested.
. (Join-Path $Root 'tools\verification_gates.ps1')
try {
  Invoke-CheckedBuild -Root $Root -Build $B -Compiler 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
  Step 'CMake build' $true 'configure=0 build=0'
  $testResult = Invoke-CheckedTests -Build $B
  Step ('Test suite (' + $testResult.Total + ')') $true ($testResult | ConvertTo-Json -Compress)
} catch {
  Step 'Build / test gate' $false $_.Exception.Message
  $rows | Format-Table -AutoSize
  exit 1
}

# 3 GPU library (multi-arch fat binary: every AMD family the compiler can target)
$dll = Join-Path $B 'rtagpu.dll'
if (Test-Path 'C:\Program Files\AMD\ROCm\7.2\bin\hipcc.bat') {
  $gout = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\build_gpu.ps1') -Out $dll 2>&1 | Out-String)
  Step 'rtagpu.dll (multi-arch)' ((Test-Path $dll) -and ($LASTEXITCODE -eq 0)) ($gout.Trim())
} else { Step 'rtagpu.dll (multi-arch)' $false 'hipcc missing' }

# 4 GPU convolution correctness
# 用 test_gpu_bridge（由 CMake 构建、也进了 ctest）：它走的是真正的 GpuBridge 动态加载路径。
# 旧的 gpu_conv.exe 是手工编的独立 demo，不跟着源码更新，曾经掩盖过
# "gpu_bridge.h 没取符号、GPU 从未真正启用"这个 bug。
$gc = Join-Path $B 'test_gpu_bridge.exe'
if (Test-Path $gc) {
  $g = (& $gc 2>&1 | Out-String)
  $okG = $g -match '\[PASS\]'
  $perf = ((($g -split ([char]10)) | Select-String 'PASS|SKIP|相对' ) -join ' ').Trim()
  Step 'GPU conv == CPU' $okG $perf
} else { Step 'GPU conv == CPU' $false 'test_gpu_bridge.exe missing' }

# 5 UPnP parser self-test
$up = Join-Path $B 'upnp_scan.exe'
if (Test-Path $up) {
  $u = (& $up --selftest 2>&1 | Out-String)
  Step 'UPnP parser self-test' ($u -match 'selftest')  '' 
} else { Step 'UPnP parser self-test' $false 'upnp_scan.exe missing' }

# 6 network end-to-end bit-exact
$play = Join-Path $B 'rtaplay.exe'; $rx = Join-Path $B 'rtanet_rx.exe'; $cmp = Join-Path $B 'wavcmp.exe'
if ((Test-Path $play) -and (Test-Path $rx) -and (Test-Path $cmp)) {
  $td = Join-Path $B 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
  $src = Join-Path $td 'vfy_src.wav'; $dst = Join-Path $td 'vfy_rx.wav'
  $null = & $play --gen $src 3 44100 24 2>&1
  $netCtrl = Get-FreePort
  $job = Start-Job -ScriptBlock { param($e, $o, $p) & $e --sink file --out $o --ctrl-port $p --seconds 9 2>&1 } -ArgumentList $rx, $dst, $netCtrl
  Start-Sleep -Seconds 2
  $null = & $play $src --net ('127.0.0.1:' + $netCtrl) 2>&1
  Wait-Job $job -Timeout 25 | Out-Null
  Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
  $c = (& $cmp $src $dst 2>&1 | Out-String)
  $cmpExit = $LASTEXITCODE
  $line = ((($c -split ([char]10)) | Select-String '[0-9]') -join ' ').Trim()
  Step 'Network e2e bit-exact' ($cmpExit -eq 0) ($line + ' (wavcmp exit=' + $cmpExit + ')')
} else { Step 'Network e2e bit-exact' $false 'exe missing' }

# 7 GPU resample kernel (vanilla-kernel sanity + CPU/GPU equality)
$grt = Join-Path $B 'gpu_resample_test.exe'
if (Test-Path $grt) {
  $g = (& $grt 2>&1 | Out-String)
  $okR = $g -match 'PASS'
  $l = ((($g -split ([char]10)) | Select-String 'PASS|FAIL' | Select-Object -First 1) -join ' ').Trim()
  Step 'GPU resample kernel' $okR $l
} else { Step 'GPU resample kernel' $false 'gpu_resample_test.exe missing' }

# 8 UI self-check (headless Edge + CDP, isolated application/state; never plays audio)
$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
$uiSafe = Join-Path $Root 'tools\uicheck-safe.ps1'
if ($nodeCmd -and (Test-Path -LiteralPath $uiSafe) -and (Test-Path -LiteralPath (Join-Path $B 'rtaweb.exe'))) {
  $uiSandbox = Join-Path $env:TEMP ('rta_uicheck_vfy_' + [guid]::NewGuid().ToString('N'))
  $wp = $null
  $uiStarted = $false
  $uiOwnedListener = $false
  $uiPort = Get-FreePort
  try {
    New-Item -ItemType Directory -Path $uiSandbox -ErrorAction Stop | Out-Null
    foreach ($leaf in @('library', 'tmp', 'data')) {
      New-Item -ItemType Directory -Path (Join-Path $uiSandbox $leaf) -ErrorAction Stop | Out-Null
    }
    # Copy executable assets only. Never copy user queues, settings or credentials.
    Copy-Item -LiteralPath (Join-Path $B 'rtaweb.exe') -Destination $uiSandbox -ErrorAction Stop
    foreach ($tool in @('ffmpeg.exe', 'ffprobe.exe')) {
      if (Test-Path -LiteralPath (Join-Path $B $tool)) { Copy-Item -LiteralPath (Join-Path $B $tool) -Destination $uiSandbox -ErrorAction Stop }
    }
    $opra = Join-Path $Root 'data\opra_index.json'
    if (Test-Path -LiteralPath $opra) { Copy-Item -LiteralPath $opra -Destination (Join-Path $uiSandbox 'data') -ErrorAction Stop }
    [IO.File]::WriteAllText((Join-Path $uiSandbox 'rlplayer-settings.conf'),
      "backend=null`nvolume=-60`nmetaon=0`nmetallm=0`nmetasummary=0`nroomcorrauto=0`ngpuvendor=off`n", [Text.UTF8Encoding]::new($false))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $uiSandbox 'rtaweb.exe'; $psi.Arguments = "--port $uiPort"
    $psi.WorkingDirectory = $uiSandbox
    $psi.EnvironmentVariables['RLPLAYER_LIBRARY_DIR'] = Join-Path $uiSandbox 'library'
    $psi.EnvironmentVariables['RLPLAYER_UPDATE_DIR'] = $uiSandbox
    $psi.EnvironmentVariables['TEMP'] = Join-Path $uiSandbox 'tmp'
    $psi.EnvironmentVariables['TMP'] = Join-Path $uiSandbox 'tmp'
    $psi.EnvironmentVariables['PATH'] = $B + ';' + $env:PATH
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $wp = New-Object System.Diagnostics.Process; $wp.StartInfo = $psi
    if (-not $wp.Start()) { throw 'isolated rtaweb process did not start' }
    $uiStarted = $true
    $uiReady = $false
    $uiReadyError = "owned listener has not appeared"
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 200
      try {
        $t = New-Object Net.Sockets.TcpClient; $t.Connect('127.0.0.1', $uiPort); $t.Close()
        $listeners = @(Get-NetTCPConnection -LocalPort $uiPort -State Listen -ErrorAction Stop | Where-Object { $_.OwningProcess -eq $wp.Id })
        if ($listeners.Count -gt 0 -and -not $wp.HasExited) { $uiOwnedListener = $true; $uiReady = $true; break }
        $uiReadyError = "listener is not owned by test pid " + $wp.Id
      } catch { $uiReadyError = $_.Exception.Message }
      if ($wp.HasExited) { break }
    }
    if ($uiReady) {
      $uo = (& powershell -NoProfile -ExecutionPolicy Bypass -File $uiSafe -Url "http://127.0.0.1:$uiPort/" -ExeDir $uiSandbox -RepoRoot $Root 2>&1 | Out-String)
      $uiExit = $LASTEXITCODE
      $ul = ((($uo -split ([char]10)) | Select-String '全部通过|项失败' | Select-Object -First 1) -join ' ').Trim()
      Step 'Web UI self-check (isolated)' ($uiExit -eq 0) ($ul + ' sandbox=' + $uiSandbox)
    } else { Step 'Web UI self-check (isolated)' $false ('isolated listener not verified on port ' + $uiPort + ': ' + $uiReadyError + '; sandbox=' + $uiSandbox) }
  } catch { Step 'Web UI self-check (isolated)' $false $_.Exception.Message }
  finally {
    if ($wp) {
      if ($uiStarted -and -not $wp.HasExited) {
        # Do not send a state-changing request to a port now owned by another process.
        try {
          $listeners = @(Get-NetTCPConnection -LocalPort $uiPort -State Listen -ErrorAction Stop | Where-Object { $_.OwningProcess -eq $wp.Id })
          if ($uiOwnedListener -and $listeners.Count -gt 0) {
            $null = Invoke-WebRequest -Uri "http://127.0.0.1:$uiPort/api/quit" -Method Post -UseBasicParsing -TimeoutSec 5
          }
        } catch {}
        if (-not $wp.WaitForExit(5000)) { Step 'Isolated UI shutdown' $false ('owned test server did not exit; pid=' + $wp.Id + '; sandbox=' + $uiSandbox) }
      }
      $wp.Dispose()
    }
  }
} else { Step 'Web UI self-check (isolated)' $false 'node, uicheck-safe.ps1 or rtaweb.exe missing' }

# 9 DSD domain: material generation -> DirectSDM bit-exact -> integrator THD+N
$mk = Join-Path $B 'make_dsd.exe'; $dc = Join-Path $B 'dsdconv.exe'
if ((Test-Path $mk) -and (Test-Path $dc)) {
  $td = Join-Path $B 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
  $dsf = Join-Path $td 'vfy_dsd64.dsf'; $dop = Join-Path $td 'vfy_dsd64_dop.wav'
  $mo = (& $mk --out $dsf --seconds 1 --verify 2>&1 | Out-String)
  $do = (& $dc --in $dsf --direct --out $dop 2>&1 | Out-String)
  $dcExit = $LASTEXITCODE
  $bitOk = $do -match 'DIRECT_SDM_BITEXACT=1'
  $thd = 0.0
  if ($mo -match 'THD\+N[^=]*=\s*([0-9.]+) dB') { $thd = [double]$Matches[1] }
  Step 'DSD path (DirectSDM + THD+N)' (($dcExit -eq 0) -and $bitOk -and ($thd -ge 80.0)) ('DirectSDM bit-exact=' + $bitOk + ', THD+N ' + $thd + ' dB')
} else { Step 'DSD path (DirectSDM + THD+N)' $false 'make_dsd/dsdconv missing' }

# 10 Removed backend rejection: actual handlers in the audit application's Null-only sandbox.
# NAA has been removed. It must return 400 before sink creation; it is not a GUI network output.
# RTANet bit-exact remains covered by section 6; DirectSDM tools remain covered by section 9.
$appAudit = Join-Path $Root 'tests\test_audit_app.js'
if ($nodeCmd -and (Test-Path -LiteralPath $appAudit)) {
  $auditRoot = Join-Path $env:TEMP ('rta_removed_backend_vfy_' + [guid]::NewGuid().ToString('N'))
  $ao = (& node $appAudit $B $auditRoot 2>&1 | Out-String)
  $auditExit = $LASTEXITCODE
  $auditLine = ((($ao -split ([char]10)) | Select-String '\[PASS\] actual app checks=' | Select-Object -First 1) -join ' ').Trim()
  Step 'Removed backend rejected (Null-isolated app)' (($auditExit -eq 0) -and ($auditLine.Length -gt 0)) ($auditLine + ' sandbox=' + $auditRoot)
} else { Step 'Removed backend rejected (Null-isolated app)' $false 'node or test_audit_app.js missing' }

# 11 distribution compatibility: static CRT, subsystem <= 6.1, no AVX-512
$cc = Join-Path $Root 'tools\check_compat.ps1'
if (Test-Path $cc) {
  $co = (& powershell -NoProfile -ExecutionPolicy Bypass -File $cc -Dir $B 2>&1 | Out-String)
  $ccExit = $LASTEXITCODE
  $cl = ((($co -split ([char]10)) | Select-String 'FAIL count' | Select-Object -First 1) -join ' ').Trim()
  Step 'Distribution compatibility' ($ccExit -eq 0) $cl
} else { Step 'Distribution compatibility' $false 'check_compat.ps1 missing' }

Write-Host ''
$rows | Format-Table -AutoSize | Out-String | Write-Host
$f = ($rows | Where-Object { $_.Result -eq 'FAIL' } | Measure-Object).Count
Write-Host ('FAIL count: ' + $f)
Write-Host 'NOTE: no audio device was opened during this check.'
exit $f