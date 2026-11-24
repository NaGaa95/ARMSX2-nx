#!/usr/bin/env bash
# Builds one Switch core NRO: armsx2_deko3d.nro (the DK3D renderer on deko3d), armsx2_vulkan.nro (the Vulkan renderer
# on the Switch Mesa SDK's NVK) or armsx2_opengl.nro (the OpenGL renderer on its nvc0, or on Zink over NVK).
# One core per GPU driver, as on the Switch 2: deko3d cannot open the GPU after Mesa ran in the same program, and nvc0
# and Vulkan share one GPU channel. Needs devkitPro's libnx, deko3d and switch-ntfs-3g, and Yadokari's uam
# (https://github.com/PalindromicBreadLoaf/uam, c0d76aa or later) next to this checkout; run it from MSYS2's bash.
#
# Usage: switch/build-switch.sh deko3d|vulkan|opengl
# Environment: BUILD_JOBS (all cores), ARMSX2_VERSION (1.0.0), SWITCH_BUILD_DIR (build-switch-<renderer>),
#              ARMSX2_SWITCH_MESA_SDK_ZIP (mesa-nx's unified Switch SDK zip: NVK, EGL and OpenGL), UAM_ROOT (../uam),
#              ARMSX2_LIBUSBHSFS_SOURCE (a checkout of the pinned libusbhsfs revision, instead of fetching it).
set -euo pipefail

# MSYS2's cmake and ninja (devkitPro's CMake files refuse others, such as a CLANGARM64 shell's).
export PATH="/usr/bin:/bin:${PATH:-}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RENDERER="${1:-deko3d}"
case "${RENDERER}" in
	deko3d|vulkan|opengl) ;;
	*) echo "usage: $0 deko3d|vulkan|opengl" >&2; exit 2 ;;
esac
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
ARMSX2_VERSION="${ARMSX2_VERSION:-1.0.0}"
SWITCH_BUILD_DIR="${SWITCH_BUILD_DIR:-build-switch-${RENDERER}}"
case "${SWITCH_BUILD_DIR}" in
	/*) BUILD_PATH="${SWITCH_BUILD_DIR}" ;;
	*) BUILD_PATH="${ROOT}/${SWITCH_BUILD_DIR}" ;;
esac

if [[ ! -f "${DEVKITPRO}/cmake/Switch.cmake" ]]; then
	echo "No devkitPro Switch toolchain in ${DEVKITPRO}: install switch-dev" >&2
	exit 1
fi

mkdir -p "${BUILD_PATH}/tmp"
export PATH="${DEVKITPRO}/portlibs/switch/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"
export TMPDIR="${BUILD_PATH}/tmp"
# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
	SWITCH_TMP_WINDOWS="$(cygpath -w "${TMPDIR}")"
	export TEMP="${SWITCH_TMP_WINDOWS}" TMP="${SWITCH_TMP_WINDOWS}"
else
	export TEMP="${TMPDIR}" TMP="${TMPDIR}"
fi

cd "${ROOT}"
if [[ "${RENDERER}" != deko3d ]]; then
	bash "${ROOT}/switch/deps/prepare_mesa_sdk_switch.sh" \
		"${ARMSX2_SWITCH_MESA_SDK_ZIP:-${ROOT}/../mesa-nx/dist/mesa-26.3.1-switch-unified-horizon-sdk.zip}" \
		"${ROOT}/switch/deps/switch_mesa_sdk"
fi

EXTRA_ARGS=()
if [[ -n "${ARMSX2_LIBUSBHSFS_SOURCE:-}" ]]; then
	EXTRA_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_LIBUSBHSFS=${ARMSX2_LIBUSBHSFS_SOURCE}")
fi

# (no precompiled headers: under MSYS2, CMake writes /c/... paths into them, which the native compiler cannot open)
cmake -S . -B "${BUILD_PATH}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DARMSX2_SWITCH_RENDERER="${RENDERER}" \
	-DARMSX2_VERSION="${ARMSX2_VERSION}" \
	-DUAM_ROOT="${UAM_ROOT:-${ROOT}/../uam}" \
	-DENABLE_TESTS=OFF \
	-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
	"${EXTRA_ARGS[@]}"

cmake --build "${BUILD_PATH}" --parallel "${BUILD_JOBS}" --target yadokari_nro -- -k "${NINJA_KEEP_GOING:-1}"

echo ">> Output: ${BUILD_PATH}/switch/app/armsx2_${RENDERER}.nro"
