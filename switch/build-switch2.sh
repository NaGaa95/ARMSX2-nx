#!/usr/bin/env bash
# Builds one Switch 2 (kaiHEN) core NRO: armsx2_drako3d.nro (the DK3D renderer on drako3d), armsx2_vulkan.nro (the
# Vulkan renderer on mesa-nx's NVK) or armsx2_opengl.nro (the OpenGL renderer on mesa-nx's nvc0, or on Zink over NVK).
# One core per GPU driver: drako3d cannot open the GPU after Mesa ran in the same program, and nvc0 and Vulkan share
# one GPU channel. Needs ounce-devkit (libounce, drako3d, uam2) in $DEVKITPRO; run it from MSYS2's bash.
#
# Usage: switch/build-switch2.sh drako3d|vulkan|opengl
# Environment: BUILD_JOBS (all cores), ARMSX2_VERSION (1.0.0), SWITCH2_BUILD_DIR (build-switch2-<renderer>),
#              ARMSX2_SWITCH2_MESA_SDK_ZIP (mesa-nx's Switch 2 SDK zip: NVK, EGL and OpenGL, and uam2rt for frame
#              generation).
set -euo pipefail

# MSYS2's cmake and ninja (devkitPro's CMake files refuse others, such as a CLANGARM64 shell's).
export PATH="/usr/bin:/bin:${PATH:-}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RENDERER="${1:-drako3d}"
case "${RENDERER}" in
	drako3d|vulkan|opengl) ;;
	*) echo "usage: $0 drako3d|vulkan|opengl" >&2; exit 2 ;;
esac
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
ARMSX2_VERSION="${ARMSX2_VERSION:-1.0.0}"
SWITCH2_BUILD_DIR="${SWITCH2_BUILD_DIR:-build-switch2-${RENDERER}}"
case "${SWITCH2_BUILD_DIR}" in
	/*) BUILD_PATH="${SWITCH2_BUILD_DIR}" ;;
	*) BUILD_PATH="${ROOT}/${SWITCH2_BUILD_DIR}" ;;
esac

if [[ ! -f "${DEVKITPRO}/libounce/cmake/Switch2.cmake" ]]; then
	echo "No Switch 2 devkit (libounce) in ${DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit" >&2
	exit 1
fi

mkdir -p "${BUILD_PATH}/tmp"
# (portlibs/switch2/bin first: its pkg-config is the Switch 2's)
export PATH="${DEVKITPRO}/portlibs/switch2/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"
export TMPDIR="${BUILD_PATH}/tmp"
# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
	SWITCH_TMP_WINDOWS="$(cygpath -w "${TMPDIR}")"
	export TEMP="${SWITCH_TMP_WINDOWS}" TMP="${SWITCH_TMP_WINDOWS}"
else
	export TEMP="${TMPDIR}" TMP="${TMPDIR}"
fi

cd "${ROOT}"
bash "${ROOT}/switch/deps/prepare_mesa_sdk_switch2.sh" \
	"${ARMSX2_SWITCH2_MESA_SDK_ZIP:-${ROOT}/../mesa-nx/dist/mesa-26.3.1-switch2-horizon-sdk.zip}" \
	"${ROOT}/switch/deps/switch2_mesa_sdk"

# (no precompiled headers: under MSYS2, CMake writes /c/... paths into them, which the native compiler cannot open)
cmake -S . -B "${BUILD_PATH}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/libounce/cmake/Switch2.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DARMSX2_SWITCH2_RENDERER="${RENDERER}" \
	-DARMSX2_VERSION="${ARMSX2_VERSION}" \
	-DENABLE_TESTS=OFF \
	-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON

cmake --build "${BUILD_PATH}" --parallel "${BUILD_JOBS}" --target yadokari_nro -- -k "${NINJA_KEEP_GOING:-1}"

echo ">> Output: ${BUILD_PATH}/switch/app/armsx2_${RENDERER}.nro"
