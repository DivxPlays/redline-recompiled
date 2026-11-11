# The kit's Android test: runs a release the way a new user's first run goes, on the Android emulator: a freshly wiped
# virtual phone every run (no window on your desktop), the APK installed and started, then screenshots, its log and
# any crash or "isn't responding".
# Usage: powershell -ExecutionPolicy Bypass -File .claude/android-test.ps1 -Apk <the release APK> [-Wait 30]
#          [-Out build/android-test]
#   -Setup  first time on a PC: downloads Google's Android SDK tools, the emulator and an Android 15 phone image (about
#           2.5 GB, into %LOCALAPPDATA%\Android\Sdk), which means accepting Google's Android SDK licence
#           (https://developer.android.com/studio/terms). Ask the user first, for both. Needs Java 17 or newer.
# Results in -Out: result.json, shot-1/2/3.png, logcat.txt. Look at the screenshots. Exit code 0 = it installed,
# started and was still running at the end with no crash, 1 = not.
param([string]$Apk, [int]$Wait = 30, [string]$Out = "build/android-test", [switch]$Setup,
      [string]$Image = "system-images/android-35/default/x86_64")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Windows PowerShell downloads crawl while drawing a progress bar
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
$env:ANDROID_HOME = $sdk
$tools = Join-Path $sdk "cmdline-tools\latest\bin"
$adb = Join-Path $sdk "platform-tools\adb.exe"
$emulator = Join-Path $sdk "emulator\emulator.exe"
$avd = "kit-test"
$serial = "emulator-5584"  # its own port, so it never talks to another emulator or a plugged-in phone

if ($Setup) {
  if (-not (Get-Command java -ErrorAction SilentlyContinue)) { throw "the Android tools need Java 17 or newer (e.g. Eclipse Temurin)" }
  if (-not (Test-Path (Join-Path $tools "android.exe"))) {
    # Google's own package list names the current command-line tools download.
    $list = (Invoke-WebRequest -UseBasicParsing https://dl.google.com/android/repository/repository2-3.xml).Content
    $at = $list.IndexOf('path="cmdline-tools;latest"')
    $zip = [regex]::Match($list.Substring($at, $list.IndexOf('</remotePackage>', $at) - $at), 'commandlinetools-win-\d+_latest\.zip').Value
    if (-not $zip) { throw "couldn't find the command-line tools in Google's package list" }
    "downloading $zip from Google..."
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) $zip
    Invoke-WebRequest -UseBasicParsing "https://dl.google.com/android/repository/$zip" -OutFile $tmp
    $unpack = Join-Path $sdk "cmdline-tools\unpack"
    Expand-Archive $tmp $unpack -Force
    Remove-Item -Recurse -Force (Join-Path $sdk "cmdline-tools\latest") -ErrorAction SilentlyContinue  # tools too old for android.exe
    Move-Item (Join-Path $unpack "cmdline-tools") (Join-Path $sdk "cmdline-tools\latest")
    Remove-Item -Recurse -Force $unpack, $tmp
  }
  # Google's Android CLI replaced sdkmanager in 2026 and takes package paths with slashes. --no-metrics: no usage data.
  $ErrorActionPreference = "Continue"  # it reports progress on stderr; its exit codes are checked instead
  foreach ($p in "platform-tools", "emulator", $Image) {
    "installing $p..."
    & (Join-Path $tools "android.exe") --no-metrics sdk install $p 2>&1 | Select-Object -Last 1
    if ($LASTEXITCODE) { throw "couldn't install $p" }
  }
  "no" | & (Join-Path $tools "avdmanager.bat") create avd -n $avd -k $Image.Replace("/", ";") -d pixel_6 --force | Out-Null
  if ($LASTEXITCODE) { throw "couldn't create the virtual phone" }
  & $emulator -accel-check | Select-Object -Last 2
  "ready. If it says acceleration isn't available, the user turns on Windows Hypervisor Platform (Windows Features, then a restart)."
  exit 0
}
if (-not $Apk) { throw "usage: -Apk <the release APK> (or -Setup)" }
if (-not (Test-Path $emulator) -or -not (Test-Path (Join-Path $env:USERPROFILE ".android\avd\$avd.ini"))) {
  throw "the Android emulator isn't set up on this PC: run this with -Setup first (ask the user: it downloads about 2.5 GB and accepts Google's Android SDK licence)"
}
$apkPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Apk)
if (-not (Test-Path -LiteralPath $apkPath)) { throw "no APK at $Apk" }
$results = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (Test-Path $results) { Remove-Item -Recurse -Force $results }
New-Item -ItemType Directory -Force $results | Out-Null
function Adb { & $adb -s $serial @args }

# adb's server keeps running after the test: start it on its own, or it holds this script's output open for good.
[void](Start-Process $adb -ArgumentList "start-server" -WindowStyle Hidden -PassThru).WaitForExit(30000)  # not -Wait: that waits for the server too
"starting a freshly wiped virtual phone (no window; a minute or two)..."
$emu = Start-Process $emulator -PassThru -WindowStyle Hidden -ArgumentList "-avd $avd -port 5584 -no-window -no-audio -no-snapshot -wipe-data -no-boot-anim -gpu swiftshader_indirect"
try {
  $ErrorActionPreference = "Continue"  # adb reports progress on stderr
  $deadline = (Get-Date).AddMinutes(6)
  while ((Adb shell getprop sys.boot_completed 2>$null | Out-String).Trim() -ne "1") {
    if ($emu.HasExited) { throw "the emulator stopped while starting (exit $($emu.ExitCode))" }
    if ((Get-Date) -gt $deadline) { throw "the virtual phone never finished starting" }
    Start-Sleep -Seconds 3
  }
  $before = @(Adb shell pm list packages)
  $install = (Adb install $apkPath 2>&1 | Out-String).Trim()
  $installed = $install -match "Success"
  $package = @(Adb shell pm list packages | Where-Object { $before -notcontains $_ }) -replace "^package:", "" | Select-Object -First 1
  $launched = $false; $running = $false; $focus = ""
  if ($package) {
    Adb logcat -c
    "installed $package; starting it and watching for $Wait seconds..."
    $launched = (Adb shell monkey -p $package -c android.intent.category.LAUNCHER 1 2>&1 | Out-String) -match "Events injected: 1"
    foreach ($n in 1..3) {
      Start-Sleep -Seconds ([int]($Wait / 3))
      Adb shell screencap -p /sdcard/shot.png; Adb pull /sdcard/shot.png (Join-Path $results "shot-$n.png") 2>$null | Out-Null
    }
    $running = [bool](Adb shell pidof $package | Out-String).Trim()
    $focus = (Adb shell dumpsys window | Select-String "mCurrentFocus" | Select-Object -First 1 | Out-String).Trim()
  }
  Adb logcat -d | Set-Content -Encoding UTF8 (Join-Path $results "logcat.txt")
} finally {
  Adb emu kill 2>$null | Out-Null
  if (-not $emu.WaitForExit(30000)) { Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue }
  $ErrorActionPreference = "Stop"
}

$log = Get-Content (Join-Path $results "logcat.txt")
$crashed = [bool]($package -and ($log | Select-String -SimpleMatch "Process: $package"))
$errors = @($log | Select-String "FATAL EXCEPTION|ANR in |UnsatisfiedLinkError|Fatal signal" | Select-Object -First 20 | ForEach-Object { "$_".Trim() })
$errorWindow = $focus -match "Application Error|Not Responding|isn't responding"
$ok = [bool]($installed -and $launched -and $running -and -not $crashed -and -not $errors.Count -and -not $errorWindow)
$verdict = if (-not $installed) { "FAIL: it didn't install: $install" }
  elseif (-not $launched) { "FAIL: $package has no app to start (no launcher activity)" }
  elseif ($crashed -or $errors.Count) { "FAIL: it crashed or reported errors: $(if ($errors.Count) { $errors[0] } else { 'see logcat.txt' })" }
  elseif ($errorWindow) { "FAIL: an error is on screen: $focus" }
  elseif (-not $running) { "FAIL: it wasn't running at the end; it closed or crashed" }
  else { "PASS: $package installed, started and kept running on a fresh Android phone" }
[ordered]@{ apk = $Apk; installed = [bool]$installed; package = $package; launched = $launched; running = $running;
  crashed = $crashed; focus = $focus; errorLines = $errors; pass = $ok; verdict = $verdict } |
  ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $results "result.json")
$verdict
"look at the screenshots before trusting it: $results\shot-1.png, shot-2.png, shot-3.png (its log in $results\logcat.txt)"
exit $(if ($ok) { 0 } else { 1 })
