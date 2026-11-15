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
Run-Hidden $app @('--verify-device-wait',(Join-Path $OutputDirectory 'device-wait.txt'))
foreach($test in @('copy-progress','cd-image','cd-burn','find-files','floppy-menu','media-control','hfsplus-ui')){
 Run-Hidden $app @(('--verify-'+$test),(Join-Path $OutputDirectory $test))
}
Run-Hidden (Join-Path $here 'hfsplus-tests.exe') @('--self-test')
'PASS: prerequisite policy, real release startup, simulated storage/copy/media/UI regressions.' | Set-Content (Join-Path $OutputDirectory 'summary.txt')
Write-Output "Reports: $OutputDirectory"
