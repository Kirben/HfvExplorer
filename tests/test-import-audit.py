"""Negative/positive tests against a real built PE, without executing mutations."""
import importlib.util,sys,tempfile,unittest,struct,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('audit',Path(__file__).with_name('audit-windows-imports.py'));audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
reference=Path(sys.argv.pop(1));original=reference.read_bytes()
class ImportAuditTests(unittest.TestCase):
 def inspect(self,data):
  with tempfile.TemporaryDirectory() as folder:
   file=Path(folder)/'fixture.bin';file.write_bytes(data);return audit.inspect(file)
 def test_reference(self):self.assertEqual(self.inspect(original)['errors'],[])
 def test_post_windows7_import(self):
  baseline=audit.inspect(reference);name=next(n for names in baseline['imports'].values() for n in names if not n.startswith('#') and len(n)>=13)
  old=name.encode()+b'\0';new=b'WaitOnAddress\0'.ljust(len(old),b'\0')
  changed=original.replace(old,new);self.assertNotEqual(changed,original)
  self.assertTrue(any('Post-Windows-7' in e for e in self.inspect(changed)['errors']))
 def test_unknown_dll(self):
  changed=re.sub(b'kernel32.dll\0',lambda m:b'combase.dll\0'.ljust(len(m[0]),b'\0'),original,flags=re.I)
  self.assertTrue(any('combase.dll' in e for e in self.inspect(changed)['errors']))
 def test_new_subsystem(self):
  data=bytearray(original);pe=struct.unpack_from('<I',data,0x3c)[0];struct.pack_into('<H',data,pe+24+48,10)
  self.assertTrue(any('OS/subsystem' in e for e in self.inspect(data)['errors']))
 def test_missing_manifest_target(self):
  changed=original.replace(audit.GUIDS[0].encode(),b'0'*len(audit.GUIDS[0]))
  self.assertTrue(any('Missing supportedOS' in e for e in self.inspect(changed)['errors']))
unittest.main()
