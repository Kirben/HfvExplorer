param([switch]$Test)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath
if(!$vs) { throw 'Install Visual Studio C++ tools and the MFC x86/x64 component first.' }
$msbuild=Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'Application build failed.' }
& $msbuild "$PSScriptRoot\tests\cd-probe.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'CD diagnostic build failed.' }
if($Test) {
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
 Write-Host "Large-image tests retained at $largeDirectory (6 GiB logical sparse file)."
}
