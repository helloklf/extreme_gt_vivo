#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"
versionName="$(sed -n 's/^version=//p' ExtremeGT_vivo/module.prop | tr -d '\r')"
case "$versionName" in
    ''|*/*|*\\*|*$'\n'*) echo "错误：module.prop 中的 version 无效" >&2; exit 1 ;;
esac
command -v zip >/dev/null || { echo "错误：找不到 zip" >&2; exit 1; }

bash ./build.sh tpoff.c
bash ./build.sh fanctld.c
bash ./build.sh fccc.c

find ./ExtremeGT_vivo -type f -name "*.sh" -exec dos2unix {} +
dos2unix ./ExtremeGT_vivo/META-INF/com/google/android/update-binary
dos2unix ./ExtremeGT_vivo/META-INF/com/google/android/updater-script

archive="$PWD/ExtremeGT_vivo_${versionName}.zip"
tmp_zip="$(mktemp "$PWD/.ExtremeGT_vivo.XXXXXXXX.zip")"
trap 'rm -f -- "$tmp_zip"' EXIT
rm -f -- "$tmp_zip"
(cd ExtremeGT_vivo && zip -rq "$tmp_zip" .)
mv -f -- "$tmp_zip" "$archive"
echo "已生成 $archive"