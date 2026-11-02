#!/system/bin/sh
# lightweight charging sampler for the X300 test session
C=/sys/class/cms_class
L=/data/local/tmp/charge-watch.log
P=/sys/class/power_supply
echo "# time online board batt btb cc_ma ibat vbat" > $L
i=0
while [ $i -lt 720 ]; do
  printf '%s %s %s %s %s %s %s %s\n' \
    "$(date +%H:%M:%S)" \
    "$(cat $P/usb/online 2>/dev/null)" \
    "$(cat $C/primary_board_temp 2>/dev/null)" \
    "$(cat $C/main_batt_temp 2>/dev/null)" \
    "$(cat $C/bat_btb_temp 2>/dev/null)" \
    "$(grep -o 'CONSTANT_CHARGE_CURRENT=[0-9]*' $P/primary_chg/uevent 2>/dev/null | cut -d= -f2)" \
    "$(cat $P/battery/current_now 2>/dev/null)" \
    "$(cat $P/battery/voltage_now 2>/dev/null)" >> $L
  i=$((i+1))
  sleep 5
done
