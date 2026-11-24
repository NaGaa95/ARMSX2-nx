#!/usr/bin/env bash
# Builds the Switch package: the three cores (armsx2_deko3d.nro, armsx2_vulkan.nro, armsx2_opengl.nro) and the
# launcher that embeds them, switch/launcher/armsx2-nx.nro, copied with its SHA-256 to the release folder. Run it from
# MSYS2's bash.
#
# Usage: switch/build-switch-all.sh
# Environment: BUILD_JOBS (all cores), ARMSX2_VERSION (1.0.0), RELEASE_DIR (build-switch-release), SKIP_CORES (1:
#              embed the cores already built in build-switch-deko3d, build-switch-vulkan and build-switch-opengl),
#              LAUNCHER_MESA20_PACKAGE (devkitPro's switch-mesa 20.1.0 package, by default the one pacman cached),
#              and ARMSX2_SWITCH_MESA_SDK_ZIP, UAM_ROOT and ARMSX2_LIBUSBHSFS_SOURCE (see switch/build-switch.sh).
set -euo pipefail

# MSYS2's tools first (devkitPro's CMake files refuse a CLANGARM64 cmake).
export PATH="/usr/bin:/bin:${PATH:-}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
export ARMSX2_VERSION="${ARMSX2_VERSION:-1.0.0}"
RELEASE_DIR="${RELEASE_DIR:-${ROOT}/build-switch-release}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac

if [[ "${SKIP_CORES:-0}" == 1 ]]; then
	echo "[1/3] [2/3] Using the cores already built (SKIP_CORES=1)"
else
	# (the Deko3D core also builds libusbhsfs, which the launcher links)
	echo "[1/3] Building the Deko3D core"
	SWITCH_BUILD_DIR=build-switch-deko3d bash "${ROOT}/switch/build-switch.sh" deko3d

	echo "[2/3] Building the Vulkan and OpenGL cores"
	SWITCH_BUILD_DIR=build-switch-vulkan bash "${ROOT}/switch/build-switch.sh" vulkan
	SWITCH_BUILD_DIR=build-switch-opengl bash "${ROOT}/switch/build-switch.sh" opengl
fi

echo "[3/3] Building the launcher"
LAUNCHER_TMP="${ROOT}/switch/launcher/build_switch/tmp"
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

# The launcher's EGL: devkitPro's stock switch-mesa 20.1.0, which the portlibs' SDL 2 is built against (the portlibs
# may hold another Mesa, such as the user's), staged from pacman's cache.
MESA20_STAGE="${ROOT}/switch/launcher/build_switch/mesa20"
MESA20_ROOT="${MESA20_STAGE}/opt/devkitpro/portlibs/switch"
MESA20_PACKAGE="${LAUNCHER_MESA20_PACKAGE:-}"
if [[ -z "${MESA20_PACKAGE}" ]]; then
	shopt -s nullglob
	mesa20_packages=(/var/cache/pacman/pkg/switch-mesa-20.1.0-*.pkg.tar.zst)
	shopt -u nullglob
	if ((${#mesa20_packages[@]})); then
		MESA20_PACKAGE="${mesa20_packages[${#mesa20_packages[@]}-1]}"
	fi
fi
if [[ -n "${MESA20_PACKAGE}" && -f "${MESA20_PACKAGE}" ]]; then
	rm -rf "${MESA20_STAGE}"
	mkdir -p "${MESA20_STAGE}"
	tar -xf "${MESA20_PACKAGE}" -C "${MESA20_STAGE}"
elif grep -q '^Version: 20\.1\.0' "${DEVKITPRO}/portlibs/switch/lib/pkgconfig/glesv2.pc" 2>/dev/null; then
	MESA20_ROOT="${DEVKITPRO}/portlibs/switch"
else
	echo "devkitPro's switch-mesa 20.1.0 is needed for the launcher. Cache it with: dkp-pacman -Sw switch-mesa" >&2
	exit 1
fi

# libusbhsfs from the Deko3D core's build (its source is ARMSX2_LIBUSBHSFS_SOURCE when that build used one)
USBHSFS_SOURCE="${ARMSX2_LIBUSBHSFS_SOURCE:-${ROOT}/build-switch-deko3d/_deps/libusbhsfs-src}"

make -C "${ROOT}/switch/launcher" -f Makefile.switch -j "${BUILD_JOBS}" RELEASE_VERSION="${ARMSX2_VERSION}" \
	LAUNCHER_MESA20_ROOT="${MESA20_ROOT}" \
	LIBUSBHSFS_INCLUDE="${USBHSFS_SOURCE}/include" \
	LIBUSBHSFS_LIB="${ROOT}/build-switch-deko3d/_deps/libusbhsfs-build/liblibusbhsfs.a" \
	CORE_DEKO3D="${ROOT}/build-switch-deko3d/switch/app/armsx2_deko3d.nro" \
	CORE_VULKAN="${ROOT}/build-switch-vulkan/switch/app/armsx2_vulkan.nro" \
	CORE_OPENGL="${ROOT}/build-switch-opengl/switch/app/armsx2_opengl.nro"

mkdir -p "${RELEASE_DIR}"
cp -f "${ROOT}/switch/launcher/armsx2-nx.nro" "${RELEASE_DIR}/armsx2-nx.nro"
(cd "${RELEASE_DIR}" && sha256sum armsx2-nx.nro > armsx2-nx.nro.sha256)

echo ">> Output: ${RELEASE_DIR}/armsx2-nx.nro (copy it to sdmc:/switch/)"
ls -la "${RELEASE_DIR}/armsx2-nx.nro"
