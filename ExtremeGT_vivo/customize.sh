set_perm_recursive $MODPATH 0 0 0755 0644

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

# Help
if [[ "$(grep abogki543627742 /proc/version)" == "" ]]; then
  echo '其它提示：'
  echo '本模块未集成适用于当前设备的 fbpat.ko (亮屏充电提速补丁)'
  echo '需要自行参考开源项目'
  echo 'https://github.com/helloklf/extreme_gt_vivo'
  echo '实机构建该内核模块，并手动放置到模块目录下'
  echo ''
fi
