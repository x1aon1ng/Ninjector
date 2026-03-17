param(
    [string]$MainFile = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\main.cpp",
    [string]$NcoreFile = "E:\Learn\my_program\all_my_hook\kanxue\Ninjector\jni\ncore\ncore.cpp"
)

$main = Get-Content -Path $MainFile -Raw
$ncore = Get-Content -Path $NcoreFile -Raw

if ($main -notmatch 'clear_spawn_in_zygote\(') {
    Write-Error "main.cpp does not call clear_spawn_in_zygote()"
    exit 1
}

if ($ncore -notmatch 'extern "C" void aclear\(') {
    Write-Error "ncore.cpp does not export aclear()"
    exit 1
}

Write-Output "spawn one-shot clear hooks are wired."
