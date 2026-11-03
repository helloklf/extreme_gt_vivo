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
fixed_temp_scale=$fixed_temp_scale
tpoff_mode=$tpoff_mode
temp_offset=-80
fccc=0
EOF

old_fbpat=/data/adb/modules/extreme_gt_vivo/fbpat.ko
if [[ -f $old_fbpat ]]; then
  cp $old_fbpat $MODPATH/fbpat.ko
fi

# Help
if [[ "$(grep abogki543627742 /proc/version)" == "" && ! -f $MODPATH/fbpat.ko ]]; then
  echo '其它提示：'
  echo '本模块未集成适用于当前设备的 fbpat.ko (亮屏充电提速补丁)'
  echo '需要自行参考开源项目'
  echo 'https://github.com/helloklf/extreme_gt_vivo'
  echo '实机构建该内核模块，并手动放置到模块目录下'
  echo ''
fi
