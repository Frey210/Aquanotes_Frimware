param(
    [string]$CredentialsPath = (Join-Path $PSScriptRoot '..\..\Aquanotes_FastAPI\credentials.md')
)

$text = Get-Content -LiteralPath $CredentialsPath -Raw
$uid = [regex]::Match($text, '(?m)^- Canary username/UID:\s*(\S+)\s*$').Groups[1].Value
$password = [regex]::Match($text, '(?m)^- Canary password:\s*(\S+)\s*$').Groups[1].Value
$uid = $uid.Trim('`')
$password = $password.Trim('`')
if (-not $uid -or -not $password) {
    throw 'Credential MQTT canary tidak ditemukan.'
}

$target = Join-Path $PSScriptRoot '..\aquanotes-triangle\include\secrets.h'
$content = "#pragma once`n`n#define DEVICE_UID `"$uid`"`n#define MQTT_PASSWORD `"$password`"`n"
[IO.File]::WriteAllText($target, $content, [Text.UTF8Encoding]::new($false))
Write-Output "Local MQTT secrets written for $uid"
