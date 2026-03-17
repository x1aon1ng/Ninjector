param(
    [string]$MainFile = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\main.cpp",
    [string]$StubSource = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\symbi\stub_src\stub.c",
    [string]$GeneratedStub = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\symbi\stub_src\generated_stub.h"
)

$main = Get-Content -Path $MainFile -Raw
$stubSource = Get-Content -Path $StubSource -Raw
$generated = Get-Content -Path $GeneratedStub -Raw

if ($main -notmatch 'printf\("Usage:') {
    Write-Error "show_help() does not print to stdout"
    exit 1
}

$markerMatch = [regex]::Match($stubSource, 'mark\s*=\s*"([^"]+)"')
if (-not $markerMatch.Success) {
    Write-Error "Could not extract marker from stub.c"
    exit 1
}
$marker = $markerMatch.Groups[1].Value
$bytes = [System.Text.Encoding]::ASCII.GetBytes($marker)
$hexParts = $bytes | ForEach-Object { ('0x{0:x2}' -f $_) }
$missing = $hexParts | Where-Object { $generated -notmatch [regex]::Escape($_) }
if ($missing.Count -gt 0) {
    Write-Error "generated_stub.h does not contain marker bytes for $marker"
    exit 1
}

Write-Output "help output and symbi marker wiring look consistent."
