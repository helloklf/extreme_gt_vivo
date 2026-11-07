"""Read-only rooted ADB charging sampler. Never writes settings or device files."""
from pathlib import Path
import argparse
from datetime import datetime
import json
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parent
FIELDS = {
    'cms': ('/sys/class/cms_class', (
        'cms_enable', 'switch_state', 'ai_charge_rate_mode', 'debug_chg_cur',
        'freq_test', 'disable_charge', 'bat_therm', 'usb_conn_temp',
        'vir_shell_temp', 'charging_technology', 'charging_speed')),
    'battery': ('/sys/class/power_supply/battery', (
        'capacity', 'status', 'temp', 'current_now', 'voltage_now',
        'primary_board_temp', 'parallel_board_temp', 'board_temp_state',
        'charge_control_limit', 'charge_control_limit_max')),
    'usb': ('/sys/class/power_supply/usb', (
        'online', 'type', 'usb_type', 'voltage_now', 'current_now',
        'current_max', 'input_current_limit')),
}
VOTERS = ('USER_IBAT', 'USER_IBUS', 'USB_ICL', 'DCHG_FCC', 'DCHG_DISABLE')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', help='ADB serial; otherwise require exactly one device')
    parser.add_argument('--samples', type=int, default=1)
    parser.add_argument('--interval', type=float, default=10)
    args = parser.parse_args()
    if args.samples < 1 or args.interval < 0:
        parser.error('samples must be positive; interval must be nonnegative')
    serial = args.serial
    if not serial:
        p = subprocess.run(['adb', 'devices'], check=True, capture_output=True, text=True)
        devices = [line.split()[0] for line in p.stdout.splitlines()
                   if len(line.split()) == 2 and line.split()[1] == 'device']
        if len(devices) != 1:
            parser.error('exactly one connected device is required; use --serial')
        serial = devices[0]
    adb = ['adb', '-s', serial]

    def remote(command):
        p = subprocess.run(adb + ['shell', 'su -c ' + shlex.quote(command)],
                           capture_output=True, timeout=45)
        if p.returncode:
            raise RuntimeError(p.stderr.decode('utf-8', errors='replace'))
        return p.stdout.decode('utf-8', errors='replace').replace('\r', '')

    identity = remote('id; getprop ro.product.model; getprop ro.build.display.id; date')
    if 'uid=0(' not in identity:
        raise RuntimeError('Root access not available')
    mount_text = remote('cat /proc/mounts')
    mounts = [line.split()[1] for line in mount_text.splitlines()
              if len(line.split()) > 2 and line.split()[2] == 'debugfs']
    commands = ['echo ===device_time===; date']
    for group, (base, names) in FIELDS.items():
        for name in names:
            path = shlex.quote(base + '/' + name)
            commands.append(f'echo ==={group}/{name}===; if [ -r {path} ]; '
                            f'then timeout 3 cat {path}; else echo UNAVAILABLE; fi')
    for mount in mounts:
        for voter in VOTERS:
            path = shlex.quote(mount + '/pmic-votable/' + voter + '/status')
            commands.append(f'if [ -r {path} ]; then echo ===vote/{voter}===; '
                            f'timeout 3 cat {path}; fi')
    directory = ROOT / 'snapshots' / ('watch-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    directory.mkdir(parents=True, exist_ok=False)
    (directory / 'identity.txt').write_text(identity, encoding='utf-8')
    print('Read-only samples:', directory, flush=True)
    records = directory / 'samples.jsonl'
    for index in range(args.samples):
        started = time.monotonic()
        raw = remote('; '.join(commands))
        record = {'host_time': datetime.now().astimezone().isoformat(),
                  'serial': serial, 'fields': {}}
        key = None
        for line in raw.splitlines():
            if line.startswith('===') and line.endswith('==='):
                key = line[3:-3]
                record['fields'][key] = ''
            elif key is not None:
                record['fields'][key] += line + '\n'
        record['fields'] = {k: v.strip() for k, v in record['fields'].items()}
        with records.open('a', encoding='utf-8') as output:
            output.write(json.dumps(record, ensure_ascii=False) + '\n')
        (directory / f'{index + 1:04d}.txt').write_text(raw, encoding='utf-8')
        fields = record['fields']
        print(f"{index+1}/{args.samples} CMS={fields.get('cms/cms_enable')} "
              f"SOC={fields.get('battery/capacity')} "
              f"Tbat_raw={fields.get('battery/temp')} "
              f"Ibat_raw={fields.get('battery/current_now')}", flush=True)
        if index + 1 < args.samples:
            time.sleep(max(0, args.interval - (time.monotonic() - started)))


if __name__ == '__main__':
    main()
