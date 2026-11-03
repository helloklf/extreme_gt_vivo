#!/system/bin/sh
# 卸载时停掉常驻进程、把被接管的东西还回去（否则会一直跑到重启）
killall fanctld 2>/dev/null
killall fccc 2>/dev/null
killall tpoff 2>/dev/null
start thermal-engine 2>/dev/null
