param(
    [Parameter(Mandatory = $true)]
    [string]$ServerExecutable,
    [string]$ServerArguments = "",
    [string]$Label = "HTTP"
)

$ErrorActionPreference = "Stop"
$port = 18773
$requestCount = 20000
$expectedBody = "<h1>Hello, World!</h1>"
$serverProcess = $null
$client = $null
$stdoutPath = Join-Path $env:TEMP ("zeus-http-{0}-{1}.out" -f $Label, [Guid]::NewGuid().ToString("N"))
$stderrPath = Join-Path $env:TEMP ("zeus-http-{0}-{1}.err" -f $Label, [Guid]::NewGuid().ToString("N"))

try {
    $occupied = netstat -ano -p tcp | Where-Object {
        $_ -match ("^\s*TCP\s+\S+:{0}\s+\S+\s+LISTENING\s+\d+\s*$" -f $port)
    }
    if ($occupied) {
        throw "TCP port $port is already occupied."
    }

    $exe = (Resolve-Path -LiteralPath $ServerExecutable).Path
    $argumentLine = if ($ServerArguments) { '"' + $ServerArguments + '"' } else { "" }
    $serverProcess = Start-Process -FilePath $exe `
        -ArgumentList $argumentLine `
        -WorkingDirectory (Get-Location).Path `
        -PassThru `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath
    Write-Output ("{0} STARTED PID={1}" -f $Label, $serverProcess.Id)

    $readyDeadline = (Get-Date).AddSeconds(120)
    $listener = $null
    while ((Get-Date) -lt $readyDeadline) {
        $serverProcess.Refresh()
        if ($serverProcess.HasExited) {
            throw "HTTP server exited before opening port $port."
        }
        $listener = netstat -ano -p tcp | Where-Object {
            $_ -match ("^\s*TCP\s+\S+:{0}\s+\S+\s+LISTENING\s+{1}\s*$" -f $port, $serverProcess.Id)
        }
        if ($listener) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $listener) {
        throw "HTTP server did not open port $port within 120 seconds."
    }

    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.MaxConnectionsPerServer = 1
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(30)
    $client.DefaultRequestHeaders.ConnectionClose = $false

    $serverProcess.Refresh()
    $basePrivateMiB = [Math]::Round($serverProcess.PrivateMemorySize64 / 1MB, 2)
    $baseWorkingSetMiB = [Math]::Round($serverProcess.WorkingSet64 / 1MB, 2)
    Write-Output ("{0} READY PID={1} PRIVATE_MIB={2} WORKING_SET_MIB={3}" -f `
        $Label, $serverProcess.Id, $basePrivateMiB, $baseWorkingSetMiB)

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 1; $i -le $requestCount; $i++) {
        $response = $null
        try {
            $response = $client.GetAsync("http://127.0.0.1:$port/hello").GetAwaiter().GetResult()
            $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            if ([int]$response.StatusCode -ne 200 -or $body -cne $expectedBody) {
                throw ("Request {0} returned status {1} and body '{2}'." -f `
                    $i, [int]$response.StatusCode, $body)
            }
        }
        finally {
            if ($response) { $response.Dispose() }
        }

        if ($i -eq 1 -or $i -eq 10000 -or $i -eq $requestCount) {
            $serverProcess.Refresh()
            if ($serverProcess.HasExited) {
                throw "HTTP server exited after request $i."
            }
            $privateMiB = [Math]::Round($serverProcess.PrivateMemorySize64 / 1MB, 2)
            $workingSetMiB = [Math]::Round($serverProcess.WorkingSet64 / 1MB, 2)
            Write-Output ("{0} REQUESTS={1} PRIVATE_MIB={2} WORKING_SET_MIB={3}" -f `
                $Label, $i, $privateMiB, $workingSetMiB)
        }
    }
    $stopwatch.Stop()
    Write-Output ("{0} PASS REQUESTS={1} ELAPSED_SECONDS={2}" -f `
        $Label, $requestCount, [Math]::Round($stopwatch.Elapsed.TotalSeconds, 2))

    $client.Dispose()
    $client = $null
    if (-not $serverProcess.WaitForExit(30000)) {
        throw "HTTP server did not exit after the persistent client closed."
    }
    if ($serverProcess.ExitCode -ne 0) {
        throw "HTTP server exited with code $($serverProcess.ExitCode)."
    }
    if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath }
    if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath }
}
catch {
    if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath }
    if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath }
    throw
}
finally {
    if ($client) { $client.Dispose() }
    if ($serverProcess) {
        $serverProcess.Refresh()
        if (-not $serverProcess.HasExited) {
            $serverProcess.Kill()
            $serverProcess.WaitForExit()
        }
    }
}
