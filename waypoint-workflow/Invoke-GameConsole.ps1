[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^agpb_wp_[A-Za-z0-9_]+$')][string]$SessionName,
    [string]$SessionDirectory,
    [switch]$Listen,
    [string]$Commands = 'agpb_list',
    [ValidatePattern('^[A-Za-z0-9_-]*$')][string]$Screenshot = '',
    [ValidateRange(100,5000)][int]$DelayMilliseconds = 500,
    [ValidateRange(1,300)][int]$AckTimeoutSeconds = 15,
    [ValidateRange(1,300)][int]$ScreenshotTimeoutSeconds = 10,
    [string]$PythonExe = (Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe')
)
# Native engine commands only. No desktop input, process-memory access or automatic links.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ConsoleTransport.ps1')
if ($Listen) {
    Write-Output 'AGPB_NATIVE_SESSION_READY: JSON lines {commands,screenshot,delay,ackTimeoutSeconds,screenshotTimeoutSeconds}; END closes this helper only.'
    $defaults = @{
        SessionName = $SessionName; SessionDirectory = $SessionDirectory; PythonExe = $PythonExe
        Commands = $Commands; Screenshot = $Screenshot; DelayMilliseconds = $DelayMilliseconds
        AckTimeoutSeconds = $AckTimeoutSeconds; ScreenshotTimeoutSeconds = $ScreenshotTimeoutSeconds
    }
    while ($null -ne ($line = [Console]::ReadLine())) {
        if ($line -eq 'END') { break }
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try {
            $request = $line | ConvertFrom-Json
            if ($null -eq $request -or $request -is [array] -or $request -isnot [pscustomobject]) { throw 'Expected one JSON object.' }
            $argsForCall = Resolve-ConsoleRequestArguments -Request $request -Defaults $defaults
            & $PSCommandPath @argsForCall
        } catch {
            Write-Output ('AGPB_NATIVE_SESSION_ERROR: ' + $_.Exception.Message)
        }
        Write-Output 'AGPB_NATIVE_SESSION_READY'
    }
    return
}
$repo = Split-Path -Parent $PSScriptRoot
& git -C $repo status --short
$transportLock = $null
$lockHeld = $false
try {
    $requirements = Get-Content -LiteralPath (Join-Path $repo 'AGENTS.md') -Raw -Encoding utf8
    $match = [regex]::Match($requirements, '(?m)^- 部署目标：\s*`([^`]+)`')
    if (!$match.Success) { throw 'Cannot find deployment target in AGENTS.md.' }
    $gameDir = [IO.Path]::GetFullPath($match.Groups[1].Value)
    $gameRoot = Split-Path -Parent $gameDir
    $gameExe = Join-Path $gameRoot 'hl2_launcher.exe'
    $running = @(Get-Process -Name hl2_launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gameExe })
    if ($running.Count -ne 1) { throw 'Expected exactly one running target game. Launch it first.' }
    $gameProcessId = $running[0].Id
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $lockKey = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($gameExe.ToLowerInvariant()))).Replace('-', '') }
    finally { $hasher.Dispose() }
    $transportLock = [Threading.Mutex]::new($false, ('Local\AgPB_NativeConsole_' + $lockKey))
    try { $lockHeld = $transportLock.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $lockHeld = $true }
    if (!$lockHeld) { throw 'Another console submission is active for this game; serialize requests.' }
    if ($Commands -match '(?i)(?<![A-Za-z0-9_])jpeg(?![A-Za-z0-9_])') { throw 'JPEG is disabled in this workflow. Use -Screenshot (TGA).' }
    if ($Commands -match '[^\x09\x0A\x0D\x20-\x7E]') { throw 'Engine commands must be printable ASCII, tabs or newlines.' }
    if (!$SessionDirectory) { $SessionDirectory = Join-Path $env:TEMP $SessionName }
    $SessionDirectory = [IO.Path]::GetFullPath($SessionDirectory).TrimEnd('\')
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
    if (!$SessionDirectory.StartsWith($tempRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'SessionDirectory must be a child of the system TEMP directory.'
    }
    # Reject a session path whose existing parents redirect writes outside TEMP.
    $candidate = $SessionDirectory
    while (!$candidate.Equals($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $existing = Get-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
        if ($null -ne $existing -and (($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or !$existing.PSIsContainer)) {
            throw "Session directory must use ordinary TEMP directories: $candidate"
        }
        $candidate = Split-Path -Parent $candidate
    }
    New-Item -ItemType Directory -Path $SessionDirectory -Force | Out-Null
    foreach ($parent in @('cfg', 'screenshots')) {
        $link = Join-Path (Join-Path $gameDir $parent) $SessionName
        $entry = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
        if ($null -ne $entry) {
            $targets = @($entry.Target)
            if ($entry.LinkType -ne 'Junction' -or $targets.Count -ne 1 -or [IO.Path]::GetFullPath([string]$targets[0]).TrimEnd('\') -ine $SessionDirectory) {
                throw "Existing session path is not the expected junction: $link"
            }
        } else { New-Item -ItemType Junction -Path $link -Target $SessionDirectory | Out-Null }
    }
    function Invoke-NativeLauncher([string]$Arguments) {
        $current = @(Get-Process -Name hl2_launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gameExe })
        if ($current.Count -ne 1 -or $current[0].Id -ne $gameProcessId) { throw 'The target game instance changed; no launcher was started.' }
        $helper = Start-Process -FilePath $gameExe -WorkingDirectory $gameRoot -ArgumentList $Arguments -WindowStyle Hidden -PassThru
        try {
            if (!$helper.WaitForExit(10000)) { throw 'Native launcher did not finish in 10 seconds; inspect the running game.' }
            if ($helper.ExitCode -ne 0) { throw "Native launcher failed: $($helper.ExitCode)" }
        } finally { $helper.Dispose() }
    }
    $nonce = [Guid]::NewGuid().ToString('N')
    $logPath = Join-Path $SessionDirectory 'console.log'
    $cursor = Get-ConsoleLogCursor -LogPath $logPath
    $cfgName = 'cmd_' + $nonce
    $cfg = @("con_logfile cfg/$SessionName/console.log", "echo AGPB_SESSION_BEGIN_$nonce", $Commands, "echo AGPB_SESSION_END_$nonce")
    $cfg | Set-Content -LiteralPath (Join-Path $SessionDirectory ($cfgName + '.cfg')) -Encoding ascii
    Invoke-NativeLauncher "-hijack +exec $SessionName/$cfgName"
    $ack = Wait-ConsoleAcknowledgement -LogPath $logPath -Cursor $cursor -Nonce $nonce -TimeoutSeconds $AckTimeoutSeconds
    Write-Output $ack.Text
    $ackMetadata = [ordered]@{
        nonce = $ack.Nonce; beginMarker = $ack.BeginMarker; endMarker = $ack.EndMarker
        logPath = $logPath; endByteOffset = $ack.EndByteOffset; logResetCount = $ack.LogResetCount
        meaning = 'engine processed CFG markers; asynchronous actions may remain pending'
    }
    Write-Output ('CONSOLE_ACK: ' + ($ackMetadata | ConvertTo-Json -Compress))
    # This optional settling interval is never treated as proof of an action completing.
    Start-Sleep -Milliseconds $DelayMilliseconds
    if ($Screenshot) {
        $imageName = $Screenshot + '_' + $nonce
        Invoke-NativeLauncher "-hijack +screenshot $SessionName/$imageName"
        $tga = Join-Path $SessionDirectory ($imageName + '.tga')
        $png = Join-Path $SessionDirectory ($imageName + '.png')
        if (!(Test-Path -LiteralPath $PythonExe -PathType Leaf)) { throw "TGA requested at $tga; pass a PythonExe with Pillow installed." }
        & $PythonExe -B (Join-Path $PSScriptRoot 'convert_tga.py') $tga $png --timeout $ScreenshotTimeoutSeconds
        if ($LASTEXITCODE -ne 0) { throw "TGA completion/conversion failed; inspect $tga" }
        Write-Output "SCREENSHOT: $png"
    }
    Write-Output "SESSION: $SessionDirectory"
    Write-Output 'CFG acknowledgement confirms engine processing only; inspect returned state for asynchronous action completion.'
} finally {
    if ($lockHeld) { $transportLock.ReleaseMutex() }
    if ($null -ne $transportLock) { $transportLock.Dispose() }
    & git -C $repo status --short
}
