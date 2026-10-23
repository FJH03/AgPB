[CmdletBinding()]
param(
    [string]$PythonExe = (Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
& git -C $repo status --short
$testDirectory = Join-Path $env:TEMP ('agpb-console-transport-test-' + [Guid]::NewGuid().ToString('N'))
try {
    . (Join-Path $PSScriptRoot 'ConsoleTransport.ps1')
    New-Item -ItemType Directory -Path $testDirectory | Out-Null
    function Assert-True([bool]$Condition, [string]$Message) {
        if (!$Condition) { throw "ASSERTION FAILED: $Message" }
    }
    function Assert-Timeout([scriptblock]$Action, [string]$Message) {
        $timedOut = $false
        try { & $Action | Out-Null }
        catch {
            if ($_.Exception.Message -like 'Timed out waiting*') { $timedOut = $true }
            else { throw }
        }
        Assert-True $timedOut $Message
    }
    function Start-Writer([scriptblock]$Script, [object[]]$Arguments) {
        $runner = [PowerShell]::Create()
        $null = $runner.AddScript($Script.ToString())
        foreach ($argument in $Arguments) { $null = $runner.AddArgument($argument) }
        [pscustomobject]@{ Runner = $runner; Pending = $runner.BeginInvoke() }
    }
    function Complete-Writer($Job) {
        try {
            $null = $Job.Runner.EndInvoke($Job.Pending)
            if ($Job.Runner.HadErrors) { throw ('Writer failed: ' + ($Job.Runner.Streams.Error | Out-String)) }
        } finally { $Job.Runner.Dispose() }
    }
    $log = Join-Path $testDirectory 'console.log'
    $encoding = [Text.Encoding]::ASCII

    # A same-nonce historical reply and an echoed command are not a fresh reply.
    [IO.File]::WriteAllText($log, "AGPB_SESSION_BEGIN_old`nAGPB_SESSION_END_old`n", $encoding)
    $cursor = Get-ConsoleLogCursor $log
    [IO.File]::AppendAllText($log, "echo AGPB_SESSION_BEGIN_old`necho AGPB_SESSION_END_old`n", $encoding)
    Assert-Timeout { Wait-ConsoleAcknowledgement $log $cursor old -TimeoutSeconds 0.2 -PollMilliseconds 10 } 'Old bytes and echoed commands must not acknowledge a submission.'
    Write-Output 'PASS: old log bytes and echoed marker commands are rejected.'

    # Source's echo implementation appends a space after each argument.
    [IO.File]::WriteAllText($log, '', $encoding)
    $cursor = Get-ConsoleLogCursor $log
    [IO.File]::AppendAllText($log, "AGPB_SESSION_BEGIN_echo `t`r`nengine output`r`nAGPB_SESSION_END_echo `t `r`n", $encoding)
    $ack = Wait-ConsoleAcknowledgement $log $cursor echo -TimeoutSeconds 1 -PollMilliseconds 10
    Assert-True ($ack.Text.Contains('engine output')) 'Real Source echo trailing space/tab must acknowledge.'
    [IO.File]::WriteAllText($log, '', $encoding)
    $cursor = Get-ConsoleLogCursor $log
    [IO.File]::AppendAllText($log, "AGPB_SESSION_BEGIN_suffix extra`r`nAGPB_SESSION_END_suffix `r`n", $encoding)
    Assert-Timeout { Wait-ConsoleAcknowledgement $log $cursor suffix -TimeoutSeconds 0.15 -PollMilliseconds 10 } 'Additional marker suffix content must not acknowledge.'
    [IO.File]::WriteAllText($log, '', $encoding)
    $cursor = Get-ConsoleLogCursor $log
    [IO.File]::AppendAllText($log, " AGPB_SESSION_BEGIN_leading `r`nAGPB_SESSION_END_leading `r`n", $encoding)
    Assert-Timeout { Wait-ConsoleAcknowledgement $log $cursor leading -TimeoutSeconds 0.15 -PollMilliseconds 10 } 'Leading text/whitespace must not acknowledge.'
    Write-Output 'PASS: real Source echo whitespace/CRLF is accepted; extra suffixes and leading whitespace are rejected.'
    # A partially written BEGIN and an END without its newline must be awaited.
    $cursor = Get-ConsoleLogCursor $log
    $writer = Start-Writer {
        param($path)
        $enc = [Text.Encoding]::ASCII
        Start-Sleep -Milliseconds 70
        [IO.File]::AppendAllText($path, 'AGPB_SESSION_BEG', $enc)
        Start-Sleep -Milliseconds 40
        [IO.File]::AppendAllText($path, "IN_delayed`r`nnew state`r`n", $enc)
        Start-Sleep -Milliseconds 70
        [IO.File]::AppendAllText($path, 'AGPB_SESSION_END_delayed', $enc)
        Start-Sleep -Milliseconds 90
        [IO.File]::AppendAllText($path, "`r`npost-ack", $enc)
    } @($log)
    try { $ack = Wait-ConsoleAcknowledgement $log $cursor delayed -TimeoutSeconds 3 -PollMilliseconds 10 }
    finally { Complete-Writer $writer }
    Assert-True ($ack.Text.Contains('new state')) 'Acknowledgement must include this submission state.'
    Assert-True ($ack.ElapsedSeconds -ge 0.20) 'Partial END must not acknowledge before its newline.'
    $raw = [IO.File]::ReadAllBytes($log)
    $suffix = $encoding.GetString($raw, [int]$ack.EndByteOffset, $raw.Length - [int]$ack.EndByteOffset)
    Assert-True ($suffix -eq 'post-ack') 'EndByteOffset must be an exact byte cursor after the END line.'
    Write-Output 'PASS: delayed, fragmented reply waits for full lines and returns an exact byte cursor.'

    # Resetting the log must discard a BEGIN observed before truncation.
    [IO.File]::WriteAllText($log, ('x' * 512) + "`n", $encoding)
    $cursor = Get-ConsoleLogCursor $log
    $writer = Start-Writer {
        param($path)
        $enc = [Text.Encoding]::ASCII
        Start-Sleep -Milliseconds 50
        [IO.File]::AppendAllText($path, "AGPB_SESSION_BEGIN_reset`nold generation`n", $enc)
        Start-Sleep -Milliseconds 100
        [IO.File]::WriteAllText($path, "AGPB_SESSION_END_reset`n", $enc)
        Start-Sleep -Milliseconds 120
        [IO.File]::WriteAllText($path, "AGPB_SESSION_BEGIN_reset`nnew generation`nAGPB_SESSION_END_reset`n", $enc)
    } @($log)
    try { $ack = Wait-ConsoleAcknowledgement $log $cursor reset -TimeoutSeconds 3 -PollMilliseconds 10 }
    finally { Complete-Writer $writer }
    Assert-True ($ack.Text.Contains('new generation') -and !$ack.Text.Contains('old generation')) 'A new BEGIN is required after truncation.'
    Assert-True ($ack.LogResetCount -ge 1 -and $ack.ElapsedSeconds -ge 0.2) 'Truncation must be detected before accepting fresh reply.'
    Write-Output 'PASS: truncation invalidates the old BEGIN and waits for a new complete pair.'

    # Same-size replacement must also invalidate the old byte cursor.
    [IO.File]::WriteAllText($log, ('x' * 200) + "`n", $encoding)
    $cursor = Get-ConsoleLogCursor $log
    $reply = "AGPB_SESSION_BEGIN_replace`nreplacement`nAGPB_SESSION_END_replace`n"
    [IO.File]::WriteAllText($log, $reply + ('z' * (201 - $reply.Length)), $encoding)
    $ack = Wait-ConsoleAcknowledgement $log $cursor replace -TimeoutSeconds 1 -PollMilliseconds 10
    Assert-True ($ack.LogResetCount -eq 1 -and $ack.Text.Contains('replacement')) 'Tail anchor must detect same-size replacement.'
    Write-Output 'PASS: same-size log replacement is detected by the byte anchor.'

    # A BEGIN on a partial pre-existing line is not a standalone marker.
    [IO.File]::WriteAllText($log, 'partial-prefix', $encoding)
    $cursor = Get-ConsoleLogCursor $log
    [IO.File]::AppendAllText($log, "AGPB_SESSION_BEGIN_partial`nAGPB_SESSION_END_partial`n", $encoding)
    Assert-Timeout { Wait-ConsoleAcknowledgement $log $cursor partial -TimeoutSeconds 0.15 -PollMilliseconds 10 } 'A partial-line suffix must not count as BEGIN.'
    [IO.File]::WriteAllText($log, "AGPB_SESSION_BEGIN_timeout`n", $encoding)
    $cursor = [pscustomobject]@{ Offset = [long]0; Anchor = [byte[]]@(); SkipPartialLine = $false }
    Assert-Timeout { Wait-ConsoleAcknowledgement $log $cursor timeout -TimeoutSeconds 0.15 -PollMilliseconds 10 } 'Missing END must time out.'
    $missingLog = Join-Path $testDirectory 'not-created.log'
    $cursor = Get-ConsoleLogCursor $missingLog
    Assert-Timeout { Wait-ConsoleAcknowledgement $missingLog $cursor missing -TimeoutSeconds 0.15 -PollMilliseconds 10 } 'Missing log must time out.'
    Write-Output 'PASS: partial old lines, missing END, and missing logs fail with bounded timeouts.'

    $defaults = @{
        SessionName = 'agpb_wp_test'; SessionDirectory = $testDirectory; PythonExe = $PythonExe
        Commands = 'agpb_list'; Screenshot = 'default'; DelayMilliseconds = 700
        AckTimeoutSeconds = 23; ScreenshotTimeoutSeconds = 17
    }
    $resolved = Resolve-ConsoleRequestArguments ('{}' | ConvertFrom-Json) $defaults
    Assert-True ($resolved.DelayMilliseconds -eq 700 -and $resolved.AckTimeoutSeconds -eq 23 -and $resolved.ScreenshotTimeoutSeconds -eq 17 -and $resolved.Screenshot -eq 'default') 'Listen defaults must inherit all direct-call options.'
    $resolved = Resolve-ConsoleRequestArguments ('{"commands":"agpb_wp_list","screenshot":"","delay":150,"ackTimeoutSeconds":4,"screenshotTimeoutSeconds":5}' | ConvertFrom-Json) $defaults
    Assert-True ($resolved.Commands -eq 'agpb_wp_list' -and $resolved.Screenshot -eq '' -and $resolved.DelayMilliseconds -eq 150 -and $resolved.AckTimeoutSeconds -eq 4 -and $resolved.ScreenshotTimeoutSeconds -eq 5) 'Listen request values must override direct-call defaults.'
    Write-Output 'PASS: Listen defaults and per-request timeout/delay overrides agree with direct calls.'

    # A complete TGA header with insufficient pixels remains undecodable past the stable interval.
    $tga = Join-Path $testDirectory 'fragmented.tga'
    $png = Join-Path $testDirectory 'fragmented.png'
    $header = [byte[]]@(0,0,2,0,0,0,0,0,0,0,0,0,2,0,2,0,24,32)
    $pixels = [byte[]]@(0,0,255,0,255,0,255,0,0,255,255,255)
    [IO.File]::WriteAllBytes($tga, $header + $pixels[0..2])
    $writer = Start-Writer {
        param($path, $destination, [byte[]]$tail)
        Start-Sleep -Milliseconds 450
        if ([IO.File]::Exists($destination)) { throw 'PNG was reported before TGA pixel payload completed.' }
        $out = [IO.File]::Open($path, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        try { $out.Write($tail, 0, $tail.Length) } finally { $out.Dispose() }
    } @($tga, $png, [byte[]]$pixels[3..11])
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        $conversion = & $PythonExe -B (Join-Path $PSScriptRoot 'convert_tga.py') $tga $png --timeout 3 --poll-ms 20 --stable-ms 100 2>&1
        $conversionExit = $LASTEXITCODE
    } finally { Complete-Writer $writer }
    Assert-True ($conversionExit -eq 0 -and (Test-Path -LiteralPath $png)) ('Complete TGA should eventually convert: ' + ($conversion | Out-String))
    Assert-True ($clock.Elapsed.TotalSeconds -ge 0.45) 'An incomplete TGA must not convert merely because its size is stable.'
    $verify = & $PythonExe -B -c 'from PIL import Image; import sys; im=Image.open(sys.argv[1]); im.load(); assert im.format == "PNG" and im.size == (2, 2); assert im.getpixel((0,0)) == (255,0,0); print("pixels verified")' $png 2>&1
    Assert-True ($LASTEXITCODE -eq 0) ('Converted PNG pixel validation failed: ' + ($verify | Out-String))
    Write-Output 'PASS: stable but incomplete TGA is retried until all pixels arrive; resulting PNG pixels decode correctly.'

    # A valid but still changing capture must settle at its final pixel dimensions.
    $changingTga = Join-Path $testDirectory 'changing-dimensions.tga'
    $changingPng = Join-Path $testDirectory 'changing-dimensions.png'
    [IO.File]::WriteAllBytes($changingTga, $header + $pixels)
    $finalHeader = [byte[]]$header.Clone()
    $finalHeader[12] = 3; $finalHeader[14] = 1
    $writer = Start-Writer {
        param($path, $destination, [byte[]]$image)
        Start-Sleep -Milliseconds 150
        if ([IO.File]::Exists($destination)) { throw 'PNG was created before dimensions settled.' }
        [IO.File]::WriteAllBytes($path, $image)
    } @($changingTga, $changingPng, [byte[]]($finalHeader + $pixels[0..8]))
    try {
        $conversion = & $PythonExe -B (Join-Path $PSScriptRoot 'convert_tga.py') $changingTga $changingPng --timeout 3 --poll-ms 20 --stable-ms 250 2>&1
        $conversionExit = $LASTEXITCODE
    } finally { Complete-Writer $writer }
    Assert-True ($conversionExit -eq 0) ('Changing dimensions should eventually settle: ' + ($conversion | Out-String))
    $verify = & $PythonExe -B -c 'from PIL import Image; import sys; im=Image.open(sys.argv[1]); im.load(); assert im.size == (3, 1)' $changingPng 2>&1
    Assert-True ($LASTEXITCODE -eq 0) ('Only the final stable dimensions should be saved: ' + ($verify | Out-String))
    Write-Output 'PASS: valid TGA dimension/size changes restart the stability interval.'
    $badTga = Join-Path $testDirectory 'incomplete.tga'
    $badPng = Join-Path $testDirectory 'incomplete.png'
    [IO.File]::WriteAllBytes($badTga, $header)
    $failure = & $PythonExe -B (Join-Path $PSScriptRoot 'convert_tga.py') $badTga $badPng --timeout 0.2 --poll-ms 20 --stable-ms 50 2>&1
    Assert-True ($LASTEXITCODE -ne 0 -and !(Test-Path -LiteralPath $badPng)) 'A permanently incomplete TGA must fail without a success PNG.'
    Assert-True (($failure | Out-String).Contains('not complete and stable')) 'TGA timeout must explain incomplete capture.'
    Write-Output 'PASS: incomplete TGA times out without creating a success PNG.'
    $beforeHash = (Get-FileHash -LiteralPath $png).Hash
    $failure = & $PythonExe -B (Join-Path $PSScriptRoot 'convert_tga.py') $tga $png --timeout 0.5 --poll-ms 20 --stable-ms 20 2>&1
    Assert-True ($LASTEXITCODE -ne 0 -and (Get-FileHash -LiteralPath $png).Hash -eq $beforeHash) 'An old PNG must be rejected and preserved.'
    Write-Output 'PASS: an existing PNG cannot be overwritten or reported as a fresh capture.'
    Write-Output 'ALL CONSOLE TRANSPORT TESTS PASSED (no game process launched).'
} finally {
    Write-Output "TEST_ARTIFACTS_RETAINED: $testDirectory"
    & git -C $repo status --short
}
