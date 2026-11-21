#!/usr/bin/env bash
# Builds the Switch 2 (kaiHEN) package: both cores (armsx2_drako3d.nro, armsx2_vulkan.nro) and the launcher that embeds
# them, switch/launcher/armsx2-nx2.nro, copied with its SHA-256 to the release folder. Run it from MSYS2's bash.
#
# Usage: switch/build-switch2-all.sh
# Environment: BUILD_JOBS (all cores), ARMSX2_VERSION (1.0.0), RELEASE_DIR (build-switch2-release), SKIP_CORES (1:
#              embed the cores already built in build-switch2-drako3d and build-switch2-vulkan), and
#              ARMSX2_SWITCH2_MESA_SDK_ZIP (see switch/build-switch2.sh).
set -euo pipefail

# MSYS2's tools first (devkitPro's CMake files refuse a CLANGARM64 cmake).
export PATH="/usr/bin:/bin:${PATH:-}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
export ARMSX2_VERSION="${ARMSX2_VERSION:-1.0.0}"
RELEASE_DIR="${RELEASE_DIR:-${ROOT}/build-switch2-release}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac

if [[ "${SKIP_CORES:-0}" == 1 ]]; then
	echo "[1/3] [2/3] Using the cores already built (SKIP_CORES=1)"
else
	echo "[1/3] Building the Drako3D core"
	SWITCH2_BUILD_DIR=build-switch2-drako3d bash "${ROOT}/switch/build-switch2.sh" drako3d

	# (this one also prepares switch/deps/switch2_mesa_sdk, whose EGL the launcher links)
	echo "[2/3] Building the Vulkan core"
	SWITCH2_BUILD_DIR=build-switch2-vulkan bash "${ROOT}/switch/build-switch2.sh" vulkan
fi

echo "[3/3] Building the launcher"
LAUNCHER_TMP="${ROOT}/switch/launcher/build/tmp"
mkdir -p "${LAUNCHER_TMP}"
export PATH="${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"
export TMPDIR="${LAUNCHER_TMP}"
# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
	LAUNCHER_TMP_WINDOWS="$(cygpath -w "${LAUNCHER_TMP}")"
	export TEMP="${LAUNCHER_TMP_WINDOWS}" TMP="${LAUNCHER_TMP_WINDOWS}"
else
	export TEMP="${LAUNCHER_TMP}" TMP="${LAUNCHER_TMP}"
fi
make -C "${ROOT}/switch/launcher" -j "${BUILD_JOBS}" RELEASE_VERSION="${ARMSX2_VERSION}" \
	CORE_DRAKO3D="${ROOT}/build-switch2-drako3d/switch/app/armsx2_drako3d.nro" \
	CORE_VULKAN="${ROOT}/build-switch2-vulkan/switch/app/armsx2_vulkan.nro"

mkdir -p "${RELEASE_DIR}"
cp -f "${ROOT}/switch/launcher/armsx2-nx2.nro" "${RELEASE_DIR}/armsx2-nx2.nro"
(cd "${RELEASE_DIR}" && sha256sum armsx2-nx2.nro > armsx2-nx2.nro.sha256)

echo ">> Output: ${RELEASE_DIR}/armsx2-nx2.nro (copy it to sdmc:/switch/)"
ls -la "${RELEASE_DIR}/armsx2-nx2.nro"
