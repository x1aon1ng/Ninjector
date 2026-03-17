param(
    [string]$SourceFile = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\symbi\symbi_injector.cpp"
)

$content = Get-Content -Path $SourceFile -Raw

if ($content -match 'PTRACE_ATTACH|PTRACE_DETACH') {
    Write-Error "Found ptrace-based stop/resume logic in $SourceFile"
    exit 1
}

if ($content -notmatch 'SIGSTOP' -or $content -notmatch 'SIGCONT') {
    Write-Error "Did not find expected SIGSTOP/SIGCONT usage in $SourceFile"
    exit 1
}

Write-Output "symbi stop/resume uses SIGSTOP/SIGCONT."
