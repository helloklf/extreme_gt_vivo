from pathlib import Path
import subprocess
out=Path('charging-investigation')
cmds={
 'root-cms-list': 'ls -la /sys/class/cms_class',
 'root-baseline': '''date; for n in cms_enable switch_state scene_disable scene freq_test debug_chg_cur ai_charge_rate_mode disable_charge adapter_power charging_speed charging_technology bat_therm sub_batt_temp usb_conn_temp vir_shell_temp avg_current_now avg_voltage_now table_title table_item debug_info; do p=/sys/class/cms_class/$n; if [ -r "$p" ]; then echo "=== $n ==="; timeout 4 cat "$p"; fi; done''',
 'root-power-supplies': 'for n in battery usb mcu vfcs-fchg; do echo ===$n===; ls -la /sys/class/power_supply/$n/; cat /sys/class/power_supply/$n/uevent; done',
 'root-thermal-state': 'getprop | grep init.svc | grep -e thermal -e thermald; for z in /sys/class/thermal/thermal_zone*; do echo "$z $(cat $z/type) temp=$(cat $z/temp) emul=$(cat $z/emul_temp 2>/dev/null)"; done',
 'root-cms-dt-list': 'ls -laR /sys/firmware/devicetree/base/soc/pmic-glink/cms_v5',
 'root-charge-kernel-log': 'dmesg | grep -i -e vfcs -e cms -e fcc -e ichg -e ibat -e jeita -e chg_current | tail -450',
}
for name,cmd in cmds.items():
 try:
  p=subprocess.run(['adb','shell',"su -c '"+cmd+"'"],capture_output=True,timeout=65)
  s=(p.stdout+p.stderr).decode('utf-8',errors='replace').replace('\r','')
 except subprocess.TimeoutExpired as ex:
  s=((ex.stdout or b'')+(ex.stderr or b'')).decode('utf-8',errors='replace')+'\nHOST TIMEOUT\n'
 (out/'snapshots'/(name+'.txt')).write_text(s,encoding='utf-8')
 print('===',name,'===\n',s[:15000],flush=True)
for name in ('vfcs-cms-core','vfcs-core','vfcs-fchg-core','vfcs-misc-core','qti_battery_charger'):
 p=subprocess.run(['adb','exec-out',"su -c 'cat /vendor_dlkm/lib/modules/"+name+".ko'"],capture_output=True,timeout=30)
 if p.returncode==0 and p.stdout.startswith(b'\x7fELF'):
  (out/'modules'/(name+'.ko')).write_bytes(p.stdout)
  print(name,len(p.stdout),'ELF saved',flush=True)
 else: print(name,p.returncode,p.stderr,p.stdout[:100],flush=True)
