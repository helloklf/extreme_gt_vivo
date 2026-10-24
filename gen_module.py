#!/usr/bin/env python3
"""Generate a minimal loadable .ko from a freestanding clang .o.

Grafts the struct module template, vermagic and symbol CRCs from a stock
module of the SAME kernel build, so the result loads without the kernel
build tree.

Usage: gen_module.py <input.o> <output.ko> <module_name> [template.ko]
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

DEFAULT_TEMPLATE = 'modules/vfcs-cms-core.ko'

SHT_SYMTAB = 'SHT_SYMTAB'
SHT_STRTAB = 'SHT_STRTAB'
SHT_RELA = 'SHT_RELA'
SHT_NOBITS = 'SHT_NOBITS'
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
R_AARCH64_ABS64 = 257

SKIP_SECTIONS = ('.comment', '.llvm_addrsig', '.note.GNU-stack',
                 '.note.gnu.property', '.eh_frame')


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v)
def u64(v): return struct.pack('<Q', v)


def modinfo_get(elf, key):
    mi = elf.get_section_by_name('.modinfo')
    if not mi:
        return None
    for kv in mi.data().split(b'\x00'):
        if kv.startswith(key.encode() + b'='):
            return kv[len(key) + 1:].decode()
    return None


def main():
    obj_path, out_path, modname = sys.argv[1], sys.argv[2], sys.argv[3]
    template = sys.argv[4] if len(sys.argv) > 4 else DEFAULT_TEMPLATE
    if len(modname) > 55:
        sys.exit('module name too long')

    obj = ELFFile(open(obj_path, 'rb'))
    tmpl = ELFFile(open(template, 'rb'))

    vermagic = modinfo_get(tmpl, 'vermagic')
    tname = modinfo_get(tmpl, 'name')
    if not vermagic or not tname:
        sys.exit('template missing vermagic/name in .modinfo')

    # --- struct module template and version CRCs from the stock module
    tm_sec = tmpl.get_section_by_name('.gnu.linkonce.this_module')
    this_module = bytearray(tm_sec.data())
    name_off = this_module.find(tname.encode() + b'\x00')
    if name_off < 0:
        sys.exit('template name not found in this_module')
    this_module[name_off:name_off + 56] = modname.encode().ljust(56, b'\x00')
    init_rel_off = cleanup_rel_off = None
    tsym = tmpl.get_section_by_name('.symtab')
    for r in tmpl.get_section_by_name(
            '.rela.gnu.linkonce.this_module').iter_relocations():
        nm = tsym.get_symbol(r['r_info_sym']).name
        if nm == 'init_module':
            init_rel_off = r['r_offset']
        elif nm == 'cleanup_module':
            cleanup_rel_off = r['r_offset']
    if init_rel_off is None or cleanup_rel_off is None:
        sys.exit('template module relocs not found')

    vers = {}
    vdata = tmpl.get_section_by_name('__versions').data()
    for i in range(len(vdata) // 64):
        crc = struct.unpack('<I', vdata[i * 64:i * 64 + 4])[0]
        nm = vdata[i * 64 + 8:i * 64 + 64].split(b'\x00')[0].decode()
        vers[nm] = crc

    o_sym = obj.get_section_by_name('.symtab')
    o_syms = list(o_sym.iter_symbols())

    sym_init = sym_cleanup = None
    for s in o_syms:
        if s.name == 'init_module':
            sym_init = s
        elif s.name == 'cleanup_module':
            sym_cleanup = s
    if not sym_init or not sym_cleanup:
        sys.exit('init/cleanup missing in object')

    externs = [s.name for s in o_syms
               if s['st_shndx'] == 'SHN_UNDEF' and s.name]
    crcs = []
    for nm in externs + ['module_layout']:
        if nm not in vers:
            sys.exit('no CRC for ' + nm)
        crcs.append((nm, vers[nm]))

    # --- carried allocatable sections from the object
    carried = []
    for s in obj.iter_sections():
        if not (s['sh_flags'] & SHF_ALLOC):
            continue
        if s.name in SKIP_SECTIONS or s.name.startswith('.rela'):
            continue
        if s['sh_type'] == SHT_NOBITS:
            data = None
        else:
            data = s.data()
        carried.append([s.name, s['sh_flags'] & ~0x20, data,
                        s['sh_addralign'] or 1, s['sh_size']])
    carried.sort(key=lambda s: 0 if s[0] == '.text' else 1)

    modinfo = ('license=GPL\x00name=%s\x00vermagic=%s\x00depends=\x00'
               % (modname, vermagic)).encode()
    verdata = b''.join(u32(c) + u32(0) + n.encode().ljust(56, b'\x00')
                       for n, c in crcs)

    # --- final section list:
    # [name, type, flags, data(bytes|int size for NOBITS), align, link, info,
    #  entsize]
    sections = []
    for name, flags, data, al, size in carried:
        typ = SHT_NOBITS if data is None else 'SHT_PROGBITS'
        sections.append([name, typ, flags, data if data is not None else size,
                         al, 0, 0, 0])
    sections += [
        ['.plt', 'SHT_PROGBITS', SHF_ALLOC | SHF_EXECINSTR, b'\x00',
         8, 0, 0, 0],
        ['.init.plt', 'SHT_PROGBITS', SHF_ALLOC | SHF_EXECINSTR, b'\x00',
         8, 0, 0, 0],
        ['.modinfo', 'SHT_PROGBITS', SHF_ALLOC, modinfo, 8, 0, 0, 0],
        ['.gnu.linkonce.this_module', 'SHT_PROGBITS', SHF_ALLOC | SHF_WRITE,
         bytes(this_module), 64, 0, 0, 0],
        ['__versions', 'SHT_PROGBITS', SHF_ALLOC, verdata, 8, 0, 0, 0],
        ['.symtab', SHT_SYMTAB, 0, None, 8, 0, 0, 24],
        ['.strtab', SHT_STRTAB, 0, b'', 1, 0, 0, 0],
        ['.shstrtab', SHT_STRTAB, 0, None, 1, 0, 0, 0],
    ]
    o_rela_secs = [s for s in obj.iter_sections()
                   if s['sh_type'] == SHT_RELA]
    rela_names = []
    for rs in o_rela_secs:
        target = obj.get_section(rs['sh_info']).name
        new_name = '.rela' + target
        sections.append([new_name, SHT_RELA, 0, None, 8, 0, 0, 24])
        rela_names.append((new_name, target, rs))
    sections.append(['.rela.gnu.linkonce.this_module', SHT_RELA, 0, None,
                     8, 0, 0, 24])
    secidx = {s[0]: i + 1 for i, s in enumerate(sections)}

    # --- string tables
    strtab = b'\x00'
    symname_off = {}
    for nm in ['init_module', 'cleanup_module'] + externs:
        symname_off[nm] = len(strtab)
        strtab += nm.encode() + b'\x00'
    shstr = b'\x00'
    for s in sections:
        shstr += s[0].encode() + b'\x00'

    def sym(name_off, info, shndx, value, size=0):
        return struct.pack('<IBBHQQ', name_off, info, 0, shndx, value, size)

    syms = [b'\x00' * 24]
    sec_sym_idx = {}
    for s in sections:
        if s[1] != SHT_RELA and s[0] not in ('.symtab', '.strtab',
                                             '.shstrtab'):
            sec_sym_idx[s[0]] = len(syms)
            syms.append(sym(0, 3, secidx[s[0]], 0))       # LOCAL|SECTION
    idx_init = len(syms)
    syms.append(sym(symname_off['init_module'], 0x12,
                    secidx['.text'], sym_init['st_value']))
    idx_cleanup = len(syms)
    syms.append(sym(symname_off['cleanup_module'], 0x12,
                    secidx['.text'], sym_cleanup['st_value']))
    ext_idx = {}
    for nm in externs:
        ext_idx[nm] = len(syms)
        syms.append(sym(symname_off[nm], 0x10, 0, 0))     # GLOBAL UNDEF

    # --- remapped relocations
    for new_name, target, rs in rela_names:
        rela = b''
        for r in rs.iter_relocations():
            osym = o_syms[r['r_info_sym']]
            if osym['st_shndx'] == 'SHN_UNDEF':
                nidx = ext_idx[osym.name]
            else:
                osec = obj.get_section(osym['st_shndx'])
                nidx = sec_sym_idx[osec.name]
            rela += (u64(r['r_offset'])
                     + u64((nidx << 32) | r['r_info_type'])
                     + struct.pack('<q', r['r_addend']))
        ent = sections[secidx[new_name] - 1]
        ent[3] = rela
        ent[5] = secidx['.symtab']
        ent[6] = secidx[target]
    rela_tm = (u64(init_rel_off)
               + u64((idx_init << 32) | R_AARCH64_ABS64) + u64(0)
               + u64(cleanup_rel_off)
               + u64((idx_cleanup << 32) | R_AARCH64_ABS64) + u64(0))
    ent = sections[secidx['.rela.gnu.linkonce.this_module'] - 1]
    ent[3] = rela_tm
    ent[5] = secidx['.symtab']
    ent[6] = secidx['.gnu.linkonce.this_module']

    sections[secidx['.symtab'] - 1][3] = b''.join(syms)
    sections[secidx['.symtab'] - 1][5] = secidx['.strtab']
    sections[secidx['.symtab'] - 1][6] = idx_init
    sections[secidx['.strtab'] - 1][3] = strtab
    sections[secidx['.shstrtab'] - 1][3] = shstr

    # --- lay out file
    sht_int = {'SHT_PROGBITS': 1, 'SHT_SYMTAB': 2, 'SHT_STRTAB': 3,
               'SHT_RELA': 4, 'SHT_NOBITS': 8}
    out = bytearray()
    out += b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\x00' * 8
    out += u16(1)          # ET_REL
    out += u16(183)        # EM_AARCH64
    out += u32(1)          # version
    out += u64(0) * 3      # entry, phoff, shoff (patched later)
    out += u32(0)          # flags
    out += u16(64)         # ehsize
    out += u16(0) + u16(0)  # phentsize, phnum
    out += u16(64) + u16(len(sections) + 1) + u16(secidx['.shstrtab'])

    offsets = []
    for s in sections:
        name, typ, flags, data, al = s[0], s[1], s[2], s[3], s[4]
        if typ == SHT_NOBITS:
            offsets.append((0, data))
            continue
        pos = align_up(len(out), al)
        out += b'\x00' * (pos - len(out))
        offsets.append((pos, len(data)))
        out += data

    shoff = align_up(len(out), 8)
    out += b'\x00' * (shoff - len(out))
    struct.pack_into('<Q', out, 0x28, shoff)  # e_shoff

    name_offs = {}
    cur = 1
    for s in sections:
        name_offs[s[0]] = cur
        cur += len(s[0]) + 1
    shdr = b'\x00' * 64
    for i, s in enumerate(sections):
        name, typ, flags, _, al, link, info, entsz = s
        off, size = offsets[i]
        shdr += (u32(name_offs[name]) + u32(sht_int[typ]) + u64(flags)
                 + u64(0) + u64(off) + u64(size) + u32(link) + u32(info)
                 + u64(al) + u64(entsz))
    out += shdr

    open(out_path, 'wb').write(bytes(out))
    print('wrote', out_path, len(out), 'bytes')


if __name__ == '__main__':
    main()
