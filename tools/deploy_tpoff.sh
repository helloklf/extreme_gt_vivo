#!/system/bin/sh
# Deploy the unified tpoff to /data/local/tmp and (re)start it.
# Usage: deploy_tpoff.sh [offset] [mode] [fixed_temp_scale]
# Defaults target X300 (board-only, fixed_temp scale 10).
OFF=${1:--95}
MODE=${2:-board}
SCALE=${3:-10}
echo "== stop old =="
killall tpoff tpoff2 2>/dev/null
sleep 1

echo "== install =="
cp /data/local/tmp/tpoff.new /data/local/tmp/tpoff
chmod 755 /data/local/tmp/tpoff
echo "== start $OFF $MODE $SCALE =="
rm -f /data/local/tmp/tpoff.log
nohup /data/local/tmp/tpoff "$OFF" "$MODE" "$SCALE" >/dev/null 2>&1 &
sleep 3
ps -A | grep tpoff | sed 's/^/  /'
cat /data/local/tmp/tpoff.log 2>/dev/null
