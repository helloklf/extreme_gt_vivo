#!/system/bin/sh
# 充电阶梯追踪：每秒采样，专门用来抓“电流在哪几个 vbat 点掉档”
# 用法（设备上）：
#   sh /data/local/tmp/chg-trace.sh [/data/local/tmp/chg_trace.csv]
# 停止：Ctrl-C 或 kill
#
# 输出列：
#   ts,vbat_uv,ibat_ua,soc,tbat,tpri,chg_voltage,normal_vote,effective
# normal_vote 是 A 表（NORMAL_CHG_EVENT）的请求值，effective 是最终生效的限流值。
# 判读：
#   电流掉档时，effective 正好等于 11720/9720/7720/5505 之一 → 是 A 表在限流；
#   effective=15600 而电流停在某处 → 是 CP / 适配器 / 线材的上限。

OUT="${1:-/data/local/tmp/chg_trace.csv}"
PS=/sys/class/power_supply/battery
CMS=/sys/class/cms_class
VOT=/sys/kernel/debug/pmic-votable/USER_IBAT/status

[ -f "$OUT" ] || echo "ts,vbat_uv,ibat_ua,soc,tbat,tpri,chg_voltage,normal_vote,effective" > "$OUT"

# debugfs 可能没挂
mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>/dev/null

prev_i=0
while :; do
    ts=$(date +%s)
    vbat=$(cat $PS/voltage_now 2>/dev/null)
    ibat=$(cat $PS/current_now 2>/dev/null)
    soc=$(cat $PS/capacity 2>/dev/null)
    tbat=$(cat $CMS/main_batt_temp 2>/dev/null)
    tpri=$(cat $CMS/primary_board_temp 2>/dev/null)
    vchg=$(cat $CMS/chg_voltage 2>/dev/null)

    normal=""
    eff=""
    if [ -r "$VOT" ]; then
        normal=$(grep -m1 NORMAL_CHG_EVENT "$VOT" | sed -n 's/.*v=\([0-9]*\).*/\1/p')
        eff=$(grep -m1 effective "$VOT" | sed -n 's/.*v=\([0-9]*\).*/\1/p')
    fi

    echo "$ts,$vbat,$ibat,$soc,$tbat,$tpri,$vchg,$normal,$eff" >> "$OUT"

    # 掉档提示：电流绝对值下降超过 1 A（current_now 单位是 uA）
    i=${ibat#-}
    if [ -n "$i" ]; then
        drop=$((prev_i - i))
        if [ "$drop" -gt 1000000 ]; then
            echo "DROP vbat=${vbat}uV ibat=${ibat}uA normal_vote=${normal} effective=${eff}" >&2
        fi
        prev_i=$i
    fi

    sleep 1
done
