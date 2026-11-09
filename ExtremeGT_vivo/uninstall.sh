#!/system/bin/sh
# 卸载时停掉常驻进程、把被接管的东西还回去（否则会一直跑到重启）
killall fanctld 2>/dev/null
killall fccc 2>/dev/null
killall tpoff 2>/dev/null
start thermal-engine 2>/dev/null

# 撤掉 PEM unified 覆盖层（解除发热限任务），只回收模块投放的文件
plugin=com.ueppp.restriction.job.ThermalRestrictionPlugin
marker=/data/bbkcore/pem/uepp/unified/$plugin/.extreme_gt_vivo_deployed
for f in $(cat "$marker" 2>/dev/null); do
  rm -f "$f"
done
rm -f "$marker"
p=$(pidof com.vivo.pem 2>/dev/null)
[ -n "$p" ] && kill -9 $p 2>/dev/null
