#!/usr/bin/env bash
# ============================================================================
# EKA2L1 Android “克隆版”构建脚本
#
# 与 build_android.sh 相同，但把包名加上 .clone 后缀（applicationId =
# com.github.eka2l1.clone），并把启动器名称改为 “EKA2L1 Clone”，因此可以与
# 正式版同时安装、互不覆盖数据，用于双开 / 对比测试。
#
# 用法:
#   ./build_android_clone.sh            # 默认 release
#   ./build_android_clone.sh release
#   ./build_android_clone.sh debug
#
# 输出:
#   build_android/eka2l1-clone-<abi>-release.apk           （release，已签名/未签名见下）
#   build_android/eka2l1-clone-<abi>-release-unsigned.apk
#   build_android/eka2l1-clone-<abi>-debug.apk             （debug，Gradle 自动用 debug key 签名）
#
# 环境变量（可选覆盖，与 build_android.sh 一致）:
#   ANDROID_SDK_ROOT / ANDROID_NDK_HOME / JOBS
#   KEYSTORE / KEY_ALIAS / KEY_PASS / STORE_PASS   —— 仅 release 用于签名
#   ABI               —— 覆盖默认 ABI（release 默认 arm64-v8a，debug 默认 x86_64）
#   APP_ID_SUFFIX     —— 覆盖包名后缀（默认 .clone）
#   APP_LABEL         —— 覆盖启动器名称（默认 "EKA2L1 Clone"）
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_PROJECT="$ROOT/src/emu/android"
OUT_DIR="$ROOT/build_android"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

# 克隆参数
APP_ID_SUFFIX="${APP_ID_SUFFIX:-.clone}"
APP_LABEL="${APP_LABEL:-EKA2L1 Clone}"

# 签名：默认用仓库根目录的 eka2l1.jks（与正式版同一证书），可用环境变量覆盖。
KEYSTORE="${KEYSTORE:-$ROOT/eka2l1.jks}"
KEY_ALIAS="${KEY_ALIAS:-eka2l1}"
STORE_PASS="${STORE_PASS:-1311817771}"
KEY_PASS="${KEY_PASS:-$STORE_PASS}"

# ── 解析 release / debug 参数（默认 release）─────────────────────────────────
BUILD_TYPE="${1:-release}"
BUILD_TYPE="$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"

case "$BUILD_TYPE" in
    release)
        GRADLE_TASK="assembleRelease"
        ABI="${ABI:-arm64-v8a}"
        ;;
    debug)
        GRADLE_TASK="assembleDebug"
        # build.gradle 的 debug buildType 声明 abiFilters 为 x86_64，
        # 注入 arm64-v8a 会与之求交集为空导致无 native 库，故默认 x86_64。
        ABI="${ABI:-x86_64}"
        ;;
    *)
        echo "用法: $0 [release|debug]   (默认 release)" >&2
        exit 1
        ;;
esac

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
        warn "NDK $REQUIRED_NDK_VER 未找到（期望路径: $NDK_PATH），Gradle 会尝试自动下载。"
    else
        info "NDK $REQUIRED_NDK_VER: $NDK_PATH"
    fi
fi

# ── 检查 JDK ────────────────────────────────────────────────────────────────
if ! command -v java >/dev/null 2>&1; then
    error "未找到 java。请安装 JDK 11+ 并将其加入 PATH，或设置 JAVA_HOME。"
fi
info "Java: $(java -version 2>&1 | head -1)"

# ── 检查 Git（用于 versionCode）─────────────────────────────────────────────
if ! command -v git >/dev/null 2>&1; then
    warn "未找到 git，versionCode 中的 GIT_HASH 将为空字符串。"
fi

# ── 进入 Android 项目目录 ────────────────────────────────────────────────────
cd "$ANDROID_PROJECT"
info "工作目录: $ANDROID_PROJECT"
chmod +x gradlew

# ── 组装 Gradle 参数 ─────────────────────────────────────────────────────────
GRADLE_ARGS=(
    "$GRADLE_TASK"
    "-PappIdSuffix=$APP_ID_SUFFIX"
    "-PappLabel=$APP_LABEL"
    "-Pandroid.injected.build.abi=$ABI"
    "--no-daemon"
    "-Dorg.gradle.workers.max=$JOBS"
)

info "构建克隆版：类型=$BUILD_TYPE  ABI=$ABI"
info "  包名      = com.github.eka2l1$APP_ID_SUFFIX"
info "  启动器名称 = $APP_LABEL"
info "命令: ./gradlew ${GRADLE_ARGS[*]}"
echo ""

./gradlew "${GRADLE_ARGS[@]}"

# ── 定位产物（限定到对应 buildType 目录）─────────────────────────────────────
APK_FILE=$(find "$ANDROID_PROJECT/app/build" -path "*/$BUILD_TYPE/*" -name "*.apk" | grep -v "test" | head -1)
if [ -z "$APK_FILE" ]; then
    APK_FILE=$(find "$ANDROID_PROJECT/app/build" -name "*.apk" | grep -v "test" | head -1)
fi
if [ -z "$APK_FILE" ]; then
    error "构建完成但未找到 APK（搜索目录: $ANDROID_PROJECT/app/build）"
fi

echo ""
info "构建成功！"
info "APK 路径: $APK_FILE"
info "文件大小: $(du -sh "$APK_FILE" | cut -f1)"

# ── debug：Gradle 已用 debug key 签名，直接复制输出 ──────────────────────────
if [ "$BUILD_TYPE" = "debug" ]; then
    FINAL_APK="$OUT_DIR/eka2l1-clone-$ABI-debug.apk"
    cp "$APK_FILE" "$FINAL_APK"
    echo ""
    info "全部完成（debug 已自动签名，可直接安装）。"
    info "  $(du -sh "$FINAL_APK" | cut -f1)  $FINAL_APK"
    exit 0
fi

# ── release：复制未签名副本，并按需签名 ──────────────────────────────────────
UNSIGNED_OUT="$OUT_DIR/eka2l1-clone-$ABI-release-unsigned.apk"
cp "$APK_FILE" "$UNSIGNED_OUT"
info "未签名副本 -> $UNSIGNED_OUT"

FINAL_APK="$OUT_DIR/eka2l1-clone-$ABI-release.apk"

APKSIGNER="$ANDROID_SDK_ROOT/build-tools/$(ls "$ANDROID_SDK_ROOT/build-tools" 2>/dev/null | sort -V | tail -1)/apksigner"

if [ ! -f "$APKSIGNER" ]; then
    warn "未找到 apksigner，输出未签名 APK（无法直接安装，会提示证书缺失）。"
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
    # 默认 keystore 不在（被删/未克隆全），退到 Android debug keystore 自动签名，
    # 至少保证克隆版能侧载安装（否则未签名会提示“证书缺失/无法解析软件包”）。
    warn "keystore 不存在: $KEYSTORE，改用 debug keystore 自动签名。"
    DEBUG_KS="$HOME/.android/debug.keystore"
    if [ ! -f "$DEBUG_KS" ]; then
        mkdir -p "$HOME/.android"
        keytool -genkeypair -v -keystore "$DEBUG_KS" \
            -storepass android -keypass android -alias androiddebugkey \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1 \
            || warn "生成 debug keystore 失败，将输出未签名 APK。"
    fi
    if [ -f "$DEBUG_KS" ]; then
        "$APKSIGNER" sign --ks "$DEBUG_KS" --ks-key-alias androiddebugkey \
            --ks-pass pass:android --key-pass pass:android \
            --out "$FINAL_APK" "$APK_FILE"
        info "签名完成（debug 证书）: $FINAL_APK"
    else
        cp "$APK_FILE" "$FINAL_APK"
    fi
fi

echo ""
info "全部完成。输出目录: $OUT_DIR"
info "  $(du -sh "$FINAL_APK" | cut -f1)  $FINAL_APK"
