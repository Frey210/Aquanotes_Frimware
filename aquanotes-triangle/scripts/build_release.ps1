param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$')]
    [string]$Version,
    [string]$SigningKey
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$configuration = Get-Content -Raw (Join-Path $projectRoot 'platformio.ini')
$escapedVersion = [regex]::Escape($Version)
if ($configuration -notmatch "AQUANOTES_FW_VERSION=\\`"$escapedVersion\\`"") {
    throw "Version platformio.ini tidak cocok. Set AQUANOTES_FW_VERSION ke $Version sebelum release build."
}

Push-Location $projectRoot
try {
    $env:PYTHONIOENCODING = 'utf-8'
    & pio run
    if ($LASTEXITCODE -ne 0) { throw 'PlatformIO build gagal.' }

    $buildDir = Join-Path $projectRoot '.pio\build\esp32-s3-devkitc-1'
    $firmware = Join-Path $buildDir 'firmware.bin'
    $factory = Join-Path $buildDir 'firmware.factory.bin'
    $partition = Join-Path $projectRoot 'partitions.csv'
    foreach ($required in @($firmware, $factory, $partition)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "Artifact tidak ditemukan: $required" }
    }

    $slotSize = 0x330000
    $firmwareSize = (Get-Item -LiteralPath $firmware).Length
    if ($firmwareSize -ge $slotSize) { throw "Firmware $firmwareSize byte tidak muat di slot $slotSize byte." }
    if ($firmwareSize -gt [math]::Floor($slotSize * 0.8)) { throw 'Margin slot OTA kurang dari 20%.' }

    $releaseDir = Join-Path $projectRoot "release\$Version"
    New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
    $appOutput = Join-Path $releaseDir "aquanotes-$Version.bin"
    $factoryOutput = Join-Path $releaseDir "aquanotes-$Version-factory.bin"
    Copy-Item -LiteralPath $firmware -Destination $appOutput -Force
    Copy-Item -LiteralPath $factory -Destination $factoryOutput -Force
    Copy-Item -LiteralPath $partition -Destination (Join-Path $releaseDir 'partitions.csv') -Force

    $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $appOutput).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        schema = 1
        version = $Version
        artifact = [IO.Path]::GetFileName($appOutput)
        sha256 = $sha256
        size = $firmwareSize
        slot_size = $slotSize
        build_utc = [DateTime]::UtcNow.ToString('o')
        git_commit = (& git rev-parse HEAD).Trim()
        signed = $false
    }

    if ($SigningKey) {
        $resolvedKey = (Resolve-Path -LiteralPath $SigningKey).Path
        $signature = "$appOutput.sig"
        $signatureMetadata = Join-Path $releaseDir 'signature.json'
        & python (Join-Path $PSScriptRoot 'sign_release.py') --key $resolvedKey --artifact $appOutput --signature $signature --metadata $signatureMetadata
        if ($LASTEXITCODE -ne 0) { throw 'Penandatanganan firmware gagal.' }
        $signatureInfo = Get-Content -Raw $signatureMetadata | ConvertFrom-Json
        $manifest.signed = $true
        $manifest.signature = [IO.Path]::GetFileName($signature)
        $manifest.signing_key_id = $signatureInfo.signing_key_id
        $manifest.signature_base64 = $signatureInfo.signature_base64
    }

    $manifest | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $releaseDir 'manifest.json')
    Write-Output "Release siap: $releaseDir"
    Write-Output "SHA-256: $sha256"
    Write-Output "Ukuran: $firmwareSize / $slotSize byte"
}
finally {
    Pop-Location
}
