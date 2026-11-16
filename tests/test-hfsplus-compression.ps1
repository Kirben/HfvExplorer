# SPDX-License-Identifier: GPL-2.0-or-later
# Native fixture exports only: no mounted volumes or physical devices.
param([string]$Directory,[string]$Binary)
$ErrorActionPreference='Stop'
$here=Split-Path -Parent $MyInvocation.MyCommand.Path
if(!$Directory){$Directory=Join-Path ([IO.Path]::GetTempPath()) ('HFVExplorer-decmpfs-'+[Guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $Directory -ErrorAction Stop | Out-Null
$fixture=Join-Path $here 'fixtures\decmpfs'
$image=Join-Path $Directory 'native.dsk'
$input=[IO.File]::OpenRead((Join-Path $fixture 'decmpfs-hfsplus.img.gz'))
$output=[IO.File]::Create($image)
$gzip=New-Object IO.Compression.GZipStream($input,[IO.Compression.CompressionMode]::Decompress)
try{$buffer=New-Object byte[] 65536;while(($n=$gzip.Read($buffer,0,$buffer.Length)) -gt 0){$output.Write($buffer,0,$n)}}finally{$gzip.Close();$input.Close();$output.Close()}
if(!$Binary){$Binary=Join-Path $here 'hfsplus-tests.exe';if(!(Test-Path -LiteralPath $Binary)){$Binary=Join-Path $here '..\bin\x64\hfsplus-tests.exe'}}
$exe=$Binary
$passed=0
foreach($line in [IO.File]::ReadAllLines((Join-Path $fixture 'decmpfs-hfsplus.sha256'))){
 if(!$line.Trim() -or $line.StartsWith('#')){continue}
 $fields=$line -split '\s+',3;$hash=$fields[0];$method=[int]$fields[1];$path=$fields[2]
 $destination=Join-Path $Directory ($path.Replace('/','-'))
 $preference=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{$result=& $exe --export-hfsplus $image $path $destination 2>&1;$code=$LASTEXITCODE}finally{$ErrorActionPreference=$preference}
 if($code){throw "Native method $method export failed: $result"}
 $sha=[Security.Cryptography.SHA256]::Create();$stream=[IO.File]::OpenRead($destination)
 try{$actual=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}finally{$stream.Close();$sha.Clear()}
 if($actual -ne $hash){throw "Native method $method hash differs for $path"}
 Write-Host "PASS: macOS content hash: $path (method $method)";$passed++
}
Write-Host "PASS: $passed macOS-verified HFS+ exports, including LZBITMAP methods 13 and 14. Fixtures: $Directory"
