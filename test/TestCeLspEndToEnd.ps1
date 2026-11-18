param()

# Protocol-level LSP harness.  It drives a scripted stdio server and checks
# framing/stream separation.  It deliberately does not load Notepad3/Win32 or
# claim to test client UI behavior.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lsp-e2e-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

$failures = 0
function Check([bool]$condition, [string]$name) {
    if (!$condition) {
        Write-Host "FAIL: $name"
        $script:failures++
    }
}

function Encode-Frame([string]$json) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    return "Content-Length: $($bytes.Length)`r`n`r`n" + [Text.Encoding]::UTF8.GetString($bytes)
}

function New-Frame([hashtable]$message) {
    return Encode-Frame (ConvertTo-Json $message -Compress -Depth 12)
}

function Find-Response([object[]]$frames, [int]$id) {
    foreach ($frame in $frames) {
        try {
            $json = ([string]$frame | ConvertFrom-Json)
            if ($null -ne $json.id -and [int]$json.id -eq $id -and $null -ne $json.result) {
                return [string]$frame
            }
        }
        catch { }
    }
    return $null
}

function Find-Method([object[]]$frames, [string]$method) {
    foreach ($frame in $frames) {
        try {
            $json = ([string]$frame | ConvertFrom-Json)
            if ($json.method -eq $method) { return $true }
        }
        catch { }
    }
    return $false
}

function Find-DiagnosticLine([object[]]$frames, [int]$line) {
    $found = $false
    foreach ($frame in $frames) {
        try {
            $json = ([string]$frame | ConvertFrom-Json)
            if ($json.method -eq 'textDocument/publishDiagnostics') {
                foreach ($diag in @($json.params.diagnostics)) {
                    if ($null -ne $diag.range.start.line -and [int]$diag.range.start.line -eq $line) {
                        $found = $true
                    }
                }
            }
        }
        catch { }
    }
    return $found
}

function Invoke-Server([string]$serverExe, [string[]]$frames) {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $serverExe
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [Diagnostics.Process]::Start($psi)
    $writer = New-Object IO.StreamWriter($proc.StandardInput.BaseStream, (New-Object System.Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    foreach ($frame in $frames) { $writer.Write($frame) }
    $writer.Close()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    if (!$proc.WaitForExit(10000)) {
        $proc.Kill()
        throw 'CeLspServer.exe did not exit within 10s'
    }
    [void]$stderrTask.GetAwaiter().GetResult()
    $stdoutStream = [IO.MemoryStream]::new()
    $proc.StandardOutput.BaseStream.CopyTo($stdoutStream)
    return @(Decode-Frames $stdoutStream.ToArray())
}

function Decode-Frames([byte[]]$bytes) {
    $crlf = [string]([char]13) + [string]([char]10)
    $frames = [Collections.Generic.List[string]]::new()
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $head = [Text.Encoding]::ASCII.GetString($bytes, $offset, $bytes.Length - $offset)
        $headerEnd = $head.IndexOf(($crlf + $crlf), [StringComparison]::Ordinal)
        if ($headerEnd -lt 0) { break }
        $m = [regex]::Match($head.Substring(0, $headerEnd), '(?im)^Content-Length:\s*(\d+)\s*$')
        if (!$m.Success) { break }
        $len = [int]$m.Groups[1].Value
        $bodyStart = $offset + $headerEnd + 4
        if ($bytes.Length -lt ($bodyStart + $len)) { break }
        $frames.Add([Text.Encoding]::UTF8.GetString($bytes, $bodyStart, $len))
        $offset = $bodyStart + $len
    }
    return $frames
}

$fakeServer = @'
$ErrorActionPreference = 'Stop'
$stdin = [Console]::OpenStandardInput()
$stdout = [Console]::OpenStandardOutput()
$utf8 = New-Object System.Text.UTF8Encoding($false)
$crlf = [string]([char]13) + [string]([char]10)
function Write-Frame([string]$json) {
    $bytes = $utf8.GetBytes($json)
    $header = $utf8.GetBytes("Content-Length: $($bytes.Length)$crlf$crlf")
    $stdout.Write($header, 0, $header.Length)
    $stdout.Write($bytes, 0, $bytes.Length)
    $stdout.Flush()
}
[Console]::Error.WriteLine('non-protocol log line')
[Console]::Error.Flush()
$buffer = New-Object byte[] 8192
$text = ''
while (($n = $stdin.Read($buffer, 0, $buffer.Length)) -gt 0) {
    $text += $utf8.GetString($buffer, 0, $n)
    while ($true) {
        $headerEnd = $text.IndexOf(($crlf + $crlf), [StringComparison]::Ordinal)
        if ($headerEnd -lt 0) { break }
        $m = [regex]::Match($text.Substring(0, $headerEnd), '(?im)^Content-Length:\s*(\d+)\s*$')
        if (!$m.Success) { break }
        $len = [int]$m.Groups[1].Value
        $bodyStart = $headerEnd + 4
        if ($text.Length -lt ($bodyStart + $len)) { break }
        $body = $text.Substring($bodyStart, $len)
        $text = $text.Substring($bodyStart + $len)
        $msg = $body | ConvertFrom-Json
        if ($msg.method -eq 'initialize' -and $msg.id) {
            Write-Frame '{"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}}'
        }
    }
}
'@

try {
    $serverPath = Join-Path $temp 'FakeLspServer.ps1'
    [IO.File]::WriteAllText($serverPath, $fakeServer, (New-Object System.Text.UTF8Encoding($false)))

    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Get-Command pwsh).Source
    $psi.ArgumentList.Add('-NoProfile')
    $psi.ArgumentList.Add('-File')
    $psi.ArgumentList.Add($serverPath)
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [Diagnostics.Process]::Start($psi)

    $writer = New-Object IO.StreamWriter($proc.StandardInput.BaseStream, (New-Object System.Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    $writer.Write((Encode-Frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'))
    $writer.Close()

    $stderrTask = $proc.StandardError.ReadToEndAsync()
    if (!$proc.WaitForExit(10000)) {
        $proc.Kill()
        throw 'Fake LSP server did not exit within 10s'
    }
    $stdoutStream = [IO.MemoryStream]::new()
    $proc.StandardOutput.BaseStream.CopyTo($stdoutStream)
    $stdoutBytes = $stdoutStream.ToArray()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $frames = @(Decode-Frames $stdoutBytes)

    Check ($frames.Count -eq 1) 'stdout contains exactly one frame'
    if ($frames.Count -gt 0) {
        $jsonText = [string]$frames[0]
        $json = $jsonText | ConvertFrom-Json
        Check ($json.id -eq 1) 'response id is 1'
        Check ($null -ne $json.result.capabilities) 'response carries capabilities'
    }
    Check ($stderr.Contains('non-protocol log line')) 'stderr marker captured separately'
    Check (!([Text.Encoding]::UTF8.GetString($stdoutBytes).Contains('non-protocol log line'))) 'stderr text absent from stdout'

    $serverExe = Join-Path $root 'Bin\Release_x64_v145\CeLspServer.exe'
    Check (Test-Path $serverExe) 'CeLspServer.exe built for incremental sync test'
    if (Test-Path $serverExe) {
        $uri = 'file:///c%3A/scripts/incremental.cea'
        $initialText = "[ENABLE]`nlabel(a)`n"
        $insertText = "label(b)`n"
        $finalText = $initialText + $insertText
        $init = New-Frame @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{ processId = $PID; rootUri = $null; capabilities = @{ positionEncodings = @('utf-8') }; workspaceFolders = $null } }
        $initialized = New-Frame @{ jsonrpc = '2.0'; method = 'initialized'; params = @{} }
        $didOpen = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didOpen'; params = @{ textDocument = @{ uri = $uri; languageId = 'ce'; version = 1; text = $initialText } } }
        $didChangeFull = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $uri; version = 2 }; contentChanges = @(@{ text = $finalText }) } }
        $didChangeIncremental = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $uri; version = 2 }; contentChanges = @(@{ range = @{ start = @{ line = 2; character = 0 }; end = @{ line = 2; character = 0 } }; text = $insertText }) } }
        $symbolRequest = New-Frame @{ jsonrpc = '2.0'; id = 5; method = 'textDocument/documentSymbol'; params = @{ textDocument = @{ uri = $uri } } }

        $fullFrames = Invoke-Server $serverExe @($init, $initialized, $didOpen, $didChangeFull, $symbolRequest)
        $incFrames = Invoke-Server $serverExe @($init, $initialized, $didOpen, $didChangeIncremental, $symbolRequest)
        $fullResponse = Find-Response $fullFrames 5
        $incResponse = Find-Response $incFrames 5
        Check ($null -ne $fullResponse) 'full sync produced documentSymbol response'
        Check ($null -ne $incResponse) 'incremental sync produced documentSymbol response'
        if ($fullResponse -and $incResponse) {
            Check ($fullResponse -eq $incResponse) 'incremental didChange leaves the same server text as full didChange'
        }

        $midUri = 'file:///c%3A/scripts/middle-insert.cea'
        $midFull = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $midUri; version = 2 }; contentChanges = @(@{ text = "aXbcdef`n" }) } }
        $midIncremental = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $midUri; version = 2 }; contentChanges = @(@{ range = @{ start = @{ line = 0; character = 1 }; end = @{ line = 0; character = 1 } }; text = 'X' }) } }
        $midOpen = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didOpen'; params = @{ textDocument = @{ uri = $midUri; languageId = 'ce'; version = 1; text = "abcdef`n" } } }
        $midSymbol = New-Frame @{ jsonrpc = '2.0'; id = 6; method = 'textDocument/documentSymbol'; params = @{ textDocument = @{ uri = $midUri } } }
        $midFullFrames = Invoke-Server $serverExe @($init, $initialized, $midOpen, $midFull, $midSymbol)
        $midIncFrames = Invoke-Server $serverExe @($init, $initialized, $midOpen, $midIncremental, $midSymbol)
        $midFullResponse = Find-Response $midFullFrames 6
        $midIncResponse = Find-Response $midIncFrames 6
        Check ($null -ne $midFullResponse -and $null -ne $midIncResponse) 'middle insertion produced documentSymbol responses'
        if ($midFullResponse -and $midIncResponse) {
            Check ($midFullResponse -eq $midIncResponse) 'empty-range middle insertion matches full text'
        }

        $badUri = 'file:///c%3A/scripts/bad-range.cea'
        $badOpen = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didOpen'; params = @{ textDocument = @{ uri = $badUri; languageId = 'ce'; version = 1; text = "abc`n" } } }
        $badChange = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $badUri; version = 2 }; contentChanges = @(@{ range = @{ start = @{ line = 999; character = 0 }; end = @{ line = 999; character = 1 } }; text = 'X' }) } }
        $badFrames = Invoke-Server $serverExe @($init, $initialized, $badOpen, $badChange)
        Check (Find-Method $badFrames 'np3/lspResync') 'invalid incremental range requests client resync'

        $semOne = New-Frame @{ jsonrpc = '2.0'; id = 10; method = 'textDocument/semanticTokens/full'; params = @{ textDocument = @{ uri = $uri } } }
        $semTwo = New-Frame @{ jsonrpc = '2.0'; id = 11; method = 'textDocument/semanticTokens/full'; params = @{ textDocument = @{ uri = $uri } } }
        $semFrames = Invoke-Server $serverExe @($init, $initialized, $didOpen, $semOne, $semTwo)
        $semOneResponse = Find-Response $semFrames 10
        $semTwoResponse = Find-Response $semFrames 11
        Check ($null -ne $semOneResponse) 'first semanticTokens request returned a result'
        Check ($null -ne $semTwoResponse) 'second same-version semanticTokens request returned a result'
        if ($semOneResponse -and $semTwoResponse) {
            $semOneJson = $semOneResponse | ConvertFrom-Json
            $semTwoJson = $semTwoResponse | ConvertFrom-Json
            Check (($semOneJson.result.data -join ',') -eq ($semTwoJson.result.data -join ',')) 'same-version semanticTokens results are identical'
        }

        $diagText = "[ENABLE]`nnop`nbogusOpcode`nnop`nnop`nnop`nnop`nnop`nnop`n"
        $diagUri = 'file:///c%3A/scripts/diag-range.cea'
        $diagOpen = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didOpen'; params = @{ textDocument = @{ uri = $diagUri; languageId = 'ce'; version = 1; text = $diagText } } }
        $diagChange = New-Frame @{ jsonrpc = '2.0'; method = 'textDocument/didChange'; params = @{ textDocument = @{ uri = $diagUri; version = 2 }; contentChanges = @(@{ range = @{ start = @{ line = 9; character = 0 }; end = @{ line = 9; character = 0 } }; text = "nop`n" }) } }
        $diagFrames = Invoke-Server $serverExe @($init, $initialized, $diagOpen, $diagChange)
        Check (Find-DiagnosticLine $diagFrames 2) 'unchanged line diagnostic survives incremental refresh on another line'
    }
}
finally {
    Get-ChildItem $temp -File | Remove-Item -Force
    Remove-Item $temp -Force
}

if ($failures -ne 0) {
    Write-Host "TestCeLspEndToEnd: $failures failure(s)"
    exit 1
}
Write-Host 'TestCeLspEndToEnd: all checks passed'
