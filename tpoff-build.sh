#!/bin/bash
# 编译 tpoff (aarch64 纯 syscall 静态程序)
# 用法: ./build-tpoff.sh
set -e
cd "$(dirname "$0")"

# 通用编译/精简参数: 去掉异常展开表、build-id、addr-sig 等多余节区
CFLAGS="-Oz -static -nostdlib -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-addrsig \
        -Wl,--build-id=none"
TARGET=aarch64-linux-android21

# 依次尝试可用的 clang:
#   1. NDK 自带的 clang ($ANDROID_NDK_HOME 或常见路径) —— 与原始构建一致
#   2. 系统 clang + NDK 的 ld.lld
NDK_DIRS=(
    "${ANDROID_NDK_HOME:-}"
    /home/hello/android-ndk/android-ndk-r27d
    /home/hello/android-ndk-r26d
)

find_ndk_clang() {
    local clang
    for ndk in "${NDK_DIRS[@]}"; do
        [ -d "$ndk" ] || continue
        clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang"
        [ -x "$clang" ] || continue
        # 真实试编译一次, 排除只能 --version 却无法干活的损坏工具链
        if echo 'int main(void){return 0;}' | "$clang" -x c - -o /dev/null 2>/dev/null; then
            echo "$clang"
            return 0
        fi
    done
    return 1
}

find_ndk_lld() {
    local lld
    for ndk in "${NDK_DIRS[@]}"; do
        [ -d "$ndk" ] || continue
        lld="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld"
        # 必须能真正执行 (排除架构不匹配的二进制)
        if [ -x "$lld" ] && "$lld" --version >/dev/null 2>&1; then
            echo "$lld"
            return 0
        fi
    done
    return 1
}

if NDK_CLANG=$(find_ndk_clang); then
    echo "[*] 使用 NDK clang: $NDK_CLANG"
    "$NDK_CLANG" $CFLAGS -o tpoff tpoff.c
    # NDK clang 编译后用 NDK 的 llvm-strip
    STRIP="$(dirname "$NDK_CLANG")/llvm-strip"
    [ -x "$STRIP" ] || STRIP=aarch64-linux-gnu-strip
else
    echo "[*] 使用系统 clang"
    command -v clang >/dev/null || { echo "错误: 找不到 clang"; exit 1; }
    LLD=$(find_ndk_lld)
    if [ -n "$LLD" ]; then
        clang --target=$TARGET $CFLAGS -fuse-ld="$LLD" -o tpoff tpoff.c
    else
        clang --target=$TARGET $CFLAGS -o tpoff tpoff.c
    fi
    STRIP=aarch64-linux-gnu-strip
fi

"$STRIP" -s tpoff
file tpoff
ls -la tpoff
echo "[*] 完成"
