"""Conservative PE import/manifest checks. Does not replace testing on each OS."""
import argparse,json,struct
from pathlib import Path

MODULES=set('kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll shlwapi.dll ole32.dll oleaut32.dll comdlg32.dll comctl32.dll winmm.dll msimg32.dll imm32.dll usp10.dll uxtheme.dll version.dll ws2_32.dll ntdll.dll rpcrt4.dll setupapi.dll crypt32.dll wintrust.dll bcrypt.dll secur32.dll normaliz.dll netapi32.dll iphlpapi.dll oleacc.dll dwmapi.dll wtsapi32.dll psapi.dll urlmon.dll wininet.dll winspool.drv'.split())
POST7=set('GetSystemTimePreciseAsFileTime GetTempPath2W GetTempPath2A GetOverlappedResultEx SetThreadDescription GetThreadDescription GetThreadInformation SetThreadInformation SetProcessDpiAwareness GetDpiForWindow GetDpiForSystem AdjustWindowRectExForDpi CreateFile2 GetPackageFamilyName GetCurrentPackageId WaitOnAddress WakeByAddressAll WakeByAddressSingle InitializeSynchronizationBarrier EnterSynchronizationBarrier DeleteSynchronizationBarrier GetSystemTimeAdjustmentPrecise GetProcessMitigationPolicy SetProcessMitigationPolicy SetProcessDefaultCpuSets SetThreadSelectedCpuSets'.split())
MODULES.update({"oledlg.dll","gdiplus.dll"})
REVIEWED_ORDINALS=json.loads(Path(__file__).with_name("windows7-reviewed-ordinals.json").read_text())
GUIDS=['35138b9a-5d96-4fbd-8e2d-a2440225f93a','4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38','1f676c76-80e1-4239-95bb-83d0f6d0da78','8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a']

def inspect(path):
 data=Path(path).read_bytes()
 def u16(p):return struct.unpack_from('<H',data,p)[0]
 def u32(p):return struct.unpack_from('<I',data,p)[0]
 def u64(p):return struct.unpack_from('<Q',data,p)[0]
 pe=u32(0x3c);assert data[pe:pe+4]==b'PE\0\0','PE signature'
 opt=pe+24;assert u16(opt)==0x20b,'PE32+ required'
 sections=[];start=opt+u16(pe+20)
 for n in range(u16(pe+6)):
  p=start+40*n;sections.append((u32(p+12),max(u32(p+8),u32(p+16)),u32(p+20)))
 def offset(rva):
  if rva<u32(opt+60):return rva
  for base,size,file in sections:
   if base<=rva<base+size:return file+rva-base
  raise ValueError('Unmapped RVA '+hex(rva))
 def string(rva):
  at=offset(rva);return data[at:data.index(b'\0',at)].decode('ascii')
 imports={};ordinals=[]
 for directory,delayed in [(1,False),(13,True)]:
  rva=u32(opt+112+directory*8)
  if not rva:continue
  at=offset(rva);size=32 if delayed else 20
  while any(data[at:at+size]):
   fields=struct.unpack_from('<'+'I'*(size//4),data,at)
   adjustment=u64(opt+24) if delayed and not fields[0]&1 else 0
   name=string((fields[1] if delayed else fields[3])-adjustment).lower()
   thunk=(fields[4] if delayed else fields[0] or fields[4])-adjustment
   names=imports.setdefault(name,[]);pos=offset(thunk)
   while u64(pos):
    value=u64(pos)
    if value>>63:entry='#'+str(value&0xffff);ordinals.append(name+'!'+entry)
    else:entry=string(value-adjustment+2)
    names.append(entry);pos+=8
   at+=size
 errors=[]
 if u16(pe+4)!=0x8664:errors.append('Machine is not AMD64')
 os_version=(u16(opt+40),u16(opt+42));subsystem=(u16(opt+48),u16(opt+50))
 if os_version!=(6,1) or subsystem!=(6,1):errors.append('OS/subsystem version must be 6.1')
 for dll,names in imports.items():
  if dll not in MODULES:errors.append('Unreviewed or non-Windows-7 DLL: '+dll)
  for function in names:
   if function in POST7:errors.append('Post-Windows-7 static import: '+dll+'!'+function)
 unreviewed=[entry for entry in ordinals if entry.split('!')[1] not in REVIEWED_ORDINALS.get(entry.split('!')[0],{})]
 if unreviewed:errors.append('Ordinal imports require manual Windows 7 review: '+', '.join(unreviewed))
 for guid in GUIDS:
  if guid.encode() not in data.lower():errors.append('Missing supportedOS manifest GUID: '+guid)
 return {'file':str(path),'osVersion':os_version,'subsystemVersion':subsystem,'imports':imports,'errors':errors,'result':'FAIL' if errors else 'PASS','scope':'Static PE checks only; not proof of execution on older Windows.'}

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('files',nargs='+');parser.add_argument('--report');args=parser.parse_args()
 reports=[inspect(f) for f in args.files]
 if args.report:Path(args.report).write_text(json.dumps(reports,indent=2),encoding='utf-8')
 for r in reports:
  print(r['result']+': '+r['file']+'; OS/subsystem 6.1; '+str(sum(map(len,r['imports'].values())))+' imports reviewed against module/known-new-API rules')
  for error in r['errors']:print('  '+error)
 raise SystemExit(any(r['errors'] for r in reports))
