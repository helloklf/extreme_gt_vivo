# UEPP 公共逻辑（customize.sh 与 service.sh 共用）
# 统一覆盖/挂载替换都必须与设备层选中的文件同名（PEM 按 oem.model > product.model
# > platform > solution > default 的优先级选文件名）

# uepp_plugin_dir <插件ID> → stdout 打印设备上的配置目录（找不到则无输出）
uepp_plugin_dir() {
  local base
  for base in /system/etc/uepp/plugins /system_ext/etc/uepp/plugins /product/etc/uepp/plugins; do
    if [ -d "$base/$1" ]; then
      echo "$base/$1"
      return 0
    fi
  done
}

# uepp_pick_target <配置目录> → stdout 打印被选中的文件名（如 SM8850.xml）
uepp_pick_target() {
  local dir=$1 name target
  for name in $(getprop ro.vivo.oem.model) $(getprop ro.vivo.product.model) \
              $(getprop ro.vivo.product.platform) $(getprop ro.vivo.product.solution) default; do
    if [ -n "$name" ] && [ -f "$dir/$name.xml" ]; then
      echo "$name.xml"
      return 0
    fi
  done
  target=$(basename "$(ls "$dir"/*.xml 2>/dev/null | head -n 1)" 2>/dev/null)
  [ -n "$target" ] && echo "$target"
}

# customize.sh 用：把模块内带 overlay.xml 的插件按设备选中文件名复制好（挂载替换 device 层）
uepp_deploy_overlays() {
  local mod=$1 dir sys overlay target
  [ -d "$mod" ] || return
  for dir in $(ls $mod); do
    overlay=$mod/$dir/overlay.xml
    [ -f "$overlay" ] || continue
    sys=$(uepp_plugin_dir $dir)
    target=$(uepp_pick_target "$sys")
    [ -z "$target" ] && target=$(getprop ro.vivo.product.model).xml
    cp "$overlay" "$mod/$dir/$target"
  done
}

# service.sh 用：解除发热限任务——ThermalRestrictionPlugin 在热升档后禁止相册云同步等
# 后台 job；从 PEM unified 覆盖层（/data/bbkcore/pem/uepp/unified/，本地不验签）关闭
# forbid 规则，每次开机现场生成，只把 forbid 的 enable 置 0，保证与固件结构一致
uepp_disable_job_restrict() {
  local plugin=com.ueppp.restriction.job.ThermalRestrictionPlugin
  local sys=$(uepp_plugin_dir $plugin) target gen tmp p
  local unified=/data/bbkcore/pem/uepp/unified/$plugin
  [ -n "$sys" ] || return
  target=$(uepp_pick_target "$sys")
  [ -n "$target" ] && [ -f "$sys/$target" ] || return

  gen=$(sed -E '/__name__="forbid_/,/\/>/ s/enable[[:space:]]*=[[:space:]]*"1"/enable = "0"/g' "$sys/$target" | tr -d '\r')
  [ -n "$gen" ] || return
  tmp=$MODPATH/.job_restrict.tmp
  printf '%s\n' "$gen" > "$tmp"
  # 原始配置归一化后比较：无 forbid 可关或已部署且一致则不动，避免每次开机都重启 PEM
  printf '%s\n' "$(tr -d '\r' < "$sys/$target")" > "$tmp.orig"
  if ! cmp -s "$tmp" "$tmp.orig" && ! cmp -s "$tmp" "$unified/$target" 2>/dev/null; then
    mkdir -p "$unified"
    cp "$tmp" "$unified/$target"
    chmod 700 /data/bbkcore/pem/uepp/unified "$unified" 2>/dev/null
    chown -R system:system /data/bbkcore/pem/uepp/unified 2>/dev/null
    chmod 600 "$unified/$target" 2>/dev/null
    # 记录投放路径，uninstall.sh 据此精确回收
    echo "$unified/$target" > "$unified/.extreme_gt_vivo_deployed"
    chmod 600 "$unified/.extreme_gt_vivo_deployed" 2>/dev/null
    # PEM 是 persistent 应用，杀掉自动拉起并重载配置
    p=$(pidof com.vivo.pem 2>/dev/null)
    [ -n "$p" ] && kill -9 $p 2>/dev/null
  fi
  rm -f "$tmp" "$tmp.orig"
}
