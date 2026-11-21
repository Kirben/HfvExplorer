# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$OutputImage)

$ErrorActionPreference = 'Stop'
$image = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputImage)
$name = 'Compressed sample.bin'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('hfv-zisofs-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $source = Join-Path $scratch $name
    $payload = New-Object byte[] 100003
    for ($i=0; $i -lt $payload.Length; $i++) {
        if ($i -lt 32768 -or $i -ge 65536) { $payload[$i] = 65 + $i % 3 }
    }
    [IO.File]::WriteAllBytes($source, $payload)
    tar.exe -cf $image --format iso9660 --options 'iso9660:zisofs=direct,iso9660:!joliet' -C $scratch $name
    if ($LASTEXITCODE -ne 0) { throw 'Libarchive could not master the zisofs image.' }
} finally {
    Remove-Item -LiteralPath (Join-Path $scratch $name) -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $scratch
}
