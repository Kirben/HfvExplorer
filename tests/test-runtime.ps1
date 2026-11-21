param(
 [string]$OutputDirectory,
 [string]$TestExecutable,
 [string]$FixtureSetup='Z:\Mac Disk Images\Use-TestImages.ps1',
 [switch]$FormatsOnly
)
# Windows PowerShell 2 compatible. Runs fixtures/injected devices only.
# From a checkout, use -FormatsOnly -TestExecutable .\bin\tests\HFVExplorer.exe.
# For interoperability in a VM, pass -FixtureSetup with that guest's fixture path.
$ErrorActionPreference='Stop'
$here=Split-Path -Parent $MyInvocation.MyCommand.Path
if(!$OutputDirectory){$OutputDirectory=Join-Path $here ('results-'+[Guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $OutputDirectory -ErrorAction Stop | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
function Run-Hidden([string]$Executable,[string[]]$Arguments,[int]$TimeoutSeconds=60){
 $quoted=@();foreach($argument in $Arguments){$quoted+='"'+$argument+'"'}
 $process=Start-Process -FilePath $Executable -ArgumentList $quoted -WindowStyle Hidden -PassThru
 if(!$process.WaitForExit(1000*$TimeoutSeconds)){$process.Kill();throw "Timed out: $Executable $Arguments"}
 if($process.ExitCode -ne 0){throw "Exit $($process.ExitCode): $Executable $Arguments"}
}
if($TestExecutable){$app=(Resolve-Path -LiteralPath $TestExecutable).ProviderPath}
else{$app=Join-Path $here 'HFVExplorer-tests.exe'}

if(!$FormatsOnly){
 $probe=Join-Path $here 'compatibility-probe.exe'
 Run-Hidden $probe @((Join-Path $OutputDirectory 'prerequisites.txt'))
 $release=Join-Path (Split-Path -Parent $here) 'HFVExplorer.exe'
 Run-Hidden $release @('--compatibility-report',(Join-Path $OutputDirectory 'release-startup.txt'))
}

$fixtureReport=Join-Path $OutputDirectory 'format-coverage.txt'
$previousFileVault=$env:HFV_APFS_NATIVE_FILEVAULT
try {
 if($FixtureSetup -and (Test-Path -LiteralPath $FixtureSetup -PathType Leaf)){
  $FixtureSetup=(Resolve-Path -LiteralPath $FixtureSetup).ProviderPath
  & $FixtureSetup
  "Shared fixture setup: $FixtureSetup" | Set-Content $fixtureReport
  $fileVault=Join-Path (Split-Path -Parent $FixtureSetup) 'APFS\Software Encryption\apfs-native-filevault.raw.gz'
  if(!$previousFileVault -and (Test-Path -LiteralPath $fileVault -PathType Leaf)){
   $rawPath=Join-Path $OutputDirectory 'native-filevault.raw'
   $inputStream=[IO.File]::OpenRead($fileVault)
   try {
    $gzip=New-Object IO.Compression.GZipStream($inputStream,[IO.Compression.CompressionMode]::Decompress)
    try {
     $raw=[IO.File]::Open($rawPath,[IO.FileMode]::CreateNew)
     try {
      $buffer=New-Object byte[] 65536
      while(($count=$gzip.Read($buffer,0,$buffer.Length)) -gt 0){$raw.Write($buffer,0,$count)}
     } finally {$raw.Close()}
    } finally {$gzip.Close()}
   } finally {$inputStream.Close()}
   $env:HFV_APFS_NATIVE_FILEVAULT=$rawPath
  }
 }else{
  'Shared fixture setup unavailable. Optional interoperability cases require fixture paths.' | Set-Content $fixtureReport
 }
 foreach($key in @('HFV_APFS_NATIVE_FILEVAULT','HFV_CERT_DMG_FIXTURES')){
  $value=[Environment]::GetEnvironmentVariable($key,'Process')
  if($value){"Enabled: $key" | Add-Content $fixtureReport}
  else{"Optional fixture unavailable: $key" | Add-Content $fixtureReport}
 }
 foreach($test in @('apfs-container','dmg-image','encrypted-dmg','ndif-image','sparse-image','sparse-bundle',
                    'optical-browser','optical-data-tracks','raw-optical','mfs-write-browser',
                    'shared-browser','copy-process')){
  Run-Hidden $app @(('--verify-'+$test),(Join-Path $OutputDirectory $test)) 600
  "PASS: $test" | Add-Content $fixtureReport
 }
}finally{$env:HFV_APFS_NATIVE_FILEVAULT=$previousFileVault}

$binhex=Join-Path $here 'binhex5-tests.exe'
if(!(Test-Path -LiteralPath $binhex)){
 $binhex=Join-Path (Split-Path -Parent $here) 'bin\x64\binhex5-tests.exe'
}
if(Test-Path -LiteralPath $binhex){
 Run-Hidden $binhex @((Join-Path $OutputDirectory 'binhex5'))
 'PASS: BinHex 5 codec' | Add-Content $fixtureReport
}else{
 'Optional executable unavailable: binhex5-tests.exe' | Add-Content $fixtureReport
}

if($FormatsOnly){
 Write-Output "Reports: $OutputDirectory"
 return
}

$previousWorker=$env:HFV_COPY_WORKER_TEST_EXECUTABLE
$env:HFV_COPY_WORKER_TEST_EXECUTABLE=$release
try{Run-Hidden $app @('--verify-copy-release',(Join-Path $OutputDirectory 'copy-release'))}finally{$env:HFV_COPY_WORKER_TEST_EXECUTABLE=$previousWorker}
Run-Hidden $app @('--verify-device-wait',(Join-Path $OutputDirectory 'device-wait.txt'))
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--feature-fixture',(Join-Path $OutputDirectory 'feature-links.dsk'))
foreach($test in @('typed-locations','physical-removable','hard-links','folder-metadata','other-hfsplus','compression-write','extended-attributes','windows-path-coordination','per-volume-commands','background-metadata','background-mutations','background-window-close','independent-storage','floppy-copy-regressions','background-copies','browser-transfers','browser-windows','volume-coordination','file-operations','browser-sessions','storage-exclusive','main-mac-browser','file-mutations','copy-progress','cd-image','cd-burn','find-files','floppy-menu','media-control','hfsplus-ui','hfsplus-cd')){
 Run-Hidden $app @(('--verify-'+$test),(Join-Path $OutputDirectory $test))
}
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--self-test')
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--large-attributes')
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--other-features',(Join-Path $OutputDirectory 'other-hfsplus-features'))
Run-Hidden (Join-Path $here 'mfs-readonly.exe') @((Join-Path $OutputDirectory 'mfs-core'))
Run-Hidden (Join-Path $here 'image-roundtrip.exe') @((Join-Path $OutputDirectory 'classic-optical.dsk'),'--keep')
Run-Hidden (Join-Path $here 'cd-readonly.exe') @((Join-Path $OutputDirectory 'classic-optical.dsk'),(Join-Path $OutputDirectory 'classic-optical'))

$compression=Join-Path $here 'test-hfsplus-compression.ps1'
if(Test-Path -LiteralPath $compression){& $compression -Directory (Join-Path $OutputDirectory 'native-compression')}
'PASS: prerequisite policy, shipping copy worker, process isolation/cancellation/cleanup, hidden frame startup, browser sessions and simulated storage/copy/media/UI regressions.' | Set-Content (Join-Path $OutputDirectory 'summary.txt')
Write-Output "Reports: $OutputDirectory"
