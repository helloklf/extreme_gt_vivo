#!/usr/bin/env python3
"""make-patch.py — one-click fbon->fboff bright-screen charge patch.

Pipeline: collect (adb pull stock module + config + DT tables)
        -> analyze (gates: CFI/MODVERSIONS/SIG_FORCE, extract template info)
        -> build (device-specific C -> clang -> gen_module.py graft)
        -> load & verify (push, insmod, check dmesg)

Usage:
  py -3 make-patch.py --serial 192.168.1.111:33445          # full patch flow
  py -3 make-patch.py --serial ... --hello                  # only verify module injection works
  py -3 make-patch.py --serial ... --scan-lo 0xa000 --scan-hi 0xe000

Fail-safe: the patch module only writes after matching complete DT table
contents; on mismatch it logs and aborts without writing.
"""
import argparse
import os
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile

WORK = 'out'
GEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen_module.py')

CMS_MODULE_CANDIDATES = [
    '/vendor_dlkm/lib/modules/vfcs-cms-core.ko',
    '/vendor_dlkm/lib/modules/vfcs_cms_core.ko',
    '/vendor_dlkm/lib/modules/vfcs_cms.ko',
    '/vendor_dlkm/lib/modules/vfcs-cms.ko',
]
TABLE_PROPS = ['vivo,sp-primary-fbon-tc-data', 'vivo,sp-primary-fboff-tc-data',
               'vivo,eq-primary-fbon-tc-data', 'vivo,eq-primary-fboff-tc-data']

C_TEMPLATE = r'''
typedef unsigned long u64;
typedef unsigned int u32;
typedef int s32;
struct power_supply;
extern struct power_supply *power_supply_get_by_name(const char *name);
extern void *power_supply_get_drvdata(struct power_supply *psy);
extern int _printk(const char *fmt, ...);

@ARRAYS@

static int kptr_ok(u64 p)
{
    return ((p & 0xffff000000000000UL) == 0xffff000000000000UL) && !(p & 7);
}

static int match(const s32 *t, const s32 *ref, int n)
{
    int k;
    for (k = 0; k < n; k++)
        if (t[k] != ref[k])
            return 0;
    return 1;
}

int init_module(void)
{
    struct power_supply *psy = power_supply_get_by_name("cms");
    char *chip;
    s32 *bufs[8];
    int n = 0, o, i, k;

    if (!psy) { _printk("fbpat: no cms psy\n"); return -19; }
    chip = (char *)power_supply_get_drvdata(psy);
    if (!chip) { _printk("fbpat: no drvdata\n"); return -22; }
    _printk("fbpat: chip=%px\n", chip);

    for (o = 0; o < (int)NOFFS; o++) {
        for (i = 0; i < 6; i++) {
            u64 p = *(volatile u64 *)(chip + data_offs[o] + i * 8);
            s32 *t;
            if (!kptr_ok(p))
                continue;
            t = (s32 *)(u64)p;
            if (match(t, fbon0, fbon0_len) ||
                (fbon1_len && match(t, fbon1, fbon1_len))) {
                _printk("fbpat: fbon table at %px (off %x idx %d)\n",
                        (void *)t, data_offs[o], i);
                if (n < 8)
                    bufs[n++] = t;
            }
        }
    }
    if (n != 2) {
        _printk("fbpat: found %d fbon tables, expected 2, abort\n", n);
        return -22;
    }
    for (o = 0; o < n; o++) {
        volatile s32 *t = bufs[o];
        for (k = 0; k < fboff_len; k++)
            t[k] = fboff[k];
        _printk("fbpat: patched %px\n", (void *)t);
    }
    _printk("fbpat: done\n");
    return 0;
}

void cleanup_module(void) {}
'''



def sh(args, **kw):
    return subprocess.run(args, capture_output=True, **kw)


def adb(serial, cmd, raw=False):
    args = ['adb']
    if serial:
        args += ['-s', serial]
    if raw:
        args += ['exec-out', cmd]
    else:
        args += ['shell', cmd]
    r = sh(args)
    return r.stdout if raw else r.stdout.decode(errors='replace')


def adb_pull(serial, remote, local):
    # vendor_dlkm is unreadable by shell; bounce through /data/local/tmp
    adb(serial, "su -c 'cp -f %s /data/local/tmp/_pull && chmod 644 "
                "/data/local/tmp/_pull'" % remote)
    r = sh(['adb', '-s', serial, 'pull', '/data/local/tmp/_pull', local])
    adb(serial, "su -c 'rm -f /data/local/tmp/_pull'")
    return r.returncode == 0 and os.path.exists(local)


def find_ndk_clang():
    cands = []
    for base in (os.environ.get('ANDROID_NDK_HOME'), 'D:/Android/SDK/ndk'):
        if base and os.path.isdir(base):
            for root, _, files in os.walk(base):
                if 'clang.exe' in files or 'clang' in files:
                    cands.append(root)
    if not cands:
        sys.exit('no NDK clang found')
    cands.sort()
    return os.path.join(cands[-1], 'clang.exe' if os.name == 'nt' else 'clang')


def parse_config(gz_path):
    import gzip
    txt = gzip.open(gz_path, 'rt', errors='replace').read()
    return txt


def extract_hashes(template_path):
    """CFI hashes preceding init_module/cleanup_module, if present."""
    elf = ELFFile(open(template_path, 'rb'))
    symtab = elf.get_section_by_name('.symtab')
    if not symtab:
        return None, None
    out = {}
    for s in symtab.iter_symbols():
        if s.name in ('init_module', 'cleanup_module') and s['st_value'] >= 4:
            sec = elf.get_section(s['st_shndx'])
            data = sec.data()
            v = s['st_value']
            out[s.name] = struct.unpack('<I', data[v - 4:v])[0]
    return out.get('init_module'), out.get('cleanup_module')


def find_llvm_objdump():
    cands = []
    for base in (os.environ.get('ANDROID_NDK_HOME'), 'D:/Android/SDK/ndk'):
        if base and os.path.isdir(base):
            for root, _, files in os.walk(base):
                if 'llvm-objdump.exe' in files or 'llvm-objdump' in files:
                    cands.append(root)
    if not cands:
        sys.exit('no llvm-objdump found')
    cands.sort()
    return os.path.join(cands[-1],
                        'llvm-objdump.exe' if os.name == 'nt' else 'llvm-objdump')


def extract_offsets(ko_path):
    """Table-pointer struct offsets: immediates of mov w8-w13 in
    vfcs_cms_get_intell_chg_current (the per-(param,fb) 6-table clusters)."""
    objdump = find_llvm_objdump()
    r = sh([objdump, '-d',
            '--disassemble-symbols=vfcs_cms_get_intell_chg_current', ko_path])
    if r.returncode:
        return []
    offs = set()
    for m in re.finditer(r'mov\s+w(?:8|9|1[0-3]), #(0x[0-9a-f]+|\d+)',
                         r.stdout.decode(errors='replace')):
        v = int(m.group(1), 0)
        if 0x8000 <= v < 0x14000:
            offs.add(v)
    return sorted(offs)


def read_dt_table(serial, node, prop):
    data = adb(serial, "su -c 'cat %s/%s'" % (node, prop), raw=True)
    if len(data) % 4:
        return None
    # device tree cells are big-endian u32
    return list(struct.unpack('>%di' % (len(data) // 4), data))



def read_dt_table_file(path):
    """Read a DT table either as raw big-endian-u32 property file or as
    a text file of ints (comma/space/newline separated)."""
    data = open(path, 'rb').read()
    try:
        txt = data.decode('ascii')
        if all(c in '0123456789-, \t\n' for c in txt):
            return [int(x) for x in re.split(r'[\s,]+', txt.strip()) if x]
    except UnicodeDecodeError:
        pass
    if len(data) % 4:
        return None
    return list(struct.unpack('>%di' % (len(data) // 4), data))


def offline_main(args):
    """Build fbpat.ko from pre-collected files, no adb involved."""
    d = args.from_dir
    if not os.path.isdir(d):
        sys.exit('not a directory: ' + d)
    os.makedirs(WORK, exist_ok=True)

    # template module
    tmpl = None
    for f in sorted(os.listdir(d)):
        if f.endswith('.ko') and 'cms' in f:
            tmpl = os.path.join(d, f)
            break
    if not tmpl:
        sys.exit('no *cms*.ko in %s' % d)
    print('[*] template', tmpl)

    # config (optional but recommended)
    cfg = ''
    for f in ('config.gz', 'config.txt', 'config'):
        p = os.path.join(d, f)
        if os.path.exists(p):
            cfg = parse_config(p) if f.endswith('.gz') else open(
                p, errors='replace').read()
            break
    if not cfg:
        print('[!] no config(.gz/.txt) in dir; assuming CFI enforced and '
              'signatures not forced')
    if 'CONFIG_MODULE_SIG_FORCE=y' in cfg:
        sys.exit('kernel forces module signatures, cannot load custom module')
    cfi = ('# CONFIG_CFI_CLANG is not set' not in cfg
           and 'CONFIG_CFI_PERMISSIVE=y' not in cfg)
    # without config we do not know: default to inserting hashes (harmless
    # when CFI is off, fatal to omit when on)
    if 'CONFIG_CFI_CLANG=y' in cfg or not cfg:
        cfi = True
    print('[*] cfi=%s' % cfi)

    h_init = h_cleanup = None
    if cfi:
        h_init, h_cleanup = extract_hashes(tmpl)
        if h_init is None:
            print('[!] template has no CFI hash prefix; building without')
            cfi = False
        else:
            print('[*] cfi hashes: init=%08x cleanup=%08x'
                  % (h_init, h_cleanup or 0))

    # tables: plain files, or inside a tar of the cms DT node (e.g. cmsdt.tar)
    tables = {}
    tars = [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.tar')]
    tar_data = {}
    for tp in tars:
        import tarfile
        try:
            with tarfile.open(tp) as tf:
                for m in tf.getmembers():
                    base = os.path.basename(m.name)
                    if base in TABLE_PROPS and m.isfile():
                        tar_data[base] = tf.extractfile(m).read()
        except tarfile.TarError:
            continue
    for prop in TABLE_PROPS:
        for cand in (os.path.join(d, prop), os.path.join(d, prop + '.txt')):
            if os.path.exists(cand):
                t = read_dt_table_file(cand)
                if t:
                    tables[prop] = t
                break
        if prop not in tables and prop in tar_data:
            data = tar_data[prop]
            if not len(data) % 4:
                tables[prop] = list(struct.unpack('>%di' % (len(data) // 4),
                                                  data))
    if tar_data:
        print('[*] read %d table(s) from tar' % len(tar_data))
    fbon0 = tables.get('vivo,sp-primary-fbon-tc-data')
    fboff = tables.get('vivo,sp-primary-fboff-tc-data')
    fbon1 = tables.get('vivo,eq-primary-fbon-tc-data') or []
    if fbon1 == fbon0:
        fbon1 = []
    if not (fbon0 and fboff):
        sys.exit('missing fbon/fboff table files - patch not applicable '
                 'to this device/firmware')
    if len(fboff) > len(fbon0) or (fbon1 and len(fboff) > len(fbon1)):
        sys.exit('fboff longer than fbon, cannot overwrite in place')
    print('[*] tables: fbon0=%d ints, fboff=%d ints, fbon1=%d ints'
          % (len(fbon0), len(fboff), len(fbon1)))

    offs = extract_offsets(tmpl)
    print('[*] candidate table offsets:', [hex(o) for o in offs])
    if len(offs) < 4:
        sys.exit('offset extraction failed (got %d) - manual analysis needed'
                 % len(offs))

    write_and_build(fbon0, fboff, fbon1, offs, tmpl, cfi, h_init, h_cleanup)
    print('[+] built %s' % os.path.join(WORK, 'fbpat.ko'))
    print('    push & load on target device:')
    print('    adb push %s /data/local/tmp/fbpat.ko' % os.path.join(WORK,
                                                                  'fbpat.ko'))
    print("    adb shell su -c 'insmod /data/local/tmp/fbpat.ko'")
    print("    check: adb shell su -c 'dmesg | grep fbpat'")



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--serial')
    ap.add_argument('--hello', action='store_true')
    ap.add_argument('--from-dir', metavar='DIR',
                    help='offline build: DIR holds vfcs-cms-core.ko, config.gz '
                         'and the four *-primary-fbon/fboff-tc-data files')
    args = ap.parse_args()
    if args.from_dir:
        offline_main(args)
        return

    devs = [l.split()[0] for l in
            sh(['adb', 'devices']).stdout.decode().splitlines()[1:]
            if l.strip().endswith('device')]
    if not args.serial:
        if len(devs) != 1:
            sys.exit('multiple/no devices, use --serial: %s' % devs)
        args.serial = devs[0]
    os.makedirs(WORK, exist_ok=True)

    print('[*] device', args.serial)
    uid = adb(args.serial, "su -c 'id -u'").strip()
    if uid != '0':
        sys.exit('no root')

    # --- config gates
    cfg_path = os.path.join(WORK, 'config.gz')
    adb(args.serial, "su -c 'cat /proc/config.gz > /data/local/tmp/_cfg.gz'")
    if not adb_pull(args.serial, '/data/local/tmp/_cfg.gz', cfg_path):
        sys.exit('no /proc/config.gz')
    cfg = parse_config(cfg_path)
    if 'CONFIG_MODULE_SIG_FORCE=y' in cfg:
        sys.exit('kernel forces module signatures, cannot load custom module')
    cfi = ('CONFIG_CFI_CLANG=y' in cfg
           and 'CONFIG_CFI_PERMISSIVE=y' not in cfg)
    print('[*] config ok (cfi=%s)' % cfi)

    # --- template module
    tmpl_local = os.path.join(WORK, 'template.ko')
    got = None
    for cand in CMS_MODULE_CANDIDATES:
        if adb_pull(args.serial, cand, tmpl_local):
            got = cand
            break
    if not got:
        # fall back to any small loadable module for template data
        lst = adb(args.serial, "su -c 'ls /vendor_dlkm/lib/modules/*.ko'")
        files = [l.strip() for l in lst.splitlines() if l.strip().endswith('.ko')]
        for f in files[:50]:
            if adb_pull(args.serial, f, tmpl_local):
                got = f
                break
    if not got:
        sys.exit('no template .ko found on device')
    print('[*] template', got)
    cms_ok = 'cms' in os.path.basename(got)

    # --- CFI hashes from template
    h_init = h_cleanup = None
    if cfi:
        h_init, h_cleanup = extract_hashes(tmpl_local)
        if h_init is None:
            print('[!] template has no CFI hash prefix; trying without')
            cfi = False
        else:
            print('[*] cfi hashes: init=%08x cleanup=%08x'
                  % (h_init, h_cleanup or 0))

    # --- DT tables
    node = adb(args.serial,
               "su -c 'find /sys/firmware/devicetree/base -name "
               "vivo,sp-primary-fbon-tc-data 2>/dev/null | head -1'").strip()
    tables = {}
    if node:
        node = os.path.dirname(node).replace('\\', '/')
        print('[*] DT node', node)
        for prop in TABLE_PROPS:
            t = read_dt_table(args.serial, node, prop)
            if t:
                tables[prop] = t
    fbon0 = tables.get('vivo,sp-primary-fbon-tc-data')
    fboff = tables.get('vivo,sp-primary-fboff-tc-data')
    fbon1 = tables.get('vivo,eq-primary-fbon-tc-data') or []
    if fbon1 == fbon0:
        fbon1 = []          # identical content, one matcher is enough

    if args.hello or not (fbon0 and fboff) or not cms_ok:
        if not args.hello:
            print('[!] this device lacks the fbon/fboff DT tables '
                  '(or cms module file) - patch not applicable. '
                  'Module loading was NOT attempted. To test raw module '
                  'injection on this device, re-run with --hello '
                  '(warning: on this MTK/6.1 kernel a hand-built module '
                  'hung the loader once - only retry for debugging).')
        else:
            write_and_build(None, None, None, args, tmpl_local, cfi,
                            h_init, h_cleanup)
            load_and_verify(args.serial, hello=True)
        return

    # equal length required for in-place overwrite
    if len(fboff) > len(fbon0):
        sys.exit('fboff longer than fbon, cannot overwrite in place')
    if fbon1 and len(fboff) > len(fbon1):
        sys.exit('fboff longer than eq-fbon, cannot overwrite in place')

    print('[*] tables: fbon0=%d ints, fboff=%d ints, fbon1=%d ints'
          % (len(fbon0), len(fboff), len(fbon1)))

    # --- table pointer offsets from the cms module disassembly
    offs = extract_offsets(tmpl_local)
    print('[*] candidate table offsets:', [hex(o) for o in offs])
    if len(offs) < 4:
        sys.exit('offset extraction failed (got %d) - manual analysis needed'
                 % len(offs))

    write_and_build(fbon0, fboff, fbon1, offs, tmpl_local, cfi,
                    h_init, h_cleanup)
    load_and_verify(args.serial, hello=False)


def arr(name, vals):
    return 'static const s32 %s[%d] = { %s };\nstatic const int %s_len = %d;' % (
        name, len(vals), ', '.join(str(v) for v in vals), name, len(vals))


def write_and_build(fbon0, fboff, fbon1, offs, tmpl, cfi, h_init, h_cleanup):
    if fbon0 is None:
        arrays = ''
        src = ('typedef unsigned long u64;\nextern int _printk(const char*,...);\n'
               'int init_module(void){_printk("fbpat: hello\\n");return 0;}\n'
               'void cleanup_module(void){}\n')
    else:
        arrays = (arr('fbon0', fbon0) + '\n' + arr('fboff', fboff) + '\n'
                  + (arr('fbon1', fbon1) if fbon1
                     else 'static const s32 *fbon1 = 0;\n'
                          'static const int fbon1_len = 0;') + '\n')
        arrays += ('static const u32 data_offs[%d] = { %s };\n'
                   '#define NOFFS %d\n'
                   % (len(offs), ', '.join('0x%x' % o for o in offs),
                      len(offs)))
        src = C_TEMPLATE.replace('@ARRAYS@', arrays)
    c_path = os.path.join(WORK, 'fbpat_dev.c')
    open(c_path, 'w').write(src)

    clang = find_ndk_clang()
    s_path = os.path.join(WORK, 'fbpat_dev.s')
    o_path = os.path.join(WORK, 'fbpat_dev.o')
    ko_path = os.path.join(WORK, 'fbpat.ko')
    r = sh([clang, '--target=aarch64-linux-gnu', '-nostdlib', '-ffreestanding',
            '-fno-builtin', '-fno-stack-protector', '-fno-jump-tables',
            '-fno-pic', '-mno-outline-atomics', '-mbranch-protection=standard',
            '-ffixed-x18', '-O2', '-S', c_path, '-o', s_path])
    if r.returncode:
        sys.exit('clang -S failed: ' + r.stderr.decode(errors='replace'))
    if cfi:
        txt = open(s_path).read()
        txt = re.sub(r'(?m)^init_module:.*$',
                     '\t.word\t0x%08x\ninit_module:' % h_init, txt)
        if h_cleanup:
            txt = re.sub(r'(?m)^cleanup_module:.*$',
                         '\t.word\t0x%08x\ncleanup_module:' % h_cleanup, txt)
        open(s_path, 'w').write(txt)
    r = sh([clang, '--target=aarch64-linux-gnu', '-c', s_path, '-o', o_path])
    if r.returncode:
        sys.exit('clang -c failed')
    r = sh([sys.executable, GEN, o_path, ko_path, 'fbpat', tmpl])
    if r.returncode:
        sys.exit('gen_module failed')
    print('[*] built', ko_path)


def load_and_verify(serial, hello):
    subprocess.run(['adb', '-s', serial, 'push', 'modload',
                    '/data/local/tmp/modload'], capture_output=True)
    adb(serial, "su -c 'chmod 755 /data/local/tmp/modload'")
    subprocess.run(['adb', '-s', serial, 'push',
                    os.path.join(WORK, 'fbpat.ko'), '/data/local/tmp/fbpat.ko'],
                   capture_output=True)
    if not hello:
        adb(serial, "su -c 'rmmod fbpat'")
    adb(serial, "su -c 'dmesg -c >/dev/null'")
    out = adb(serial,
              "su -c '/data/local/tmp/modload /data/local/tmp/fbpat.ko'")
    print('[*] load:', out.strip())
    adb(serial, "su -c 'sleep 1'")
    log = adb(serial, "su -c 'dmesg | grep fbpat'")
    print(log.strip())
    if 'done' in log or 'hello' in log:
        print('[+] SUCCESS')
    elif 'abort' in log or 'no cms psy' in log:
        print('[!] module loaded but patch aborted (safe no-op)')
    else:
        print('[!] load failed, see above')


if __name__ == '__main__':
    main()
