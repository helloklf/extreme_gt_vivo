#!/system/bin/sh
C=/sys/class/cms_class
P=/sys/class/power_supply
D=/sys/kernel/debug
[ -d $D/pmic-votable ] || mount -t debugfs none $D 2>/dev/null
echo "== charger =="
printf 'usb_online=%s wls_online=%s\n' "$(cat $P/usb/online)" "$(cat $P/wls/online)"
for f in chg_type supported_adapter fastchg_is_ok adapter_power adapter_power_max chg_voltage charging_speed; do
  printf '%-20s %s\n' "$f" "$(cat $C/$f 2>/dev/null | tr '\n' ' ')"
done
grep -E 'CONSTANT_CHARGE_CURRENT=|INPUT_CURRENT_LIMIT=|INPUT_VOLTAGE_LIMIT=|USB_TYPE=' $P/primary_chg/uevent
echo "== battery =="
grep -E 'STATUS|CAPACITY|CURRENT_NOW|VOLTAGE_NOW|CHARGE_TYPE' $P/battery/uevent
echo "== temps =="
printf 'board=%s main=%s btb=%s chgic=%s sub=%s fixed=%s\n' \
  "$(cat $C/primary_board_temp)" "$(cat $C/main_batt_temp)" "$(cat $C/bat_btb_temp)" \
  "$(cat $C/chg_ic_temp)" "$(cat $C/charge_sub_temp)" "$(cat $C/fixed_temp)"
echo "== cms decision =="
dmesg | grep -E 'select_intell_param_type|get_intell_chg_current:|get_current_by_temp: primary|main_work:' | tail -4
echo "== votable =="
for v in USER_IBAT USER_IBUS FCC; do printf '%-8s %s\n' "$v" "$(tail -1 $D/pmic-votable/$v/status 2>/dev/null | sed 's/.*effective=//')"; done
echo "== daemons =="
ps -A | grep -E ' tpoff| cmsboost' | sed 's/^/  /'
tail -2 /data/local/tmp/tpoff.log 2>/dev/null
