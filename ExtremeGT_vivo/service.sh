# Magisk 开机脚本所在模块的根目录
MODPATH=${0%/*}

# 防止开机失败
sleep 60

# 关闭温控限频（联发科）
stop thermal_core
stop thermald
# stop vendor.thermal-mediatek # 开启后会导致原神无法正常运行

# 关闭温控限频（高通）
rmmod qti_cpufreq_cdev
if [[ "$(getprop ro.boot.hardware)" == 'qcom' ]]; then
  for c in /sys/class/thermal/cooling_device*; do
    type=$(cat "$c/type" 2>/dev/null)
    case "$type" in
    gpu|pause-cpu*|cpu-hotplug*|cpufreq-cpu*|ddr-cdev)
      echo 0 > "$c/cur_state"
      chmod 444 "$c/cur_state"
    ;;
    esac
  done
fi
# stop thermal-engine # 可能产生不良影响

# 关闭系统性能调度
#   此部分已废弃，性能控制已交由 Scene 处理
# echo 0 > /proc/powerhal_cpu_ctrl/adpf_enable
# echo 0 > /sys/rsc/svp/svp_enable
# echo 0 > /sys/rsc/svp/enabled
# stop vivo-vperf-hal-1-0
# stop vendor.vivoperfservice
# stop touch_boost

# 动态温控配置（ThermalConfig.MD）
if [[ -e /proc/pem/thml_key_type ]]; then
  for dir in /sys/devices/system/cpu/cpufreq/policy*
  do
    echo 0 > $dir/scaling_min_freq
  done
  echo 0 > /proc/pem/thml_key_type # Antutu(100), Monster(9)
  chmod 444 /proc/pem/thml_key_type
  echo "[policy_id = 0]\n" > /dev/thermal/thermal_data_summary.conf
fi

# vivo温控 /vendor/etc/thermal/thermal_config.xml 
# /proc/thermal_lifespan 不知道写点啥好，好像也没啥影响

# 模拟温度
for tz in /sys/class/thermal/thermal_zone*; do
  if [[ -f $tz/type ]]; then
    case $(cat $tz/type) in
    *"shell"*|*"skin"*|"vir"*|*"wifi"*|*"ntc"*)
      echo 32000 > $tz/emul_temp
    ;;
    esac
  fi
done

# 充电加速
# echo 0 > /sys/class/cms_class/cms_enable # 可能导致过热并损坏设备，不动这个
killall tpoff 2>/dev/null
killall fccc 2>/dev/null
killall fccc_turbo 2>/dev/null
chmod 755 "$MODPATH/tpoff"
chmod 755 "$MODPATH/fccc"
tpoff_mode=both
fixed_temp_scale=1
temp_offset=-80
fccc=0
fccc_ht=0
[ -f "$MODPATH/tpoff.conf" ] && . "$MODPATH/tpoff.conf"
if [[ "$fccc" == '1' ]]; then
  # tpoff_mode=both 时 fixed_temp 不影响电池温度，fccc 跳过 fixed_temp 检查
  htccc_enable=/sys/class/cms_class/htccc_enable
  if [[ -e $htccc_enable ]]; then
    if [[ "$fccc_ht" == '1' ]]; then
      echo 0 > $htccc_enable
    else
      echo 1 > $htccc_enable
    fi
  fi
  nohup "$MODPATH/fccc" "$tpoff_mode" >> "$MODPATH/fccc.log" 2>&1 &
fi
nohup "$MODPATH/tpoff" "$temp_offset" "$tpoff_mode" "$fixed_temp_scale" >/dev/null 2>&1 &

# 风扇接管：只有带风扇的机型（如 iQOO 15 Ultra）才有 /dev/fanpwm
# 温度源用 ddr(SoC 顶盖) + 电池，两条曲线取最大，配置在同目录 fan.conf
if [[ -e /dev/fanpwm ]]; then
  # thermal-engine 也会写这个节点，不停掉会互相抢风扇
  stop thermal-engine 2>/dev/null
  killall fanctld 2>/dev/null
  chmod 755 "$MODPATH/fanctld"
  nohup "$MODPATH/fanctld" >/dev/null 2>&1 &
fi

if [[ -e "$MODPATH/fbpat.ko" ]]; then
  insmod "$MODPATH/fbpat.ko"
else
  cms_ko=/vendor_dlkm/lib/modules/vfcs-cms-core.ko
  cms_vermagic='vermagic=6.12.58-android16-6-gfd60568f1f3a-4k'
  if grep -aq "$cms_vermagic" "$cms_ko" 2>/dev/null && \
     grep -q '^vfcs_cms_core ' /proc/modules 2>/dev/null; then
    # 基线: vfcs-cms-core.ko @ 6.12.58-android16-6-gfd60568f1f3a-4k
    insmod "$MODPATH/fbpat_6.12.58_gfd60568f1f3a.ko"
  fi
fi
