param([string]$OutputDirectory)
# Windows PowerShell 2 compatible. Runs fixtures/injected devices only.
$ErrorActionPreference='Stop'
$here=Split-Path -Parent $MyInvocation.MyCommand.Path
if(!$OutputDirectory){$OutputDirectory=Join-Path $here ('results-'+[Guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $OutputDirectory -ErrorAction Stop | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
function Run-Hidden([string]$Executable,[string[]]$Arguments){
 $quoted=@();foreach($argument in $Arguments){$quoted+='"'+$argument+'"'}
 $process=Start-Process -FilePath $Executable -ArgumentList $quoted -WindowStyle Hidden -PassThru
 if(!$process.WaitForExit(60000)){$process.Kill();throw "Timed out: $Executable"}
 if($process.ExitCode -ne 0){throw "Exit $($process.ExitCode): $Executable $Arguments"}
}
$probe=Join-Path $here 'compatibility-probe.exe'
Run-Hidden $probe @((Join-Path $OutputDirectory 'prerequisites.txt'))
$release=Join-Path (Split-Path -Parent $here) 'HFVExplorer.exe'
Run-Hidden $release @('--compatibility-report',(Join-Path $OutputDirectory 'release-startup.txt'))
$app=Join-Path $here 'HFVExplorer-tests.exe'
$previousWorker=$env:HFV_COPY_WORKER_TEST_EXECUTABLE
$env:HFV_COPY_WORKER_TEST_EXECUTABLE=$release
try{Run-Hidden $app @('--verify-copy-release',(Join-Path $OutputDirectory 'copy-release'))}finally{$env:HFV_COPY_WORKER_TEST_EXECUTABLE=$previousWorker}
Run-Hidden $app @('--verify-device-wait',(Join-Path $OutputDirectory 'device-wait.txt'))
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--feature-fixture',(Join-Path $OutputDirectory 'feature-links.dsk'))
foreach($test in @('copy-process','physical-removable','hard-links','folder-metadata','other-hfsplus','compression-write','optical-data-tracks','mfs-write-browser','extended-attributes','windows-path-coordination','per-volume-commands','background-metadata','background-mutations','background-window-close','independent-storage','floppy-copy-regressions','background-copies','browser-transfers','browser-windows','volume-coordination','file-operations','browser-sessions','shared-browser','storage-exclusive','main-mac-browser','file-mutations','copy-progress','cd-image','cd-burn','find-files','floppy-menu','media-control','hfsplus-ui','hfsplus-cd')){
 Run-Hidden $app @(('--verify-'+$test),(Join-Path $OutputDirectory $test))
}
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--self-test')
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--large-attributes')
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--other-features',(Join-Path $OutputDirectory 'other-hfsplus-features'))
Run-Hidden (Join-Path $here 'mfs-readonly.exe') @((Join-Path $OutputDirectory 'mfs-core'))
Run-Hidden (Join-Path $here 'image-roundtrip.exe') @((Join-Path $OutputDirectory 'classic-optical.dsk'),'--keep')
Run-Hidden (Join-Path $here 'cd-readonly.exe') @((Join-Path $OutputDirectory 'classic-optical.dsk'),(Join-Path $OutputDirectory 'classic-optical'))
$binhex=Join-Path $here 'binhex5-tests.exe'
if(Test-Path -LiteralPath $binhex){Run-Hidden $binhex @((Join-Path $OutputDirectory 'binhex5'))}
$compression=Join-Path $here 'test-hfsplus-compression.ps1'
if(Test-Path -LiteralPath $compression){& $compression -Directory (Join-Path $OutputDirectory 'native-compression')}
'PASS: prerequisite policy, shipping copy worker, process isolation/cancellation/cleanup, hidden frame startup, browser sessions and simulated storage/copy/media/UI regressions.' | Set-Content (Join-Path $OutputDirectory 'summary.txt')
Write-Output "Reports: $OutputDirectory"
