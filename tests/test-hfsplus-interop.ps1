# SPDX-License-Identifier: GPL-2.0-or-later
# Optional independent interoperability checks. Requires WSL and hfsprogs.
# Uses only newly created image files. No mounts or physical devices.
param([string]$Directory,[switch]$IncrementalOnly)
$ErrorActionPreference='Stop'
if(!$Directory){$Directory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-HFSPlus-interop-'+[Guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $Directory -ErrorAction Stop | Out-Null
$Directory=(Resolve-Path -LiteralPath $Directory).Path
$exe=Join-Path $PSScriptRoot '..\bin\x64\hfsplus-tests.exe'
function LinuxPath([string]$path){$value=& wsl -e wslpath -a $path;if($LASTEXITCODE){throw 'WSL path conversion failed'};return $value.Trim()}
function NewReference([string]$path,[bool]$HfsX=$false){$file=[IO.File]::Open($path,[IO.FileMode]::CreateNew);try{$file.SetLength(134217728)}finally{$file.Dispose()}; if($HfsX){& wsl -e /usr/sbin/mkfs.hfsplus -s -J -v Reference (LinuxPath $path)}else{& wsl -e /usr/sbin/mkfs.hfsplus -J -v Reference (LinuxPath $path)};if($LASTEXITCODE){throw 'Reference formatter failed'}}
function Check([string]$path){
 # hfsprogs 540.1's -n mode also incorrectly flags its own pristine output.
 # Use disposable writable fixtures, and reject any reported structural repair.
 $output=& wsl -e /usr/sbin/fsck.hfsplus -fy (LinuxPath $path) 2>&1
 $output | Tee-Object -FilePath ($path+'.fsck.log') | Write-Host
 if($LASTEXITCODE -ne 0 -or ($output -match 'Invalid |incorrect|needs.*repair|Repairing volume|was repaired|corrupt').Count){throw "Independent checker rejected $path"}
}
if(!$IncrementalOnly){
$own=Join-Path $Directory 'own.dsk'; & $exe $own;if($LASTEXITCODE){throw 'Own formatter/core tests failed'};Check $own
$reference=Join-Path $Directory 'reference.dsk';NewReference $reference;& $exe --modify $reference;if($LASTEXITCODE){throw 'Reference-image write failed'};Check $reference
$fragmented=Join-Path $Directory 'fragmented.dsk';& $exe --fragmented $fragmented;if($LASTEXITCODE){throw 'Fragmented fixture failed'};Check $fragmented
$crash=Join-Path $Directory 'crash.dsk';& $exe --crash $crash;if($LASTEXITCODE){throw 'Crash fixture failed'};Check $crash;& $exe --check-recovered $crash;if($LASTEXITCODE){throw 'Own journal was not replayed'}
$referenceCrash=Join-Path $Directory 'reference-crash.dsk';NewReference $referenceCrash;& $exe --crash-existing $referenceCrash;if($LASTEXITCODE){throw 'Reference journal crash fixture failed'};Check $referenceCrash;& $exe --check-recovered $referenceCrash;if($LASTEXITCODE){throw 'Reference-volume journal was not replayed'}
Write-Host "PASS: independent formatting, HFS+ writes, fragmented catalog and committed-journal replay in both byte orders. Fixtures retained at $Directory."

foreach($mode in @('hfsx','hfsx-ci')){
 $path=Join-Path $Directory ($mode+'.dsk'); & $exe ('--'+$mode) $path;if($LASTEXITCODE){throw 'Own HFSX format/write failed'};Check $path
 $path=Join-Path $Directory ($mode+'-crash.dsk'); & $exe ('--crash-'+$mode) $path;if($LASTEXITCODE){throw 'Own HFSX journal fixture failed'};Check $path;& $exe --check-recovered $path;if($LASTEXITCODE){throw 'HFSX recovery failed'}
}
$referenceX=Join-Path $Directory 'reference-hfsx.dsk';NewReference $referenceX $true;& $exe --modify $referenceX;if($LASTEXITCODE){throw 'Reference HFSX write failed'};Check $referenceX
$referenceXCrash=Join-Path $Directory 'reference-hfsx-crash.dsk';NewReference $referenceXCrash $true;& $exe --crash-existing $referenceXCrash;if($LASTEXITCODE){throw 'Reference HFSX crash fixture failed'};Check $referenceXCrash;& $exe --check-recovered $referenceXCrash;if($LASTEXITCODE){throw 'Reference HFSX recovery failed'}
Write-Host 'PASS: HFSX case-sensitive/case-insensitive formatting and writes, reference HFSX mutation, and independent journal replay.'
}
$largeCatalog=Join-Path $Directory 'large-catalog.dsk';& $exe --incremental-large-catalog $largeCatalog;if($LASTEXITCODE){throw 'Large map-chain catalog failed'};Check $largeCatalog
$forks=Join-Path $Directory 'fragmented-forks.dsk';& $exe --fragmented-forks $forks;if($LASTEXITCODE){throw 'Fragmented forks failed'};Check $forks
$resized=Join-Path $Directory 'resized.dsk';& $exe --incremental-resize $resized;if($LASTEXITCODE){throw 'Journal enlargement failed'};Check $resized
foreach($phase in @('journal-resize-data-flushed','journal-resize-new-log-committed','journal-resize-jib-written','journal-resize-jib-committed','journal-resize-home-flushed','journal-resize-cleared')){
 $path=Join-Path $Directory ($phase+'.dsk');& $exe --incremental-resize-crash $phase $path;if($LASTEXITCODE){throw "Resize crash fixture failed: $phase"};Check $path
}
foreach($x in @($false,$true)){
 $path=Join-Path $Directory ('resize-reference-'+$x+'.dsk');NewReference $path $x;& $exe --incremental-reference $path;if($LASTEXITCODE){throw 'Reference journal enlargement failed'};Check $path
}
Write-Host 'PASS: independent incremental catalog/map-chain updates, fragmented data/resource forks, journal enlargement and every relocation interruption boundary.'