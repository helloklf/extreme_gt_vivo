set_perm_recursive $MODPATH 0 0 0755 0644
chmod 755 $MODPATH/fanctld 2>/dev/null
chmod 755 $MODPATH/fccc 2>/dev/null
chmod 644 $MODPATH/fan.conf 2>/dev/null

# Mediatek thermal config
mtk_t=/vendor/etc/thermal
if [[ -d $mtk_t ]]; then
  mkdir -p $MODPATH/system$mtk_t
  for file in `ls $mtk_t`
  do
    case $file in
      "disable_"*|"fix_ttj_95.conf")
        cp $mtk_t/$file $MODPATH/system$mtk_t/
      ;;
      "fix_ttj_85.conf")
        cp $mtk_t/fix_ttj_95.conf $MODPATH/system$mtk_t/
      ;;
      *)
        echo $file
        if [[ -f $mtk_t/disable_skin_control.conf ]]; then
         cp $mtk_t/disable_skin_control.conf $MODPATH/system$mtk_t/$file
        else
         cp $mtk_t/fix_ttj_95.conf $MODPATH/system$mtk_t/$file
        fi
      ;;
    esac
  done
fi

# UEPP
. $MODPATH/uepp.sh
uepp_deploy_overlays $MODPATH/system/etc/uepp/plugins

# TPOFF 探测 fixed_temp 的写入单位
killall tpoff 2>/dev/null
fixed_temp=/sys/class/cms_class/fixed_temp
main_batt_temp=/sys/class/cms_class/main_batt_temp
bat_btb_temp=/sys/class/cms_class/bat_btb_temp
fixed_temp_scale=1
tpoff_mode=board
tpoff_enable=1

# tpoff 需要电池温度来源（main_batt_temp，缺失时退而用 bat_btb_temp），
# 两者都不存在则此机型不支持温度伪装，相关配置不写入 tpoff.conf
if [[ ! -f $main_batt_temp && ! -f $bat_btb_temp ]]; then
  tpoff_enable=0
  echo '此机型缺少电池温度节点（main_batt_temp/bat_btb_temp），'
  echo 'tpoff 温度伪装不可用，已跳过相关配置'
fi

if [[ $tpoff_enable == 1 ]]; then
  echo '检测温度单位，此步骤很重要'
  echo '识别错误可能导致异常重启，请耐心等待'
  if [[ -f $fixed_temp ]]; then
    test_temp=10
    echo $test_temp > $fixed_temp
    sleep 15
    real_effect="$(dumpsys battery | grep 'temperature:' | cut -d ':' -f 2 | tr -d ' ')"
    if [[ ! "$real_effect" = "$test_temp"* ]]; then
      sleep 10
      real_effect="$(dumpsys battery | grep 'temperature:' | cut -d ':' -f 2 | tr -d ' ')"
    fi
    after_temp="$(cat $main_batt_temp 2>/dev/null)"
    if [[ -z "$after_temp" ]]; then
      # 某些机型/节点状态下 main_batt_temp 读取偶发失败，再试一次
      sleep 2
      after_temp="$(cat $main_batt_temp 2>/dev/null)"
    fi
    echo 0 > $fixed_temp
    if [[ "$real_effect" == "${test_temp}0" ]]; then
      fixed_temp_scale=10
    fi
    expected_temp=$((test_temp * fixed_temp_scale))
    if [[ -z "$after_temp" ]]; then # 读不到(老机子)，不伪装
      tpoff_mode=board
    elif [[ "$after_temp" == "$expected_temp" ]]; then
      tpoff_mode=board
    else
      tpoff_mode=both
    fi
  fi
fi

# 更新安装时，从旧配置继承用户已设置的值
old_conf=/data/adb/modules/extreme_gt_vivo/tpoff.conf
get_old_val() {
  grep -m1 "^$1=" "$old_conf" 2>/dev/null | cut -d= -f2 | tr -d ' \r'
}
temp_offset=-80
fccc=0
fccc_ht=0
if [[ -f $old_conf ]]; then
  v=$(get_old_val temp_offset); [[ -n $v ]] && temp_offset=$v
  v=$(get_old_val fccc_ht);     [[ -n $v ]] && fccc_ht=$v
  v=$(get_old_val fccc);        [[ -n $v ]] && fccc=$v
  echo "已继承旧配置: fccc=$fccc fccc_ht=$fccc_ht temp_offset=$temp_offset"
fi

cat > "$MODPATH/tpoff.conf" <<EOF
# 温度伪装总开关（安装期探测，此机型无电池温度节点时为 0，service.sh 不启动 tpoff）
tpoff_enable=$tpoff_enable
EOF

if [[ $tpoff_enable == 1 ]]; then
cat >> "$MODPATH/tpoff.conf" <<EOF

# 温度单位， 1 或 10 ，如果充电死机可以尝试切换
fixed_temp_scale=$fixed_temp_scale
# 温度偏移量，-80相当于温控阈值调高8°C
temp_offset=$temp_offset
EOF
fi

cat >> "$MODPATH/tpoff.conf" <<EOF

# 自动配置好的，不要改！（tpoff 和 fccc 都使用；tpoff_enable=0 时未探测，取保守默认 board）
tpoff_mode=$tpoff_mode
# 使用独立的充电控制程序
fccc=$fccc
# 提高fccc动态温控阈值
fccc_ht=$fccc_ht
EOF

old_fbpat=/data/adb/modules/extreme_gt_vivo/fbpat.ko
if [[ -f $old_fbpat ]]; then
  cp $old_fbpat $MODPATH/fbpat.ko
fi
cms_ko=/vendor_dlkm/lib/modules/vfcs-cms-core.ko
cms_vermagic='vermagic=6.12.58-android16-6-gfd60568f1f3a-4k'
grep -aq "$cms_vermagic" "$cms_ko" 2>/dev/null && \
grep -q '^vfcs_cms_core ' /proc/modules 2>/dev/null || \
rm -f $MODPATH/fbpat_6.12.58_gfd60568f1f3a.ko

# Help
echo ''
echo '其它提示：'
echo '有以下两步可以进一步提升充电效率'

echo "修改 $MODPATH/tpoff.conf"
echo "设置 fccc=1  使用fccc控制充电，去除官方限速阶梯"
echo "设置 fccc_ht=1  将快充限制温度从45°C提高到48°C"
echo "但是，启用fccc可能导致充电尾段，电量百分比失准"
