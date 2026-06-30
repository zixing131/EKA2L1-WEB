#!/usr/bin/env bash
# ============================================================================
# EKA2L1 Android arm64-v8a 构建脚本
#
# 输出: build_android/eka2l1-arm64-v8a-release.apk（已签名）
#       build_android/eka2l1-arm64-v8a-release-unsigned.apk（未签名副本）
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
#   KEYSTORE          —— keystore 路径；设置后自动签名
#   KEY_ALIAS         —— 签名 key alias（与 KEYSTORE 配合使用）
#   KEY_PASS          —— key 密码
#   STORE_PASS        —— keystore 密码
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_PROJECT="$ROOT/src/emu/android"
OUT_DIR="$ROOT/build_android"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

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

# ── 组装 Gradle 参数 ─────────────────────────────────────────────────────────
# android.injected.build.abi 强制 Gradle 只针对 arm64-v8a 调用 CMake/NDK，
# 跳过 armeabi-v7a，大幅减少构建时间和产物体积。
GRADLE_ARGS=(
    assembleRelease
    "-Pandroid.injected.build.abi=arm64-v8a"
    "--no-daemon"
    "-Dorg.gradle.workers.max=$JOBS"
)

info "开始构建 arm64-v8a Release APK (并行线程: $JOBS)..."
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

# ── 复制未签名 APK 到输出目录 ────────────────────────────────────────────────
UNSIGNED_OUT="$OUT_DIR/eka2l1-arm64-v8a-release-unsigned.apk"
cp "$APK_FILE" "$UNSIGNED_OUT"
info "未签名副本 -> $UNSIGNED_OUT"

# ── 签名并输出到 build_android/ ──────────────────────────────────────────────
FINAL_APK="$OUT_DIR/eka2l1-arm64-v8a-release.apk"

APKSIGNER="$ANDROID_SDK_ROOT/build-tools/$(ls "$ANDROID_SDK_ROOT/build-tools" 2>/dev/null | sort -V | tail -1)/apksigner"

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

echo ""
info "全部完成。输出目录: $OUT_DIR"
info "  $(du -sh "$FINAL_APK" | cut -f1)  $FINAL_APK"
