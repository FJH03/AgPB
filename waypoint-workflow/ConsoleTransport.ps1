# Native-console transport primitives. No game launch occurs when dot-sourced.
Set-StrictMode -Version Latest

function Get-ConsoleLogCursor {
    param([Parameter(Mandatory)][string]$LogPath)
    $offset = [long]0
    $anchor = [byte[]]@()
    $skipPartialLine = $false
    if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
        $stream = [IO.File]::Open($LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            $offset = $stream.Length
            $count = [int][Math]::Min(128, $offset)
            if ($count -gt 0) {
                $anchor = New-Object byte[] $count
                $null = $stream.Seek($offset - $count, [IO.SeekOrigin]::Begin)
                if ($stream.Read($anchor, 0, $count) -ne $count) { throw 'Console log changed while taking its cursor; retry submission.' }
                $skipPartialLine = $anchor[$count - 1] -ne 10
            }
        } finally { $stream.Dispose() }
    }
    [pscustomobject]@{ Offset = $offset; Anchor = $anchor; SkipPartialLine = $skipPartialLine }
}

function Wait-ConsoleAcknowledgement {
    param(
        [Parameter(Mandatory)][string]$LogPath,
        [Parameter(Mandatory)]$Cursor,
        [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9]+$')][string]$Nonce,
        [ValidateRange(0.05,300)][double]$TimeoutSeconds = 15,
        [ValidateRange(5,1000)][int]$PollMilliseconds = 50
    )
    $beginMarker = 'AGPB_SESSION_BEGIN_' + $Nonce
    $endMarker = 'AGPB_SESSION_END_' + $Nonce
    $offset = [long]$Cursor.Offset
    $anchor = [byte[]]$Cursor.Anchor
    $skipPartialLine = [bool]$Cursor.SkipPartialLine
    $pending = ''
    $seenBegin = $false
    $captured = [Text.StringBuilder]::new()
    $resets = 0
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $lastReadError = ''
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $stream = $null
        $reset = $false
        $chunk = [byte[]]@()
        try {
            if (!(Test-Path -LiteralPath $LogPath -PathType Leaf)) {
                $reset = $offset -gt 0 -or $pending.Length -gt 0 -or $seenBegin
            } else {
                $stream = [IO.File]::Open($LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
                $reset = $stream.Length -lt $offset
                if (!$reset -and $anchor.Length -gt 0) {
                    $probe = New-Object byte[] $anchor.Length
                    $null = $stream.Seek($offset - $anchor.Length, [IO.SeekOrigin]::Begin)
                    if ($stream.Read($probe, 0, $probe.Length) -ne $probe.Length) { $reset = $true }
                    else {
                        for ($i = 0; $i -lt $probe.Length; $i++) {
                            if ($probe[$i] -ne $anchor[$i]) { $reset = $true; break }
                        }
                    }
                }
                if ($reset) { $offset = 0 }
                $null = $stream.Seek($offset, [IO.SeekOrigin]::Begin)
                $count = [int][Math]::Min(1048576, [Math]::Max(0, $stream.Length - $offset))
                if ($count -gt 0) {
                    $buffer = New-Object byte[] $count
                    $read = $stream.Read($buffer, 0, $count)
                    if ($read -gt 0) {
                        if ($read -eq $count) { $chunk = $buffer }
                        else { $chunk = New-Object byte[] $read; [Array]::Copy($buffer, $chunk, $read) }
                        $offset += $read
                    }
                }
                $anchorCount = [int][Math]::Min(128, $offset)
                $anchor = New-Object byte[] $anchorCount
                if ($anchorCount -gt 0) {
                    $null = $stream.Seek($offset - $anchorCount, [IO.SeekOrigin]::Begin)
                    if ($stream.Read($anchor, 0, $anchorCount) -ne $anchorCount) {
                        $reset = $true; $offset = 0; $anchor = [byte[]]@(); $chunk = [byte[]]@()
                    }
                }
            }
        } catch [IO.IOException] {
            $lastReadError = $_.Exception.Message
            $chunk = [byte[]]@()
        } finally { if ($null -ne $stream) { $stream.Dispose() } }
        if ($reset) {
            $resets++
            $pending = ''; $seenBegin = $false; $skipPartialLine = $false
            $null = $captured.Clear()
            if ($null -eq $stream) { $offset = 0; $anchor = [byte[]]@() }
        }
        if ($chunk.Length -gt 0) {
            # Markers are ASCII. Preserve byte positions even if unrelated log text is GBK.
            $pending += [Text.Encoding]::ASCII.GetString($chunk)
            while (($newline = $pending.IndexOf("`n")) -ge 0) {
                $line = $pending.Substring(0, $newline).TrimEnd("`r")
                $pending = $pending.Substring($newline + 1)
                if ($skipPartialLine) { $skipPartialLine = $false; continue }
                # Source echo emits a trailing space after each argument.
                # Only horizontal ASCII suffix whitespace is allowed on a marker line.
                $markerLine = $line.TrimEnd([char[]]" 	")
                if ($markerLine -ceq $beginMarker) {
                    $seenBegin = $true
                    $null = $captured.Clear()
                }
                if ($seenBegin) {
                    $null = $captured.AppendLine($line)
                    if ($markerLine -ceq $endMarker) {
                        return [pscustomobject]@{
                            Nonce = $Nonce; BeginMarker = $beginMarker; EndMarker = $endMarker
                            LogPath = $LogPath; Text = $captured.ToString()
                            EndByteOffset = $offset - $pending.Length
                            LogResetCount = $resets; ElapsedSeconds = $clock.Elapsed.TotalSeconds
                        }
                    }
                    if ($captured.Length -gt 16777216) { throw 'Console acknowledgement exceeded the 16 MiB capture limit.' }
                }
            }
            if ($pending.Length -gt 1048576) { throw 'Console log contains an unterminated line over 1 MiB.' }
        }
        $remaining = $TimeoutSeconds * 1000 - $clock.Elapsed.TotalMilliseconds
        if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Min($PollMilliseconds, [Math]::Ceiling($remaining))) }
    }
    $detail = if ($lastReadError) { " Last read error: $lastReadError" } else { '' }
    throw "Timed out waiting for this CFG's complete BEGIN/END acknowledgement ($Nonce) in $LogPath.$detail No asynchronous action completion is implied."
}

function Resolve-ConsoleRequestArguments {
    param([Parameter(Mandatory)]$Request, [Parameter(Mandatory)][hashtable]$Defaults)
    $result = @{}
    foreach ($key in $Defaults.Keys) { $result[$key] = $Defaults[$key] }
    $mapping = @{
        commands = 'Commands'; screenshot = 'Screenshot'; delay = 'DelayMilliseconds'
        ackTimeoutSeconds = 'AckTimeoutSeconds'; screenshotTimeoutSeconds = 'ScreenshotTimeoutSeconds'
    }
    foreach ($property in $Request.PSObject.Properties) {
        if (!$mapping.ContainsKey($property.Name)) { throw "Unknown console request property: $($property.Name)" }
        $target = $mapping[$property.Name]
        if ($target -in @('Commands', 'Screenshot')) { $result[$target] = [string]$property.Value }
        else { $result[$target] = [int]$property.Value }
    }
    return $result
}
