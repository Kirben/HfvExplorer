param([switch]$Test,[switch]$TestFloppy,[switch]$TestCd,[switch]$TestHfsPlus,[switch]$TestBinHex)
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
 & "$PSScriptRoot\tests\test-hfsplus-compression.ps1" -Directory (Join-Path $hfsPlusDirectory 'native-compression')
 if($LASTEXITCODE){throw 'Native compressed-file tests failed.'}
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" --large (Join-Path $hfsPlusDirectory 'large.dsk')
 if($LASTEXITCODE){throw "HFS+ large-fork tests failed; retained $hfsPlusDirectory"}
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" --large-hfsx (Join-Path $hfsPlusDirectory 'large-hfsx.dsk')
 if($LASTEXITCODE){throw "HFSX large-fork tests failed; retained $hfsPlusDirectory"}
 & $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Test /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'HFS+ hidden UI test build failed.'}
 & "$PSScriptRoot\bin\x64\hfsplus-tests.exe" --feature-fixture (Join-Path $hfsPlusDirectory 'feature-links.dsk')
 if($LASTEXITCODE){throw 'HFS+ feature UI fixture failed.'}
 $hfsPlusUi=Join-Path $hfsPlusDirectory 'ui'
 $hfsPlusTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-hfsplus-ui',('"'+$hfsPlusUi+'"')) -WindowStyle Hidden -PassThru
 if(!$hfsPlusTest.WaitForExit(30000)){$hfsPlusTest.Kill();throw 'HFS+ hidden UI test timed out.'}
 if($hfsPlusTest.ExitCode -ne 0){throw "HFS+ hidden UI test failed; retained $hfsPlusUi"}
 Get-Content (Join-Path $hfsPlusUi 'hfsplus-ui-report.txt')
 $floppyCopies=Join-Path $hfsPlusDirectory 'floppy-copy-regressions'
 $floppyCopiesTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-floppy-copy-regressions',('"'+$floppyCopies+'"')) -WindowStyle Hidden -PassThru
 if(!$floppyCopiesTest.WaitForExit(60000)){$floppyCopiesTest.Kill();throw 'Floppy copy regression tests timed out.'}
 Get-Content (Join-Path $floppyCopies 'floppy-copy-regressions-report.txt')
 if($floppyCopiesTest.ExitCode -ne 0){throw "Floppy copy regressions failed; retained $floppyCopies"}
 $background=Join-Path $hfsPlusDirectory 'background-copies'
 $backgroundTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-background-copies',('"'+$background+'"')) -WindowStyle Hidden -PassThru
 if(!$backgroundTest.WaitForExit(60000)){$backgroundTest.Kill();throw 'Background copy tests timed out.'}
 Get-Content (Join-Path $background 'background-copy-report.txt')
 if($backgroundTest.ExitCode -ne 0){throw "Background copy tests failed; retained $background"}
 $transfers=Join-Path $hfsPlusDirectory 'browser-transfers'
 $transfersTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-browser-transfers',('"'+$transfers+'"')) -WindowStyle Hidden -PassThru
 if(!$transfersTest.WaitForExit(60000)){$transfersTest.Kill();throw 'Cross-window transfer tests timed out.'}
 Get-Content (Join-Path $transfers 'browser-transfers-report.txt')
 if($transfersTest.ExitCode -ne 0){throw "Cross-window transfer tests failed; retained $transfers"}
 $windows=Join-Path $hfsPlusDirectory 'browser-windows'
 $windowsTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-browser-windows',('"'+$windows+'"')) -WindowStyle Hidden -PassThru
 if(!$windowsTest.WaitForExit(60000)){$windowsTest.Kill();throw 'Independent browser window tests timed out.'}
 Get-Content (Join-Path $windows 'browser-windows-report.txt')
 if($windowsTest.ExitCode -ne 0){throw "Independent browser window tests failed; retained $windows"}
 $coordination=Join-Path $hfsPlusDirectory 'volume-coordination'
 $coordinationTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-volume-coordination',('"'+$coordination+'"')) -WindowStyle Hidden -PassThru
 if(!$coordinationTest.WaitForExit(60000)){$coordinationTest.Kill();throw 'Volume coordination tests timed out.'}
 Get-Content (Join-Path $coordination 'volume-coordination-report.txt')
 if($coordinationTest.ExitCode -ne 0){throw "Volume coordination tests failed; retained $coordination"}
 $operations=Join-Path $hfsPlusDirectory 'file-operations'
 $operationTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-file-operations',('"'+$operations+'"')) -WindowStyle Hidden -PassThru
 if(!$operationTest.WaitForExit(60000)){$operationTest.Kill();throw 'Independent file-operation tests timed out.'}
 Get-Content (Join-Path $operations 'file-operations-report.txt')
 if($operationTest.ExitCode -ne 0){throw "Independent file-operation tests failed; retained $operations"}
 $sessions=Join-Path $hfsPlusDirectory 'browser-sessions'
 $sessionTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-browser-sessions',('"'+$sessions+'"')) -WindowStyle Hidden -PassThru
 if(!$sessionTest.WaitForExit(60000)){$sessionTest.Kill();throw 'Browser session tests timed out.'}
 Get-Content (Join-Path $sessions 'browser-session-report.txt')
 if($sessionTest.ExitCode -ne 0){throw "Browser session tests failed; retained $sessions"}
 $shared=Join-Path $hfsPlusDirectory 'shared-browser'
 $sharedTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-shared-browser',('"'+$shared+'"')) -WindowStyle Hidden -PassThru
 if(!$sharedTest.WaitForExit(60000)){$sharedTest.Kill();throw 'Shared browser tests timed out.'}
 Get-Content (Join-Path $shared 'shared-browser-report.txt')
 if($sharedTest.ExitCode -ne 0){throw "Shared browser tests failed; retained $shared"}
 $exclusive=Join-Path $hfsPlusDirectory 'storage-exclusive'
 $exclusiveTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-storage-exclusive',('"'+$exclusive+'"')) -WindowStyle Hidden -PassThru
 if(!$exclusiveTest.WaitForExit(60000)){$exclusiveTest.Kill();throw 'Exclusive access tests timed out.'}
 Get-Content (Join-Path $exclusive 'exclusive-access-report.txt')
 if($exclusiveTest.ExitCode -ne 0){throw "Exclusive access tests failed; retained $exclusive"}
 $mainMac=Join-Path $hfsPlusDirectory 'main-browser'
 $mainMacTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-main-mac-browser',('"'+$mainMac+'"')) -WindowStyle Hidden -PassThru
 if(!$mainMacTest.WaitForExit(60000)){$mainMacTest.Kill();throw 'Unified main-browser tests timed out.'}
 if($mainMacTest.ExitCode -ne 0){throw "Unified main-browser tests failed; retained $mainMac"}
 Get-Content (Join-Path $mainMac 'main-mac-browser-report.txt')
 $hfsPlusCd=Join-Path $hfsPlusDirectory 'cd'
 $cdTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-hfsplus-cd',('"'+$hfsPlusCd+'"')) -WindowStyle Hidden -PassThru
 if(!$cdTest.WaitForExit(60000)){$cdTest.Kill();throw 'HFS+ CD tests timed out.'}
 if($cdTest.ExitCode -ne 0){throw 'HFS+ CD tests failed.'}
 Get-Content (Join-Path $hfsPlusCd 'hfsplus-cd-report.txt')
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
if($Test -or $TestBinHex) {
 & $msbuild "$PSScriptRoot\tests\binhex5-tests.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE){throw 'BinHex 5 test build failed.'}
 $binhexDirectory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-BinHex5-'+[Guid]::NewGuid().ToString('N'))
 & "$PSScriptRoot\bin\x64\binhex5-tests.exe" $binhexDirectory
 if($LASTEXITCODE){throw 'BinHex 5 tests failed.'}
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
 $mutationDirectory=Join-Path $floppyDirectory 'file-mutations'
 $mutationTest=Start-Process -FilePath "$PSScriptRoot\bin\tests\HFVExplorer.exe" -ArgumentList @('--verify-file-mutations',('"'+$mutationDirectory+'"')) -WindowStyle Hidden -PassThru
 if(!$mutationTest.WaitForExit(60000)){$mutationTest.Kill();throw 'File mutation tests timed out.'}
 if($mutationTest.ExitCode -ne 0){throw "File mutation tests failed; retained $mutationDirectory"}
 Get-Content (Join-Path $mutationDirectory 'file-mutations-report.txt')
 Write-Host "Simulated floppy fixtures retained at $floppyDirectory; no physical devices opened."
}
if($Test) {
 $auditFiles=@("$PSScriptRoot\bin\x64\HFVExplorer.exe","$PSScriptRoot\bin\x64\cd-probe.exe","$PSScriptRoot\bin\x64\floppy-probe.exe","$PSScriptRoot\bin\x64\compatibility-probe.exe","$PSScriptRoot\bin\tests\HFVExplorer.exe")
 & python "$PSScriptRoot\tests\audit-windows-imports.py" --report "$PSScriptRoot\bin\x64\windows-import-audit.json" @auditFiles
 if($LASTEXITCODE){throw 'Windows compatibility import audit failed.'}
 & python "$PSScriptRoot\tests\test-import-audit.py" "$PSScriptRoot\bin\x64\compatibility-probe.exe"
 if($LASTEXITCODE){throw 'Import auditor negative/positive tests failed.'}
}
