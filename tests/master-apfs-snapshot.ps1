# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$OutputImage,
    [string]$ApfsTool = 'apfs.exe'
)

$ErrorActionPreference = 'Stop'
$image = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputImage)
if (Test-Path -LiteralPath $image) { throw 'Choose a new fixture path.' }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('hfv-apfs-snapshot-' + [Guid]::NewGuid().ToString('N'))
$folder = Join-Path $scratch 'Folder'
$file = Join-Path $folder 'sample.txt'
New-Item -ItemType Directory -Path $folder | Out-Null
try {
    [IO.File]::WriteAllBytes($file, [Text.Encoding]::ASCII.GetBytes("Independently written APFS data`n"))
    & $ApfsTool pack $scratch $image --fs apfs --compression zlib --volname 'Independent APFS' --snapshot 'Independent backup' --source-date-epoch 1700000000 --strict
    if ($LASTEXITCODE -ne 0) { throw 'The independent APFS writer could not build the fixture.' }
    & $ApfsTool snapshot verify $image
    if ($LASTEXITCODE -ne 0) { throw 'The independent APFS writer could not verify its snapshot.' }
} finally {
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $folder
    Remove-Item -LiteralPath $scratch
}
