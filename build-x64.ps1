param([switch]$Test,[switch]$TestFloppy,[switch]$TestCd,[switch]$TestHfsPlus)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$instances=(& $vswhere -all -products '*' -format json | ConvertFrom-Json)
$vs=$instances | Where-Object {
 (Test-Path -LiteralPath (Join-Path $_.installationPath 'VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe')) -and
 (Test-Path -LiteralPath (Join-Path $_.installationPath 'VC\Tools\MSVC\14.44.35207\atlmfc\lib\x64\nafxcw.lib'))
} | Select-Object -First 1 -ExpandProperty installationPath
if(!$vs){throw 'Install v143 14.44 (17.14) x86/x64 tools and matching MFC in one Visual Studio instance. v145 cannot target Windows 7/8.'}
$msbuild=Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'Application build failed.' }
$compiledHelp=Join-Path $PSScriptRoot 'help\HFVExplorer.chm'
if(Test-Path -LiteralPath $compiledHelp) {Copy-Item -LiteralPath $compiledHelp -Destination "$PSScriptRoot\bin\x64\HFVExplorer.chm"}
& $msbuild "$PSScriptRoot\tests\cd-probe.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'CD diagnostic build failed.' }
& $msbuild "$PSScriptRoot\tests\floppy-probe.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'Floppy diagnostic build failed.' }
& $msbuild "$PSScriptRoot\tests\compatibility-probe.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE){throw 'Compatibility probe build failed.'}
& "$PSScriptRoot\bin\x64\compatibility-probe.exe"
if($LASTEXITCODE){throw 'Host prerequisite or compatibility policy test failed.'}
$startupReport=Join-Path $PSScriptRoot 'bin\x64\compatibility-startup.txt'
$startup=Start-Process -FilePath "$PSScriptRoot\bin\x64\HFVExplorer.exe" -ArgumentList @('--compatibility-report',('"'+$startupReport+'"')) -WindowStyle Hidden -PassThru
if(!$startup.WaitForExit(30000)){$startup.Kill();throw 'Release compatibility startup timed out.'}
if($startup.ExitCode -ne 0){throw 'Release compatibility startup failed.'}

if($Test -or $TestHfsPlus) {
 & $msbuild "$PSScriptRoot\tests\hfsplus-tests.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'HFS+ core test build failed.'}
 $hfsPlusDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-HFSPlus-'+[Guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Path $hfsPlusDirectory | Out-Null
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" (Join-Path $hfsPlusDirectory 'formatted.dsk')
 if($LASTEXITCODE){throw "HFS+ core tests failed; retained $hfsPlusDirectory"}
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" --large (Join-Path $hfsPlusDirectory 'large.dsk')
 if($LASTEXITCODE){throw "HFS+ large-fork tests failed; retained $hfsPlusDirectory"}
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" --large-hfsx (Join-Path $hfsPlusDirectory 'large-hfsx.dsk')
 if($LASTEXITCODE){throw "HFSX large-fork tests failed; retained $hfsPlusDirectory"}
 & $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Test /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'HFS+ hidden UI test build failed.'}
 $hfsPlusUi=Join-Path $hfsPlusDirectory 'ui'
 $hfsPlusTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-hfsplus-ui',('"'+$hfsPlusUi+'"')) -WindowStyle Hidden -PassThru
 if(!$hfsPlusTest.WaitForExit(30000)){$hfsPlusTest.Kill();throw 'HFS+ hidden UI test timed out.'}
 if($hfsPlusTest.ExitCode -ne 0){throw "HFS+ hidden UI test failed; retained $hfsPlusUi"}
 Get-Content (Join-Path $hfsPlusUi 'hfsplus-ui-report.txt')
 Write-Host "HFS+ test images retained at $hfsPlusDirectory; no physical devices accessed."
}
if($TestCd) {
 & $msbuild "$PSScriptRoot\tests\image-roundtrip.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'CD fixture builder failed.'}
 & $msbuild "$PSScriptRoot\tests\cd-readonly.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'CD reader test build failed.'}
 $cdCacheImage=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-CD-cache-'+[Guid]::NewGuid().ToString('N')+'.dsk')
 & "$PSScriptRoot\bin\x64\image-roundtrip.exe" $cdCacheImage --keep
 if($LASTEXITCODE){throw 'CD cache fixture creation failed.'}
 $cdCacheDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-CD-cache-tests-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\cd-readonly.exe" $cdCacheImage $cdCacheDirectory
 if($LASTEXITCODE){throw "CD cache tests failed; retained $cdCacheDirectory"}
 Write-Host "CD cache fixtures retained at $cdCacheDirectory; source image $cdCacheImage. No physical drives opened."
}
if($Test) {
 & $msbuild "$PSScriptRoot\tests\fork-limits.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Fork-limit test build failed.' }
 $forkLimits=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-fork-limits-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\fork-limits.exe" $forkLimits
 if($LASTEXITCODE) { throw "Fork-limit tests failed; retained $forkLimits" }
 & $msbuild "$PSScriptRoot\tests\image-roundtrip.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Test build failed.' }
 $testImage=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-'+[Guid]::NewGuid().ToString('N')+'.dsk')
 & "$PSScriptRoot\bin\x64\image-roundtrip.exe" $testImage
 if($LASTEXITCODE) { throw "Image test failed; retained $testImage" }
 Write-Host "Test image retained at $testImage"
 & $msbuild "$PSScriptRoot\tests\cd-readonly.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Read-only CD test build failed.' }
 $populatedImage=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-CD-'+[Guid]::NewGuid().ToString('N')+'.dsk')
 & "$PSScriptRoot\bin\x64\image-roundtrip.exe" $populatedImage --keep
 if($LASTEXITCODE) { throw 'Populated CD fixture creation failed.' }
 $fixtureDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-CD-tests-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\cd-readonly.exe" $populatedImage $fixtureDirectory
 if($LASTEXITCODE) { throw "Read-only CD tests failed; retained $fixtureDirectory" }
 & $msbuild "$PSScriptRoot\tests\large-image-core.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Large-image core test build failed.' }
 $largeDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-large-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\large-image-core.exe" $largeDirectory
 if($LASTEXITCODE) { throw "Large-image core tests failed; retained $largeDirectory" }
 & $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Test /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Headless GUI-storage test build failed.' }
 $manifest=Join-Path $largeDirectory 'manifest.bin'
 $report=Join-Path $largeDirectory 'gui-report.txt'
 # The Test configuration exits before preferences, drive discovery or window creation.
 $headless=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-large-image',('"'+$manifest+'"'),('"'+$report+'"')) -WindowStyle Hidden -PassThru
 if(!$headless.WaitForExit(30000)) { $headless.Kill(); throw 'Headless image test timed out.' }
 if(Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
 if($headless.ExitCode -ne 0) { throw "Headless image tests failed; retained $largeDirectory" }
 Write-Host "Large-image tests retained at $largeDirectory (includes approximately 10 GiB of large-fork fixtures)."
 & $msbuild "$PSScriptRoot\tests\mfs-readonly.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Read-only MFS test build failed.' }
 $mfsDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-MFS-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\mfs-readonly.exe" $mfsDirectory
 if($LASTEXITCODE) { throw "Read-only MFS tests failed; retained $mfsDirectory" }
 Write-Host "MFS fixtures and exports retained at $mfsDirectory"
}

if($Test -or $TestFloppy) {
 & $msbuild "$PSScriptRoot\tests\floppy-write.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Floppy writer test build failed.' }
 & "$PSScriptRoot\bin\x64\floppy-write.exe"
 if($LASTEXITCODE) { throw 'Simulated floppy writer tests failed.' }
 & $msbuild "$PSScriptRoot\tests\floppy-readonly.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Read-only floppy test build failed.' }
 $floppyDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-floppy-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\floppy-readonly.exe" $floppyDirectory
 if($LASTEXITCODE) { throw "Read-only floppy tests failed; retained $floppyDirectory" }
 & $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Test /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Headless floppy GUI-adapter test build failed.' }
 $floppyImage=Join-Path $floppyDirectory 'floppy-hfs.dsk'
 $floppyReport=Join-Path $floppyDirectory 'gui-floppy-report.txt'
 $floppyTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-floppy',('"'+$floppyImage+'"'),('"'+$floppyReport+'"')) -WindowStyle Hidden -PassThru
 if(!$floppyTest.WaitForExit(30000)) { $floppyTest.Kill(); throw 'Headless floppy test timed out.' }
 if(Test-Path -LiteralPath $floppyReport) { Get-Content -LiteralPath $floppyReport }
 if($floppyTest.ExitCode -ne 0) { throw "Headless floppy test failed; retained $floppyDirectory" }
 $waitReport=Join-Path $floppyDirectory 'device-wait-report.txt'
 $waitTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-device-wait',('"'+$waitReport+'"')) -WindowStyle Hidden -PassThru
 if(!$waitTest.WaitForExit(30000)) {throw 'Device wait test timed out.'}
 if(Test-Path -LiteralPath $waitReport) {Get-Content -LiteralPath $waitReport}
 if($waitTest.ExitCode -ne 0) {throw "Device wait tests failed; retained $floppyDirectory"}
 $copyDirectory=Join-Path $floppyDirectory 'copy-progress'
 $copyTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-copy-progress',('"'+$copyDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$copyTest.WaitForExit(30000)) {throw 'Copy progress test timed out.'}
 $copyReport=Join-Path $copyDirectory 'copy-progress-report.txt'
 if(Test-Path -LiteralPath $copyReport) {Get-Content -LiteralPath $copyReport}
 if($copyTest.ExitCode -ne 0) {throw "Copy progress tests failed; retained $copyDirectory"}
 $cdImageDirectory=Join-Path $floppyDirectory 'cd-image'
 $cdImageTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-cd-image',('"'+$cdImageDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$cdImageTest.WaitForExit(30000)) {throw 'CD image tests timed out.'}
 $cdImageReport=Join-Path $cdImageDirectory 'cd-image-report.txt'
 if(Test-Path -LiteralPath $cdImageReport) {Get-Content -LiteralPath $cdImageReport}
 if($cdImageTest.ExitCode -ne 0) {throw "CD image tests failed; retained $cdImageDirectory"}
 $cdBurnDirectory=Join-Path $floppyDirectory 'cd-burn'
 $cdBurnTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-cd-burn',('"'+$cdBurnDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$cdBurnTest.WaitForExit(30000)) {throw 'CD burn tests timed out.'}
 $cdBurnReport=Join-Path $cdBurnDirectory 'cd-burn-report.txt'
 if(Test-Path -LiteralPath $cdBurnReport) {Get-Content -LiteralPath $cdBurnReport}
 if($cdBurnTest.ExitCode -ne 0) {throw "CD burn tests failed; retained $cdBurnDirectory"}
 $findDirectory=Join-Path $floppyDirectory 'find-files'
 $findTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-find-files',('"'+$findDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$findTest.WaitForExit(30000)) {throw 'Find-files tests timed out.'}
 $findReport=Join-Path $findDirectory 'find-report.txt'
 if(Test-Path -LiteralPath $findReport) {Get-Content -LiteralPath $findReport}
 if($findTest.ExitCode -ne 0) {throw "Find-files tests failed; retained $findDirectory"}
 $menuDirectory=Join-Path $floppyDirectory 'floppy-menu'
 $menuTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-floppy-menu',('"'+$menuDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$menuTest.WaitForExit(30000)) {$menuTest.Kill();throw 'Floppy menu tests timed out.'}
 $menuReport=Join-Path $menuDirectory 'floppy-menu-report.txt'
 if(Test-Path -LiteralPath $menuReport) {Get-Content -LiteralPath $menuReport}
 if($menuTest.ExitCode -ne 0) {throw "Floppy menu tests failed; retained $menuDirectory"}
 $mediaDirectory=Join-Path $floppyDirectory 'media-control'
 $mediaTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-media-control',('"'+$mediaDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$mediaTest.WaitForExit(30000)) {$mediaTest.Kill();throw 'Media control tests timed out.'}
 $mediaReport=Join-Path $mediaDirectory 'media-control-report.txt'
 if(Test-Path -LiteralPath $mediaReport) {Get-Content -LiteralPath $mediaReport}
 if($mediaTest.ExitCode -ne 0) {throw "Media control tests failed; retained $mediaDirectory"}
 Write-Host "Simulated floppy fixtures retained at $floppyDirectory; no physical devices opened."
}
if($Test) {
 $auditFiles=@("$PSScriptRoot\bin\x64\HFVExplorer.exe","$PSScriptRoot\bin\x64\cd-probe.exe","$PSScriptRoot\bin\x64\floppy-probe.exe","$PSScriptRoot\bin\x64\compatibility-probe.exe","$PSScriptRoot\bin\tests\HFVExplorer.exe")
 & python "$PSScriptRoot\tests\audit-windows-imports.py" --report "$PSScriptRoot\bin\x64\windows-import-audit.json" @auditFiles
 if($LASTEXITCODE){throw 'Windows compatibility import audit failed.'}
 & python "$PSScriptRoot\tests\test-import-audit.py" "$PSScriptRoot\bin\x64\compatibility-probe.exe"
 if($LASTEXITCODE){throw 'Import auditor negative/positive tests failed.'}
}
