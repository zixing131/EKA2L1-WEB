#!/usr/bin/env bash
# ============================================================================
# EKA2L1 Android armeabi-v7a（arm32 / 32 位老设备）构建脚本
#
# 用途:
#   面向 32 位 ARM 设备的构建——armeabi-v7a。对应没有 64 位 ABI 的老机器
#   （多为 Android 5.0~7.x 时代的低端 / 早期 arm32 设备）。
#
#   关于“安卓 2.3 (Gingerbread, API 9/10)”:
#   ----------------------------------------------------------------------
#   本仓库当前形态【无法】真正构建到 Android 2.3，原因是硬约束，不是脚本能绕过的:
#     1) 内核为 C++20，需要现代 Clang；能编 API 9 的远古 NDK(r10e) 编不了 C++20。
#     2) 现代 NDK r25(25.1.8937393) 原生支持的最低 API 就是 19。
#     3) UI 依赖 androidx.appcompat / material / camerax / mlkit 全部硬要求
#        API 21；降到 21 以下 manifest 合并直接失败，2.3 等于重写整个 Java UI。
#   因此本脚本的现实底线是 arm32(armeabi-v7a) + minSdk 21(Android 5.0)。
#   MIN_SDK 环境变量允许实验性下探，但低于 21 需要你自行降级/替换上述依赖，
#   并在 app/build.gradle 里改 minSdkVersion——本脚本只会传值并警告，不保证能编过。
#
# 输出: build_android_old/eka2l1-armeabi-v7a-release.apk（已签名）
#       build_android_old/eka2l1-armeabi-v7a-release-unsigned.apk（未签名副本）
#
# 依赖:
#   - Android SDK（包含 Gradle 插件所需组件）
#   - Android NDK 25.1.8937393（或通过 ANDROID_NDK_HOME 指定其他版本）
#   - JDK 11+（JAVA_HOME 指向对应目录）
#   - CMake 3.22.1+（NDK 内置版本或系统版本均可）
#   - Git（用于生成 versionCode 的 git hash）
#
# 环境变量（可选覆盖）:
#   ANDROID_SDK_ROOT  —— Android SDK 根目录（优先级高于 ANDROID_HOME）
#   ANDROID_NDK_HOME  —— 指定 NDK 目录，不设则使用 SDK 内 NDK
#   JOBS              —— 并行编译线程数（默认 nproc）
#   MIN_SDK           —— 实验性 minSdkVersion 覆盖（默认沿用 build.gradle 的 21）
#   KEYSTORE          —— keystore 路径；设置后自动签名
#   KEY_ALIAS         —— 签名 key alias（与 KEYSTORE 配合使用）
#   KEY_PASS          —— key 密码
#   STORE_PASS        —— keystore 密码
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_PROJECT="$ROOT/src/emu/android"
OUT_DIR="$ROOT/build_android_old"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

# 目标 ABI：32 位 ARM。
TARGET_ABI="armeabi-v7a"

# 签名：默认用仓库根目录的 eka2l1.jks，可用环境变量覆盖。
KEYSTORE="${KEYSTORE:-$ROOT/eka2l1.jks}"
KEY_ALIAS="${KEY_ALIAS:-eka2l1}"
STORE_PASS="${STORE_PASS:-1311817771}"
KEY_PASS="${KEY_PASS:-$STORE_PASS}"

mkdir -p "$OUT_DIR"

# ── 颜色输出辅助 ────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ── 检查 Android SDK ────────────────────────────────────────────────────────
if [ -z "${ANDROID_SDK_ROOT:-}" ] && [ -n "${ANDROID_HOME:-}" ]; then
    export ANDROID_SDK_ROOT="$ANDROID_HOME"
fi

if [ -z "${ANDROID_SDK_ROOT:-}" ]; then
    # 常见默认位置
    for candidate in \
        "$HOME/Library/Android/sdk" \
        "$HOME/Android/Sdk" \
        "/opt/android-sdk" \
        "/usr/local/lib/android/sdk"; do
        if [ -d "$candidate" ]; then
            export ANDROID_SDK_ROOT="$candidate"
            info "自动检测到 Android SDK: $ANDROID_SDK_ROOT"
            break
        fi
    done
fi

if [ -z "${ANDROID_SDK_ROOT:-}" ]; then
    error "未找到 Android SDK。请设置 ANDROID_SDK_ROOT 或 ANDROID_HOME 环境变量。\n  示例: export ANDROID_SDK_ROOT=\$HOME/Library/Android/sdk"
fi

info "Android SDK: $ANDROID_SDK_ROOT"

# ── 检查 NDK ────────────────────────────────────────────────────────────────
REQUIRED_NDK_VER="25.1.8937393"

if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    if [ ! -d "$ANDROID_NDK_HOME" ]; then
        error "ANDROID_NDK_HOME 指向的目录不存在: $ANDROID_NDK_HOME"
    fi
    info "使用自定义 NDK: $ANDROID_NDK_HOME"
else
    NDK_PATH="$ANDROID_SDK_ROOT/ndk/$REQUIRED_NDK_VER"
    if [ ! -d "$NDK_PATH" ]; then
        warn "NDK $REQUIRED_NDK_VER 未找到（期望路径: $NDK_PATH）"
        warn "build.gradle 中 ndkVersion='$REQUIRED_NDK_VER'；Gradle 会自动下载，或可手动安装："
        warn "  \$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager \"ndk;$REQUIRED_NDK_VER\""
    else
        info "NDK $REQUIRED_NDK_VER: $NDK_PATH"
    fi
fi

# ── 检查 JDK ────────────────────────────────────────────────────────────────
if ! command -v java >/dev/null 2>&1; then
    error "未找到 java。请安装 JDK 11+ 并将其加入 PATH，或设置 JAVA_HOME。"
fi

JAVA_VER=$(java -version 2>&1 | awk -F '"' '/version/ {print $2}' | cut -d. -f1)
if [ -n "$JAVA_VER" ] && [ "$JAVA_VER" -lt 11 ] 2>/dev/null; then
    warn "检测到 JDK $JAVA_VER，建议使用 JDK 11+。"
fi
info "Java: $(java -version 2>&1 | head -1)"

# ── 检查 Git（用于 versionCode）─────────────────────────────────────────────
if ! command -v git >/dev/null 2>&1; then
    warn "未找到 git，versionCode 中的 GIT_HASH 将为空字符串。"
fi

# ── 进入 Android 项目目录 ────────────────────────────────────────────────────
cd "$ANDROID_PROJECT"
info "工作目录: $ANDROID_PROJECT"

# ── 确保 gradlew 可执行 ──────────────────────────────────────────────────────
chmod +x gradlew

# 说明：构建走 `clean assembleRelease`（见下方 GRADLE_ARGS）。
# 必须 clean 的原因：若历史上曾用 android.injected.build.abi 构建过，AGP 会把
# “isTestOnly=true” 的判定连同已编译的二进制 manifest 缓存进中间产物，仅删
# 部分 intermediates 无法重置该判定，残留 testOnly 会导致 INSTALL_FAILED_TEST_ONLY。
# clean 只清 app/build，native 的 .cxx 目标在其外、得以保留，故只重链不重编。

# ── 组装 Gradle 参数 ─────────────────────────────────────────────────────────
# -PtargetAbi 让 build.gradle 把 ndk.abiFilters 限定为 armeabi-v7a，只编 32 位、
# 产出纯 32 位 APK。注意【不用】android.injected.build.abi —— 那会让 AGP 给 APK 打
# android:testOnly="true"，普通 adb install 直接报 INSTALL_FAILED_TEST_ONLY。
GRADLE_ARGS=(
    clean
    assembleRelease
    "-PtargetAbi=$TARGET_ABI"
    "--no-daemon"
    "-Dorg.gradle.workers.max=$JOBS"
)

# 实验性 minSdk 下探：仅在用户显式设置 MIN_SDK 时传递并提示风险。
# 注意：build.gradle 的 minSdkVersion 是硬编码 21，此处通过 -P 注入
# android.minSdk 只对支持读取该属性的工程生效；当前工程未读取该属性，
# 故此分支主要起“记录意图 + 警告”作用，真正下探需手改 build.gradle 与依赖。
if [ -n "${MIN_SDK:-}" ]; then
    warn "MIN_SDK=$MIN_SDK 已设置。提醒：本工程依赖 androidx/material/camerax/mlkit"
    warn "硬要求 API 21；低于 21 需自行修改 app/build.gradle 的 minSdkVersion 并"
    warn "替换/降级这些依赖，否则 manifest 合并会失败。Android 2.3(API 9) 不可达。"
    GRADLE_ARGS+=( "-PoldMinSdk=$MIN_SDK" )
fi

info "目标 ABI: ${TARGET_ABI}（32 位 ARM 老设备）"
info "开始构建 $TARGET_ABI Release APK (并行线程: $JOBS)..."
info "命令: ./gradlew ${GRADLE_ARGS[*]}"
echo ""

./gradlew "${GRADLE_ARGS[@]}"

# ── 定位产物 ─────────────────────────────────────────────────────────────────
# Gradle 可能将产物写入 outputs/apk/ 或 intermediates/apk/，两处均搜索
APK_FILE=$(find "$ANDROID_PROJECT/app/build" -name "*.apk" | grep -v "test" | head -1)

if [ -z "$APK_FILE" ]; then
    error "构建完成但未找到 APK（搜索目录: $ANDROID_PROJECT/app/build）"
fi

echo ""
info "构建成功！"
info "APK 路径: $APK_FILE"
info "文件大小: $(du -sh "$APK_FILE" | cut -f1)"

# ── 定位 build-tools（zipalign / apksigner 同目录）──────────────────────────
BUILD_TOOLS_DIR="$ANDROID_SDK_ROOT/build-tools/$(ls "$ANDROID_SDK_ROOT/build-tools" 2>/dev/null | sort -V | tail -1)"
APKSIGNER="$BUILD_TOOLS_DIR/apksigner"
ZIPALIGN="$BUILD_TOOLS_DIR/zipalign"

# ── 剥离非目标 ABI（mlkit 等 AAR 自带 arm64-v8a 预编译 .so）─────────────────
# android.injected.build.abi 只约束 externalNativeBuild，剥不掉第三方 AAR
# 已打包的其他 ABI .so。这里手动把 lib/ 下所有非 $TARGET_ABI 的目录删掉，
# 产出纯 32 位包。删 zip 条目后必须重新 zipalign，再签名（签名要求对齐）。
STRIPPED_APK="$OUT_DIR/.eka2l1-$TARGET_ABI-stripped.apk"
cp "$APK_FILE" "$STRIPPED_APK"

# 找出 APK 内 lib/ 下除目标 ABI 外的所有 ABI 目录并删除
OTHER_ABIS=$(unzip -l "$STRIPPED_APK" 2>/dev/null \
    | grep -oE 'lib/[^/]+/' | sort -u \
    | grep -v "lib/$TARGET_ABI/" || true)
if [ -n "$OTHER_ABIS" ]; then
    for abi_dir in $OTHER_ABIS; do
        info "剥离非目标 ABI: ${abi_dir}*"
        zip -d "$STRIPPED_APK" "${abi_dir}*" >/dev/null 2>&1 || true
    done
else
    info "无需剥离：APK 仅含 $TARGET_ABI"
fi

# 删除条目后重新对齐（4 字节；.so 用 -p 页对齐）
ALIGNED_APK="$OUT_DIR/.eka2l1-$TARGET_ABI-aligned.apk"
if [ -f "$ZIPALIGN" ]; then
    rm -f "$ALIGNED_APK"
    "$ZIPALIGN" -p -f 4 "$STRIPPED_APK" "$ALIGNED_APK"
    info "zipalign 完成"
else
    warn "未找到 zipalign，跳过对齐（签名可能告警）。"
    mv "$STRIPPED_APK" "$ALIGNED_APK"
fi
rm -f "$STRIPPED_APK"

# 后续打包/签名都基于剥离对齐后的包
APK_FILE="$ALIGNED_APK"

# ── 复制未签名 APK 到输出目录 ────────────────────────────────────────────────
UNSIGNED_OUT="$OUT_DIR/eka2l1-$TARGET_ABI-release-unsigned.apk"
cp "$APK_FILE" "$UNSIGNED_OUT"
info "未签名副本 -> $UNSIGNED_OUT"

# ── 签名并输出到 build_android_old/ ──────────────────────────────────────────
FINAL_APK="$OUT_DIR/eka2l1-$TARGET_ABI-release.apk"

if [ ! -f "$APKSIGNER" ]; then
    warn "未找到 apksigner，输出未签名 APK（安装前需手动签名）。"
    cp "$APK_FILE" "$FINAL_APK"
elif [ -f "$KEYSTORE" ]; then
    info "用 keystore 签名: $KEYSTORE (alias=$KEY_ALIAS) -> $FINAL_APK"
    "$APKSIGNER" sign \
        --ks "$KEYSTORE" \
        --ks-key-alias "$KEY_ALIAS" \
        --ks-pass "pass:$STORE_PASS" \
        --key-pass "pass:$KEY_PASS" \
        --out "$FINAL_APK" \
        "$APK_FILE"
    info "签名完成: $FINAL_APK"
else
    warn "keystore 不存在: $KEYSTORE，输出未签名 APK。安装前需手动签名。"
    cp "$APK_FILE" "$FINAL_APK"
fi

# 清理临时对齐包
rm -f "$ALIGNED_APK"

echo ""
info "全部完成。输出目录: $OUT_DIR"
info "  $(du -sh "$FINAL_APK" | cut -f1)  $FINAL_APK"
