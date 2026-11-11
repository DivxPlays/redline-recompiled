# The kit's iOS test: there's no iOS on Windows, so GitHub's macOS runners build the app for the iOS Simulator, start
# it on a freshly booted simulated iPhone the way a new user's first run goes, and send back screenshots and its log.
# Usage: powershell -ExecutionPolicy Bypass -File .claude/ios-test.ps1 [-Path <folder with the Xcode project or XcodeGen project.yml, default
#          the repo root>] [-Scheme <scheme, default the first>] [-Wait 30] [-Ref <branch, default this one>]
#          [-Out build/ios-test]
#   -Setup  first time in a project: writes .github/workflows/kit-ios-test.yml. Commit and push it (it must be on the
#           default branch), then run the test.
# It tests the branch as it is on GitHub, so commits not pushed yet aren't in it (at release time, that's only the
# release commit itself). A run takes 2 to 10 minutes, depending on the app. macOS minutes are free in a public repo; in a private one each
# counts as 10 of the 2,000 free minutes a month, so a run uses about 30 (small app) to 100: tell the user before the first one.
# Results in -Out: result.json, shot-1/2/3.png, app.log, build.log. Look at the screenshots. Exit code 0 = it built,
# started and was still running at the end with no crash or error, 1 = not.
param([string]$Path = ".", [string]$Scheme = "", [int]$Wait = 30, [string]$Ref = "", [string]$Out = "build/ios-test",
      [switch]$Setup)
$ErrorActionPreference = "Stop"
$gh = if (Get-Command gh -ErrorAction SilentlyContinue) { "gh" } else { "C:\Program Files\GitHub CLI\gh.exe" }
$root = (git rev-parse --show-toplevel).Trim()
$wf = Join-Path $root ".github/workflows/kit-ios-test.yml"

$yml = @'
# The kit's iOS test, written by .claude/ios-test.ps1 -Setup and started by that script. Builds the app for the iOS
# Simulator, starts it on a freshly booted simulated iPhone, and saves screenshots, its log and result.json.
name: kit iOS test
on:
  workflow_dispatch:
    inputs:
      path: { description: "Folder with the Xcode project or project.yml", default: "." }
      scheme: { description: "Scheme (empty: the first one)", default: "" }
      wait: { description: "Seconds to watch it run", default: "30" }
permissions:
  contents: read
jobs:
  ios:
    runs-on: macos-latest
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4
      - name: Build, start on a simulated iPhone, watch
        working-directory: ${{ inputs.path }}
        env:
          SCHEME: ${{ inputs.scheme }}
          WAIT: ${{ inputs.wait }}
        run: |
          out="$GITHUB_WORKSPACE/ios-test"; mkdir -p "$out"
          built=false; launched=false; running=false; crashed=false; bid=""
          if [ -f project.yml ]; then command -v xcodegen >/dev/null || brew install -q xcodegen; xcodegen generate; fi
          if [ -f Podfile ]; then pod install; fi
          proj=$(ls -d *.xcworkspace 2>/dev/null | head -n1); flag=-workspace
          if [ -z "$proj" ]; then proj=$(ls -d *.xcodeproj 2>/dev/null | head -n1); flag=-project; fi
          if [ -z "$SCHEME" ]; then SCHEME=$(xcodebuild -list -json $flag "$proj" | jq -r '(.workspace // .project).schemes[0]'); fi
          udid=$(xcrun simctl list devices available -j | jq -r '[.devices[][] | select(.name | startswith("iPhone"))][0].udid')
          xcodebuild $flag "$proj" -scheme "$SCHEME" -sdk iphonesimulator -configuration Debug -destination "id=$udid" \
            -derivedDataPath "$RUNNER_TEMP/dd" CODE_SIGNING_ALLOWED=NO build > "$out/build.log" 2>&1 && built=true
          app=$(find "$RUNNER_TEMP/dd/Build/Products" -maxdepth 2 -name '*.app' 2>/dev/null | head -n1)
          if $built && [ -n "$app" ]; then
            bid=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")
            exe=$(/usr/libexec/PlistBuddy -c 'Print CFBundleExecutable' "$app/Info.plist")
            xcrun simctl boot "$udid"; xcrun simctl bootstatus "$udid" -b > /dev/null
            xcrun simctl install "$udid" "$app"
            xcrun simctl launch "$udid" "$bid" && launched=true
            for n in 1 2 3; do sleep $((WAIT / 3)); xcrun simctl io "$udid" screenshot "$out/shot-$n.png" > /dev/null 2>&1; done
            # still running: launchd lists the app with a process id, not "-"
            xcrun simctl spawn "$udid" launchctl list | grep "UIKitApplication:$bid" | awk '{print $1}' | grep -qE '^[0-9]+$' && running=true
            ls ~/Library/Logs/DiagnosticReports 2>/dev/null | grep -q "^$exe" && crashed=true
            xcrun simctl spawn "$udid" log show --last 10m --style compact --predicate "process == \"$exe\"" > "$out/app.log" 2>&1
          fi
          errors=$(grep -hE "Fatal error|Terminating app due to uncaught exception|Library not loaded|EXC_BAD_|SIGABRT" "$out/app.log" 2>/dev/null | head -n20)
          buildErrors=$(grep -E "error:" "$out/build.log" | head -n10)
          jq -n --arg scheme "$SCHEME" --arg app "$bid" --argjson built $built --argjson launched $launched \
            --argjson running $running --argjson crashed $crashed --arg errors "$errors" --arg buildErrors "$buildErrors" \
            '{scheme: $scheme, app: $app, built: $built, launched: $launched, running: $running, crashed: $crashed,
              errorLines: ($errors | split("\n") | map(select(length > 0))),
              buildErrors: ($buildErrors | split("\n") | map(select(length > 0)))}' > "$out/result.json"
      - uses: actions/upload-artifact@v4
        if: always()
        with: { name: ios-test, path: ios-test }
'@

if ($Setup) {
  New-Item -ItemType Directory -Force (Split-Path $wf) | Out-Null
  [System.IO.File]::WriteAllText($wf, $yml.Replace("`r`n", "`n"))
  "wrote .github/workflows/kit-ios-test.yml: commit and push it to the default branch, then run this again without -Setup"
  exit 0
}
if (-not (Test-Path $wf)) { throw "no iOS test in this project yet: run this with -Setup, then commit and push the workflow" }
if (-not $Ref) { $Ref = (git rev-parse --abbrev-ref HEAD).Trim() }

$since = [DateTimeOffset]::UtcNow.AddSeconds(-30)  # GitHub's clock and this PC's may differ a little
& $gh workflow run kit-ios-test.yml --ref $Ref -f "path=$Path" -f "scheme=$Scheme" -f "wait=$Wait"
if ($LASTEXITCODE) { throw "couldn't start the iOS test on GitHub (is kit-ios-test.yml pushed to the default branch?)" }
$deadline = (Get-Date).AddMinutes(2)
do {
  Start-Sleep -Seconds 4
  $run = & $gh run list --workflow kit-ios-test.yml --event workflow_dispatch --limit 5 --json databaseId,createdAt,url |
    ConvertFrom-Json | Where-Object { [DateTimeOffset]$_.createdAt -ge $since } | Select-Object -Last 1
} until ($run -or (Get-Date) -gt $deadline)
if (-not $run) { throw "the iOS test was started but its run didn't show up on GitHub" }

"building and running on a simulated iPhone on GitHub (2 to 10 minutes): $($run.url)"
& $gh run watch $run.databaseId --interval 20 | Out-Null
$results = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (Test-Path $results) { Remove-Item -Recurse -Force $results }
& $gh run download $run.databaseId -n ios-test -D $results
if ($LASTEXITCODE -or -not (Test-Path (Join-Path $results "result.json"))) { throw "the run left no results; read it on GitHub: $($run.url)" }

$r = Get-Content (Join-Path $results "result.json") -Raw | ConvertFrom-Json
$ok = [bool]($r.built -and $r.launched -and $r.running -and -not $r.crashed -and -not @($r.errorLines).Count)
$verdict = if (-not $r.built) { "FAIL: it didn't build for the iOS Simulator: $(@($r.buildErrors)[0]) (all of it in $results\build.log)" }
  elseif (-not $r.launched) { "FAIL: it built but didn't start on the simulated iPhone" }
  elseif ($r.crashed) { "FAIL: it crashed (a crash report came up)" }
  elseif (@($r.errorLines).Count) { "FAIL: it reported errors: $(@($r.errorLines)[0])" }
  elseif (-not $r.running) { "FAIL: it wasn't running at the end; it closed or crashed" }
  else { "PASS: $($r.app) built, started and kept running on a simulated iPhone" }
$r | Add-Member pass $ok -Force; $r | Add-Member verdict $verdict -Force  # for the release report
$r | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $results "result.json")
$verdict
"look at the screenshots before trusting it: $results\shot-1.png, shot-2.png, shot-3.png (its log in $results\app.log)"
exit $(if ($ok) { 0 } else { 1 })
