from pathlib import Path
from elftools.elf.elffile import ELFFile
import re,bisect,sys
module=Path(sys.argv[1]) if len(sys.argv)>1 else Path('charging-investigation/modules/vfcs-cms-core.ko')
with module.open('rb') as f:
 elf=ELFFile(f); sections={s.name:s.data() for s in elf.iter_sections() if s['sh_type']!='SHT_NOBITS'}
 names={}
 for sym in elf.get_section_by_name('.symtab').iter_symbols():
  if isinstance(sym['st_shndx'],int) and sym.name and sym['st_size']:
   sec=elf.get_section(sym['st_shndx']).name
   names.setdefault(sec,[]).append((sym['st_value'],sym['st_size'],sym.name))
 for v in names.values():v.sort()
text=Path('charging-investigation/analysis/'+module.stem+'.disassembly.txt').read_text()
def annotate(m):
 sec=m.group(1); off=int(m.group(2) or '0',16); result=m.group(0)
 if sec.startswith('.rodata.str'):
  data=sections.get(sec,b'')[off:]; result+=' ; '+repr(data.split(b'\0')[0].decode(errors='replace'))
 else:
  for addr,size,name in names.get(sec,[]):
   if addr<=off<addr+size:result+=' ; '+name+('+'+hex(off-addr) if off>addr else '');break
 return result
text=re.sub(r'(?<=\t)(\.[\w.]+)(?:\+0x([0-9a-f]+))?',annotate,text)
Path('charging-investigation/analysis/'+module.stem+'.annotated.txt').write_text(text,encoding='utf-8')
for name in sys.argv[2:] or ['cms_enable_store','switch_state_store','ai_charge_rate_mode_store','freq_test_store','debug_store']:
 m=re.search(r'^\w+ <'+re.escape(name)+r'>:\n.*?(?=^\w+ <|\Z)',text,re.M|re.S)
 print('===',name,'===\n'+(m.group() if m else 'NOT FOUND'))
