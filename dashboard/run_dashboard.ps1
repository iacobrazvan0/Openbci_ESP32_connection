param(
    [string]$Port = "COM3",
    [switch]$Demo,
    [switch]$SmokeTest
)

$dashboardRoot = $PSScriptRoot
$pythonPath = Join-Path $dashboardRoot ".venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pythonPath)) {
    Write-Error "Mediul Python lipseste. Ruleaza: py -3.11 -m venv .venv; .\.venv\Scripts\python.exe -m pip install -r requirements.txt"
    exit 1
}

$env:PYTHONPATH = $dashboardRoot
$arguments = @("-m", "eeg_dashboard.app")
if ($Demo) {
    $arguments += "--demo"
} else {
    $arguments += @("--port", $Port)
}
if ($SmokeTest) { $arguments += "--smoke-test" }

& $pythonPath @arguments
exit $LASTEXITCODE
