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
oem_model=`getprop ro.vivo.oem.model`
product_model=`getprop ro.vivo.product.model`
platform=`getprop ro.vivo.product.platform`
solution=`getprop ro.vivo.product.solution`

uepp_sys=/system/etc/uepp/plugins
uepp_mod=$MODPATH/system/etc/uepp/plugins
for dir in `ls $uepp_mod`
do
  plugin=$uepp_mod/$dir
  overlay=$plugin/overlay.xml
  target=

  [[ -f $overlay ]] || continue

  sys_plugin=$uepp_sys/$dir
  if [[ -d $sys_plugin ]]; then
    for name in $oem_model $product_model $platform $solution default
    do
      if [[ -n $name && -f $sys_plugin/$name.xml ]]; then
        target=$name.xml
        break
      fi
    done

    if [[ -z $target ]]; then
      first_xml=`ls $sys_plugin/*.xml 2>/dev/null | head -n 1`
      if [[ -n $first_xml ]]; then
        target=`basename $first_xml`
      fi
    fi
  fi

  if [[ -z $target ]]; then
    target=$product_model.xml
  fi

  cp $overlay $plugin/$target
done

# TPOFF 探测 fixed_temp 的写入单位
killall tpoff 2>/dev/null
echo '检测温度单位，此步骤很重要'
echo '识别错误可能导致异常重启，请耐心等待'
fixed_temp=/sys/class/cms_class/fixed_temp
main_batt_temp=/sys/class/cms_class/main_batt_temp
fixed_temp_scale=1
tpoff_mode=board
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
  echo 0 > $fixed_temp
  if [[ "$real_effect" == "${test_temp}0" ]]; then
    fixed_temp_scale=10
  fi
  expected_temp=$((test_temp * fixed_temp_scale))
  if [[ "$after_temp" == "$expected_temp" ]]; then
    tpoff_mode=board
  else
    tpoff_mode=both
  fi
fi
cat > "$MODPATH/tpoff.conf" <<EOF
# 温度单位， 1 或 10 ，如果充电死机可以尝试切换
fixed_temp_scale=$fixed_temp_scale
# 自动配置好的，不要改！
tpoff_mode=$tpoff_mode
# 温度偏移量，-80相当于温控阈值调高8°C
temp_offset=-80
# 使用独立的充电控制程序
fccc=0
EOF

old_fbpat=/data/adb/modules/extreme_gt_vivo/fbpat.ko
if [[ -f $old_fbpat ]]; then
  cp $old_fbpat $MODPATH/fbpat.ko
fi

# fbpat 兼容性锚点：设备上的 cms 模块须与构建基线同一 build，
# 且当前已加载（证明内核 ABI 与该基线兼容）。
# 注意不要用 /proc/version 判断：vivo 内核不校验 vermagic，
# 内核 banner 与 vendor_dlkm 模块基线可能不同（混刷）。
cms_ko=/vendor_dlkm/lib/modules/vfcs-cms-core.ko
cms_vermagic='vermagic=6.12.58-android16-6-gfd60568f1f3a-4k'
fbpat_applicable=0
if grep -aq "$cms_vermagic" "$cms_ko" 2>/dev/null && \
   grep -q '^vfcs_cms_core ' /proc/modules 2>/dev/null; then
  fbpat_applicable=1
else
  rm -f $MODPATH/fbpat_6.12.58_gfd60568f1f3a.ko
fi

# Help
if [[ $fbpat_applicable == 0 && ! -f $MODPATH/fbpat.ko ]]; then
  echo '其它提示：'
  echo '现在你有以下方式可以进一步提升充电效率'
  echo '1.'
  echo '参考开源项目 构建 fbpat.ko (亮屏充电提速补丁)'
  echo 'https://github.com/helloklf/extreme_gt_vivo'
  echo '并手动放置到模块目录下'
  echo ''
  if [ -f "/sys/class/vfcs_class/fchg_fcc_ma" ]; then
    echo '2.'
    echo '使用 fccc 程序取代系统默认的充电速度控制'
    echo "修改 $MODPATH/tpoff.conf 配置 fccc=1 即可"
  fi
fi
