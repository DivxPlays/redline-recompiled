# The kit's Linux test: runs a release the way a Linux (or Steam Deck) player's first run goes, in WSL2: a Linux build
# natively, or the Windows build under Wine (Steam Deck's Proton is Wine underneath). Every run gets a fresh home
# folder, a fresh Wine prefix and its own virtual screen (Xvfb), so nothing carries over and nothing opens on your
# desktop. The Linux itself has Wine and the test's tools on it, as a player's would have Wine or Proton.
# Usage: powershell -ExecutionPolicy Bypass -File .claude/linux-test.ps1 -Package <zip, tar, AppImage, file or folder> -Run <what a player
#          starts, e.g. RedlineLauncher.exe or redline.sh> [-Wine] [-Wait 60] [-Out build/linux-test]
#   -Wine   run it under Wine; automatic when -Run is a .exe, .bat, .cmd or .msi
#   -Proton run the Windows build under GE-Proton instead, the way Steam Deck players run it (Proton without Steam,
#           a fresh compatibility folder each run; give it -Wait 90: Proton's first start sets up its prefix)
#   -Setup  first time on a PC: installs Ubuntu in WSL and the test's tools (Wine, Xvfb, ImageMagick). Ask the user
#           first: it downloads about 1 GB. With -Proton, also the latest GE-Proton (about 500 MB, from its GitHub
#           releases, checked against its SHA-512 sum).
# Results in -Out: result.json, shot-1/2/3.png, app.log (all it printed). Look at the screenshots: a window can be open
# and still show an error. Exit code 0 = a window is open at the end and nothing reported an error, 1 = not.
param([string]$Package, [string]$Run, [switch]$Wine, [switch]$Proton, [int]$Wait = 60, [string]$Out = "build/linux-test",
      [string]$Distro = "Ubuntu-26.04", [switch]$Setup)
$ErrorActionPreference = "Stop"
$env:WSL_UTF8 = "1"
if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) { throw "no WSL on this PC: 'wsl --install' (then restart)" }

# What runs inside Linux. "setup": install the tools. "run": the test, inside xvfb-run's virtual screen.
$sh = @'
#!/bin/bash
if [ "$1" = setup ]; then
  set -e
  export DEBIAN_FRONTEND=noninteractive
  # apt tries IPv6 first, and WSL often has no IPv6 route
  echo 'Acquire::ForceIPv4 "true";' > /etc/apt/apt.conf.d/99force-ipv4
  dpkg --add-architecture i386
  apt-get update -qq
  apt-get install -y -qq wine wine64 wine32:i386 xvfb x11-utils xdotool imagemagick unzip >/dev/null
  echo "ready: $(wine --version)"
  exit 0
fi
if [ "$1" = proton ]; then
  # GE-Proton, the Proton build most Steam Deck and Linux players add: the latest release from its GitHub page,
  # checked against its published SHA-512 sum. Vulkan in software (lavapipe): Proton draws through DXVK.
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y -qq python3 curl mesa-vulkan-drivers mesa-vulkan-drivers:i386 libvulkan1 libvulkan1:i386 >/dev/null
  mkdir -p /opt/kit-proton && cd /opt/kit-proton
  curl -4 -fsSL https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest > latest.json
  # the PC (x86_64) build: since GE-Proton 11 a release also carries an ARM (aarch64) one
  pick() { python3 -c "import json; a=[x for x in json.load(open('latest.json'))['assets'] if x['name'].endswith('$1') and not any(arm in x['name'] for arm in ('aarch64', 'arm64'))]; a.sort(key=lambda x: 'x86_64' not in x['name']); print(a[0]['browser_download_url'])"; }
  tarball=$(pick .tar.gz); sums=$(pick .sha512sum); name=$(basename "$tarball" .tar.gz)
  if [ ! -x "$name/proton" ]; then
    echo "downloading $name..."
    curl -4 -fsSL -o "$name.tar.gz" "$tarball"
    curl -4 -fsSL -o "$name.sha512sum" "$sums"
    sha512sum -c "$name.sha512sum"
    tar -xzf "$name.tar.gz" && rm "$name.tar.gz"
  fi
  ln -sfn "/opt/kit-proton/$name" /opt/kit-proton/current
  for old in GE-Proton*; do [ "$old" = "$name" ] || [ "$old" = "$name.sha512sum" ] || rm -rf "/opt/kit-proton/$old"; done
  echo "ready: $name"
  exit 0
fi
pkg=$(wslpath -a "$2"); run=$3; wait=$4; out=$(wslpath -a "$5"); mode=$6
export PATH=$PATH:/usr/lib/x86_64-linux-gnu/wine  # Ubuntu keeps wineserver off the PATH
work=$(mktemp -d /tmp/kit-linux-XXXXXX)
export HOME=$work/home
mkdir -p "$HOME" "$work/app"
unset WAYLAND_DISPLAY  # windows go to the virtual screen, not the user's desktop
case "$pkg" in
  *.zip) unzip -q "$pkg" -d "$work/app" ;;
  *.tar|*.tar.*|*.tgz) tar -xf "$pkg" -C "$work/app" ;;
  *) cp -r "$pkg" "$work/app/" ;;
esac
target=$(find "$work/app" -type f -name "$(basename "$run")" | head -n1)
: > "$out/app.log"; : > "$out/state.txt"
if [ -n "$target" ]; then
  echo found=1 >> "$out/state.txt"
  cd "$(dirname "$target")"
  if [ "$mode" = wine ]; then
    export WINEPREFIX=$HOME/.wine WINEDEBUG=fixme-all
    # a new prefix, as on a player's first run, without the Mono and Gecko download prompts
    WINEDLLOVERRIDES="mscoree,mshtml=" wineboot --init >/dev/null 2>&1; wineserver -w
    case "$target" in
      *.bat|*.cmd) wine cmd /c "$target" ;;
      *.msi) wine msiexec /i "$target" ;;
      *) wine "$target" ;;
    esac >> "$out/app.log" 2>&1 &
  elif [ "$mode" = proton ]; then
    # Proton without Steam: a fresh compatibility folder (its prefix), as on a player's first run
    export STEAM_COMPAT_DATA_PATH=$HOME/compat STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/steam WINEDEBUG=fixme-all
    mkdir -p "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH"
    case "$target" in
      *.bat|*.cmd) /opt/kit-proton/current/proton run cmd /c "$target" ;;
      *) /opt/kit-proton/current/proton run "$target" ;;
    esac >> "$out/app.log" 2>&1 &
  else
    chmod +x "$target"
    APPIMAGE_EXTRACT_AND_RUN=1 "$target" >> "$out/app.log" 2>&1 &  # WSL has no FUSE for AppImages
  fi
  pid=$!
  for n in 1 2 3; do sleep $((wait / 3)); import -window root "$out/shot-$n.png" 2>/dev/null; done
  if kill -0 "$pid" 2>/dev/null; then echo running=1 >> "$out/state.txt"
  else wait "$pid"; echo "exit=$?" >> "$out/state.txt"; fi
fi
for w in $(xdotool search --onlyvisible --name . 2>/dev/null); do xdotool getwindowname "$w"; done |
  grep -v '^$' | sort -u > "$out/windows.txt"
[ -n "$target" ] && kill "$pid" 2>/dev/null
[ "$mode" = wine ] && wineserver -k 2>/dev/null
[ "$mode" = proton ] && WINEPREFIX=$HOME/compat/pfx /opt/kit-proton/current/files/bin/wineserver -k 2>/dev/null
rm -rf "$work"
exit 0
'@
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kit-linux-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
[System.IO.File]::WriteAllText((Join-Path $tmp "kit-linux.sh"), $sh.Replace("`r`n", "`n"))
$installed = { @(wsl -l -q | ForEach-Object { $_.Trim() }) -contains $Distro }

try {
  if ($Setup) {
    if (-not (& $installed)) {
      "installing $Distro in WSL..."
      wsl --install -d $Distro --no-launch
      if ($LASTEXITCODE) { throw "couldn't install $Distro" }
    }
    "installing Wine, Xvfb and ImageMagick in $Distro (about 1 GB)..."
    wsl -d $Distro -u root --cd $tmp --exec bash kit-linux.sh setup
    if ($LASTEXITCODE) { throw "the tools didn't install; read the apt errors above" }
    if ($Proton) {
      "installing GE-Proton from its GitHub releases (about 500 MB)..."
      wsl -d $Distro -u root --cd $tmp --exec bash kit-linux.sh proton
      if ($LASTEXITCODE) { throw "GE-Proton didn't install; read the errors above" }
    }
    exit 0
  }
  if (-not $Package -or -not $Run) { throw "usage: -Package <zip or folder> -Run <what a player starts> (or -Setup)" }
  if (-not (& $installed)) { throw "$Distro isn't in WSL yet: run this with -Setup first (ask the user: it downloads about 1 GB)" }
  wsl -d $Distro -u root --exec sh -c "command -v xvfb-run && command -v import && command -v xdotool && command -v wine" | Out-Null
  if ($LASTEXITCODE) { throw "the test's tools aren't in $Distro yet: run this with -Setup first" }

  $pkg = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Package)
  if (-not (Test-Path -LiteralPath $pkg)) { throw "no package at $Package" }
  $results = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
  if (Test-Path $results) { Remove-Item -Recurse -Force $results }
  New-Item -ItemType Directory -Force $results | Out-Null
  $mode = if ($Proton) { "proton" } elseif ($Wine -or $Run -match '\.(exe|bat|cmd|msi)$') { "wine" } else { "native" }
  $useWine = $mode -ne "native"
  $how = @{ native = ""; wine = " under Wine"; proton = " under Proton (GE-Proton, as on Steam Deck)" }[$mode]
  if ($mode -eq "proton") {
    wsl -d $Distro -u root --exec test -x /opt/kit-proton/current/proton
    if ($LASTEXITCODE) { throw "GE-Proton isn't in $Distro yet: run this with -Setup -Proton first (ask the user: about 500 MB)" }
  }

  "running $Run on Linux$how and watching for $Wait seconds..."
  wsl -d $Distro -u root --cd $tmp --exec xvfb-run -a -s "-screen 0 1920x1080x24" bash kit-linux.sh run $pkg $Run $Wait $results $mode
  if ($LASTEXITCODE) { throw "the test itself failed in WSL (exit $LASTEXITCODE)" }
} finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$state = @{}
Get-Content (Join-Path $results "state.txt") | ForEach-Object { $k, $v = $_ -split "=", 2; $state[$k] = $v }
Remove-Item (Join-Path $results "state.txt")
# "$_": plain strings, or Windows PowerShell writes each line's file properties into the JSON
$windows = @(Get-Content (Join-Path $results "windows.txt") | Where-Object { $_ } | ForEach-Object { "$_" })
$errorWindows = @($windows | Where-Object { $_ -match "Program Error|error|exception|fatal|crash|not found|missing" })
# What a broken Linux or Wine run prints: missing libraries or DLLs, crashes, a Python traceback.
$printed = @(Get-Content (Join-Path $results "app.log") | Where-Object { $_ -match
  "error while loading shared libraries|cannot open shared object|err:module|Unhandled (exception|page fault)|segmentation fault|core dumped|command not found|: not found$|Traceback|^\s*(error|fatal)\b" } | ForEach-Object { "$_" })
$exit = if ($state.exit) { [int]$state.exit } else { $null }
$ok = [bool]($state.found -and $windows.Count -and -not $errorWindows.Count -and -not $printed.Count)
$verdict = if (-not $state.found) { "FAIL: $Run isn't in the package" }
  elseif ($errorWindows.Count) { "FAIL: error windows: $($errorWindows -join ' | ')" }
  elseif ($printed.Count) { "FAIL: it reported errors: $($printed[0].Trim())" }
  elseif (-not $windows.Count) { "FAIL: nothing is on screen at the end$(if ($null -ne $exit) { "; it exited with code $exit" })" }
  else { "PASS: it ran on Linux$how with no error windows or errors" }
[ordered]@{ run = $Run; mode = $mode; found = [bool]$state.found; running = [bool]$state.running; exitCode = $exit;
  windows = $windows; errorWindows = $errorWindows; errorLines = @($printed | Select-Object -First 20); pass = $ok;
  verdict = $verdict } | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $results "result.json")

"windows open at the end: $(if ($windows.Count) { $windows -join ' | ' } else { 'none' })"
$verdict
"look at the screenshots before trusting it: $results\shot-1.png, shot-2.png, shot-3.png (output in $results\app.log)"
exit $(if ($ok) { 0 } else { 1 })
