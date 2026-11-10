#!/system/bin/sh
# 读取 vfcs / vfcs_cms 节点里和输入电流/快充电流相关的 DT 属性
D=/sys/firmware/devicetree/base/soc/vfcs
C=/sys/firmware/devicetree/base/soc/vfcs_cms
for p in \
  fchg-curr-max-ma fchg-ibus-max-ma ufcs-fchg-curr-max-ma pps-fchg-curr-max-ma \
  wls-fchg-curr-max-ma wls-fchg-ibus-max-ma fchg-ic-switch-ibus-thr-ma \
  fchg-mode-bypass-to-div2-curr-thr-ma fchg-mode-switch-curr-thr-ma \
  fchg-vbat-scale-factor fchg-vbat-scale-factor-max fchg-vbus-max-mv \
  chg-protocol-support pps-fchg-support ffc-support fchg-ic-num \
  exit-fchg-ibat-thr-ma cv-ibus-drop-step-ma pps-power-derating-ma \
  ibus-cur-max-ma qc-ibus-limit-ma primary-cur-max-ma parallel-cur-max-ma
do
  f=$D/vivo,$p
  [ -f "$f" ] || f=$C/vivo,$p
  if [ -f "$f" ]; then
    printf '%-42s ' "$p"
    xxd -p "$f" | tr -d '\n'
    echo
  fi
done
