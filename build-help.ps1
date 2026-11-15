param([string]$Compiler = "${env:ProgramFiles(x86)}\HTML Help Workshop\hhc.exe")
$ErrorActionPreference='Stop'
$project=Join-Path $PSScriptRoot 'help\HFVExplorer.hhp'
$compiled=Join-Path $PSScriptRoot 'help\HFVExplorer.chm'
if(!(Test-Path -LiteralPath $Compiler)) {throw 'Install Microsoft HTML Help Workshop or pass -Compiler with the path to hhc.exe.'}
$started=[DateTime]::UtcNow.AddSeconds(-2)
& $Compiler $project
# hhc.exe commonly returns 1 on success. Validate the actual artifact instead.
if(!(Test-Path -LiteralPath $compiled)){throw 'Help compiler did not produce HFVExplorer.chm.'}
if((Get-Item -LiteralPath $compiled).LastWriteTimeUtc -lt $started){throw 'Help compiler did not update HFVExplorer.chm.'}
$bytes=[IO.File]::ReadAllBytes($compiled)
if($bytes.Length -lt 4096 -or [Text.Encoding]::ASCII.GetString($bytes,0,4) -ne 'ITSF'){throw 'Invalid compiled help output.'}
$destination=Join-Path $PSScriptRoot 'bin\x64'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $compiled -Destination $destination
Write-Host "Built and copied HFVExplorer.chm to $destination"
