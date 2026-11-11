# The kit's clean-PC test: runs a release the way a new user's first run goes, on a fresh Windows (Windows Sandbox:
# no Python, no Visual C++ libraries, no dev tools, nothing from this PC), then throws that Windows away.
# Usage: powershell -ExecutionPolicy Bypass -File .claude/clean-pc-test.ps1 -Package <zip or folder, as users download it> -Run <what a user
#          starts, e.g. RedlineLauncher.exe or "Install Backseat.bat"> [-Wait 90] [-Map <read-only folder>] [-Out build/clean-pc]
#   -Wait  seconds to watch it run after starting (screenshots at a third, two thirds and the end)
#   -Map   a folder the test needs, shared read-only at C:\Map (e.g. the user's own game disc for a recomp)
#   -Previous  the last release's package: the upgrade test. A player runs the old version first (it writes its
#          settings and saves), then installs this one over it; the settings and saves it wrote must still be there,
#          unchanged by the update's own files, and the new version must start.
# First, Windows Defender scans the package: a flagged release gets quarantined on players' PCs before it runs.
# Results in -Out: result.json, shot-1/2/3.png (and shot-old.png) and the package's logs. Look at the screenshots: a
# window can be open and still show an error. Exit code 0 = it ran with no error windows or crashes, 1 = it didn't.
# Needs Windows Sandbox (Windows Pro or Enterprise) and its wsb command. Only one sandbox can run at a time.
param([Parameter(Mandatory)][string]$Package, [Parameter(Mandatory)][string]$Run, [int]$Wait = 90, [string]$Map = "",
      [string]$Previous = "", [string]$Out = "build/clean-pc")
$ErrorActionPreference = "Stop"
if (-not (Get-Command wsb -ErrorAction SilentlyContinue)) { throw "no wsb command: turn on Windows Sandbox (Windows Pro or Enterprise)" }
if ((cmd /c "wsb list 2>nul" | Out-String).Trim()) { throw "a Windows Sandbox is already running; close it first (only one can run)" }

$pkg = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Package)
if (-not (Test-Path -LiteralPath $pkg)) { throw "no package at $Package" }
$prev = if ($Previous) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Previous) } else { "" }
if ($prev -and -not (Test-Path -LiteralPath $prev)) { throw "no previous package at $Previous" }
$results = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (Test-Path $results) { Remove-Item -Recurse -Force $results }
New-Item -ItemType Directory -Force $results | Out-Null

# Windows Defender, scan only (nothing is quarantined here). Recompiled and packed programs get false positives.
$defender = "not scanned (no Windows Defender)"
$mp = Join-Path $env:ProgramFiles "Windows Defender\MpCmdRun.exe"
if (Test-Path $mp) {
  $ErrorActionPreference = "Continue"
  $scan = & $mp -Scan -ScanType 3 -File $pkg -DisableRemediation 2>&1 | Out-String
  $defender = switch ($LASTEXITCODE) { 0 { "clean" } 2 { "flagged" } default { "scan failed (exit $LASTEXITCODE)" } }
  $ErrorActionPreference = "Stop"
  if ($defender -eq "flagged") {
    $threats = @([regex]::Matches($scan, '(?m)^Threat\s*:\s*(.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    [ordered]@{ run = $Run; defender = $defender; threats = $threats } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $results "result.json")
    "FAIL: Windows Defender flags the package ($($threats -join ', ')). Players' PCs would quarantine it before it runs."
    "If it's a false positive, send it to Microsoft (https://www.microsoft.com/wdsi/filesubmission) and don't publish until it scans clean."
    exit 1
  }
}
"Windows Defender: $defender"

$inbox = Join-Path ([System.IO.Path]::GetTempPath()) ("kit-clean-pc-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force (Join-Path $inbox "package") | Out-Null
Copy-Item -LiteralPath $pkg -Destination (Join-Path $inbox "package") -Recurse
if ($prev) {
  New-Item -ItemType Directory -Force (Join-Path $inbox "previous") | Out-Null
  Copy-Item -LiteralPath $prev -Destination (Join-Path $inbox "previous") -Recurse
}

# What runs inside the sandbox, as the logged-in user: unpack, start it, watch, collect.
Set-Content -Encoding UTF8 (Join-Path $inbox "run.ps1") -Value @'
param([string]$Run, [int]$Wait)
$res = "C:\Results"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
function Shot($n) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save("$res\shot-$n.png"); $bmp.Dispose()
}
function Unpack($from) {
  $item = Get-ChildItem $from | Select-Object -First 1
  New-Item -ItemType Directory -Force C:\Test | Out-Null
  if ($item.Extension -eq ".zip") { Expand-Archive -LiteralPath $item.FullName -DestinationPath C:\Test -Force }
  else { Copy-Item -LiteralPath $item.FullName -Destination C:\Test -Recurse -Force }
}
function Find { Get-ChildItem C:\Test -Recurse -File | Where-Object { $_.Name -eq (Split-Path $Run -Leaf) } | Select-Object -First 1 }
# Where programs keep settings and saves: their own folder, AppData, Documents, Saved Games (logs and caches aside).
function DataFiles {
  @("C:\Test", $env:APPDATA, $env:LOCALAPPDATA, [Environment]::GetFolderPath("MyDocuments"), "$env:USERPROFILE\Saved Games") |
    Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { Get-ChildItem $_ -Recurse -File -Force -ErrorAction SilentlyContinue } |
    Where-Object { $_.FullName -notmatch '\\(Temp|Microsoft|Packages|CrashDumps)\\|\\[^\\]*cache[^\\]*\\' -and  # GPU shader caches too
      $_.Extension -notin '.log', '.tmp', '.lock', '.etl', '.dmp' } | ForEach-Object FullName
}
$wrote = @{}; $lost = @(); $replaced = @()
if (Test-Path C:\In\previous) {
  # The upgrade test: the old version first, as the player had it, then closed the way a player closes it.
  Unpack C:\In\previous
  $base = @(DataFiles)
  $old = Find
  if ($old) {
    $pre = @(Get-Process | ForEach-Object Id)
    Start-Process -FilePath $old.FullName -WorkingDirectory $old.DirectoryName
    Start-Sleep -Seconds ([int]($Wait / 2)); Shot "old"
    Get-Process | Where-Object { $pre -notcontains $_.Id } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    foreach ($f in @(DataFiles | Where-Object { $base -notcontains $_ })) { $wrote[$f] = (Get-FileHash -LiteralPath $f).Hash }
  }
}
Unpack C:\In\package
foreach ($f in $wrote.Keys) {  # the update's own files must not overwrite what the player's old version wrote
  if ((Test-Path -LiteralPath $f) -and (Get-FileHash -LiteralPath $f).Hash -ne $wrote[$f]) { $replaced += $f }
}
$target = Find
$before = @(Get-Process | ForEach-Object Id)
if ($target) {
  Start-Process -FilePath $target.FullName -WorkingDirectory $target.DirectoryName
  foreach ($n in 1..3) { Start-Sleep -Seconds ([int]($Wait / 3)); Shot $n }
}
# Only windows of what the test started: this script's own console doesn't count as the app running.
$windows = @(Get-Process | Where-Object { $_.MainWindowTitle -and $before -notcontains $_.Id } |
  ForEach-Object { "$($_.ProcessName): $($_.MainWindowTitle)" })
$errors = @($windows | Where-Object { $_ -match "System Error|has stopped working|was not found|is missing|Fatal|Unhandled|Exception|Error" })
# A console left open (an installer stopped at "pause") counts as a window, so read what it says: an error there is a
# failure even though nothing crashed.
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class KitCon {
  [StructLayout(LayoutKind.Sequential)] public struct XY { public short X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct Info { public XY Size, Cursor; public short Attr, L, T, R, B; public XY Max; }
  [DllImport("kernel32.dll")] static extern bool FreeConsole();
  [DllImport("kernel32.dll")] static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateFile(string n, uint a, uint s, IntPtr p, uint d, uint f, IntPtr t);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GetConsoleScreenBufferInfo(IntPtr h, out Info i);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool ReadConsoleOutputCharacter(IntPtr h, StringBuilder b, uint n, XY at, out uint read);
  public static string Read(uint pid) {
    FreeConsole();
    if (!AttachConsole(pid)) return null;
    var h = CreateFile("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    var text = new StringBuilder();
    Info i;
    if (GetConsoleScreenBufferInfo(h, out i)) {
      for (short y = 0; y <= i.Cursor.Y; y++) {
        var line = new StringBuilder(i.Size.X); uint read; var at = new XY(); at.Y = y;
        ReadConsoleOutputCharacter(h, line, (uint)i.Size.X, at, out read);
        text.AppendLine(line.ToString().TrimEnd());
      }
    }
    CloseHandle(h); FreeConsole();
    return text.ToString();
  }
}
"@
foreach ($p in @(Get-Process -Name cmd, powershell, pwsh -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -and $before -notcontains $_.Id })) {
  $text = [KitCon]::Read([uint32]$p.Id)
  if (-not $text) { continue }
  Set-Content -Encoding UTF8 "$res\console-$($p.Id).txt" $text
  $bad = @($text -split "`r?`n" | Where-Object { $_ -match "error|fail|not recognized|exception|cannot|could not|missing|denied" })
  if ($bad.Count) { $errors += "console of $($p.ProcessName): $($bad[0].Trim())" }
}
$crashed = [bool](Get-Process WerFault -ErrorAction SilentlyContinue)
$new = @(Get-Process | Where-Object { $before -notcontains $_.Id } | ForEach-Object ProcessName | Sort-Object -Unique)
New-Item -ItemType Directory -Force "$res\logs" | Out-Null
Get-ChildItem C:\Test -Recurse -File -Include *.log, *.dmp, crash*.txt -ErrorAction SilentlyContinue |
  Where-Object { $_.Length -lt 20MB } | ForEach-Object { Copy-Item $_.FullName "$res\logs\$($_.Name)" -ErrorAction SilentlyContinue }
$lost = @($wrote.Keys | Where-Object { -not (Test-Path -LiteralPath $_) })
[ordered]@{ run = $Run; found = [bool]$target; windows = $windows; errorWindows = $errors; crashed = $crashed;
  newProcesses = $new; upgrade = (Test-Path C:\In\previous); oldVersionWrote = @($wrote.Keys); lost = $lost;
  replaced = $replaced } | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 "$res\result.json"
'@

$map = if ($Map) { "<MappedFolder><HostFolder>$((Resolve-Path $Map).Path)</HostFolder><SandboxFolder>C:\Map</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>" } else { "" }
$config = "<Configuration><vGPU>Enable</vGPU><Networking>Enable</Networking><MappedFolders>" +
  "<MappedFolder><HostFolder>$inbox</HostFolder><SandboxFolder>C:\In</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>" +
  "<MappedFolder><HostFolder>$results</HostFolder><SandboxFolder>C:\Results</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>" +
  "$map</MappedFolders></Configuration>"

"starting a fresh Windows Sandbox (its window opens; leave it alone until the test ends)..."
$started = (wsb start --config $config --raw | Out-String | ConvertFrom-Json)
$id = if ($started.Id) { $started.Id } else { $started.id }
if (-not $id) { throw "the sandbox didn't start: $started" }
try {
  # wsb reports "not ready yet" on stderr, which Windows PowerShell turns into a terminating error under "Stop":
  # from here on, its exit codes are checked by hand.
  $ErrorActionPreference = "Continue"
  # Its user only logs in once a window connects to it; wsb connect stays open as that window, so it runs alongside.
  Start-Process wsb -ArgumentList "connect --id $id" -WindowStyle Hidden
  $deadline = (Get-Date).AddMinutes(4)  # wait for its user to log in
  while ($true) {
    wsb exec --id $id -c "cmd /c exit 0" -r ExistingLogin 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { break }
    if ((Get-Date) -gt $deadline) { throw "the sandbox never finished logging in" }
    Start-Sleep -Seconds 3
  }
  if ($prev) { "upgrade test: the previous release runs first, then this one goes over it..." }
  "running $Run on the fresh PC and watching for $Wait seconds..."
  wsb exec --id $id -c "powershell -WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -File C:\In\run.ps1 -Run `"$Run`" -Wait $Wait" -r ExistingLogin | Out-Null
  $deadline = (Get-Date).AddSeconds(2 * $Wait + 240)  # in case exec returned before the script finished
  while (-not (Test-Path (Join-Path $results "result.json"))) {
    if ((Get-Date) -gt $deadline) { throw "no result from the sandbox" }
    Start-Sleep -Seconds 2
  }
} finally {
  # Make sure it's gone: a sandbox left running blocks the next test (only one can run).
  foreach ($try in 1..10) {
    wsb stop --id $id 2>$null | Out-Null
    Start-Sleep -Seconds 3
    if (-not ((cmd /c "wsb list 2>nul") | Out-String).Contains($id)) { break }
  }
  Remove-Item -Recurse -Force $inbox -ErrorAction SilentlyContinue
}

$r = Get-Content (Join-Path $results "result.json") -Raw | ConvertFrom-Json
$lost = @($r.lost) + @($r.replaced)
$ok = [bool]($r.found -and -not $r.crashed -and -not @($r.errorWindows).Count -and @($r.windows).Count -and -not $lost.Count)
$verdict = if (-not $r.found) { "FAIL: $Run isn't in the package" }
  elseif ($r.crashed) { "FAIL: it crashed (Windows Error Reporting came up)" }
  elseif (@($r.errorWindows).Count) { "FAIL: error windows: $($r.errorWindows -join ' | ')" }
  elseif (-not @($r.windows).Count) { "FAIL: nothing is on screen at the end; it closed or failed without a window" }
  elseif (@($r.lost).Count) { "FAIL: the update lost what the old version wrote (settings, saves): $(@($r.lost) -join ' | ')" }
  elseif (@($r.replaced).Count) { "FAIL: the update's own files replaced what the old version wrote: $(@($r.replaced) -join ' | ')" }
  elseif ($prev) { "PASS: it updates over the previous release, keeps its settings and saves, and runs with no errors" }
  else { "PASS: it ran on a fresh PC with no error windows or crashes" }
foreach ($k in @{ defender = $defender; pass = $ok; verdict = $verdict }.GetEnumerator()) { $r | Add-Member $k.Key $k.Value -Force }
$r | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $results "result.json")
"windows open at the end: $(if (@($r.windows).Count) { $r.windows -join ' | ' } else { 'none' })"
if ($prev) { "upgrade: the previous release wrote $(@($r.oldVersionWrote).Count) settings or save file(s)" }
$verdict
"look at the screenshots before trusting it: $results\shot-1.png, shot-2.png, shot-3.png$(if ($prev) { ', shot-old.png' }) (logs in $results\logs)"
exit $(if ($ok) { 0 } else { 1 })
