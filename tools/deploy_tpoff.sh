#!/system/bin/sh
# Deploy the unified tpoff to /data/local/tmp and (re)start it.
# Usage: deploy_tpoff.sh [offset]      (default -95, X300 sp table -> 7700mA)
OFF=${1:--95}
echo "== stop old =="
killall tpoff tpoff2 2>/dev/null
sleep 1

echo "== install =="
cp /data/local/tmp/tpoff.new /data/local/tmp/tpoff
chmod 755 /data/local/tmp/tpoff
/data/local/tmp/tpoff probe

echo "== start $OFF =="
rm -f /data/local/tmp/tpoff.log
nohup /data/local/tmp/tpoff "$OFF" >/dev/null 2>&1 &
sleep 3
ps -A | grep tpoff | sed 's/^/  /'
cat /data/local/tmp/tpoff.log 2>/dev/null
