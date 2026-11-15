param([switch]$Test)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath
if(!$vs) { throw 'Install Visual Studio C++ tools and the MFC x86/x64 component first.' }
$msbuild=Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild "$PSScriptRoot\src\HFVExplorer-x64.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if($LASTEXITCODE) { throw 'Application build failed.' }
if($Test) {
 & $msbuild "$PSScriptRoot\tests\image-roundtrip.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
 if($LASTEXITCODE) { throw 'Test build failed.' }
 $testImage=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-'+[Guid]::NewGuid().ToString('N')+'.dsk')
 & "$PSScriptRoot\bin\x64\image-roundtrip.exe" $testImage
 if($LASTEXITCODE) { throw "Image test failed; retained $testImage" }
 Write-Host "Test image retained at $testImage"
}
