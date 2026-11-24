#!/usr/bin/env bash
# Stage the Nintendo Switch Mesa SDK (mesa-nx's mesa-*-switch-unified-horizon-sdk.zip) for the Switch cores: NVK for
# the Vulkan core, EGL with nvc0 or Zink for the OpenGL core.
#
# The unified SDK has real archives: libEGL.a (the GL drivers), libGL.a, libglapi.a and libvulkan.a (NVK with its
# compiler), and Mesa's utility libraries beside them. As with the Switch 2 SDK, PCSX2's Vulkan loader (VKLoader)
# defines public vk* function-pointer variables, so NVK's public vk* functions must not stay global in the link:
# everything is merged into one relocatable object in the order of the SDK's OpenGLConfig.cmake link group, which picks
# the same archive members as a normal link, and only then are the public vk* entry points (and Mesa's posix_memalign
# shim: common/Horizon/HorizonCompat.cpp provides newlib's) localized. lib/libEGL.a and lib/libvulkan.a are rewritten
# to name that object's archive, as for the Switch 2.
#
# Usage: prepare_mesa_sdk_switch.sh [mesa-switch-unified-sdk.zip] [output-directory]
#        prepare_mesa_sdk_switch.sh --check [output-directory]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${1:-}" == "--check" ]]; then
	shift
	OUT="${1:-${ARMSX2_SWITCH_MESA_SDK_ROOT:-${ROOT}/switch/deps/switch_mesa_sdk}}"
	for required in libvulkan_local.o libvulkan_local.a lib/libmesa_local.a lib/libEGL.a lib/libvulkan.a \
		include/EGL/egl.h include/vulkan/vulkan.h lib/cmake/OpenGL/OpenGLConfig.cmake; do
		if [[ ! -f "${OUT}/${required}" ]]; then
			echo "error: Switch Mesa SDK is incomplete: ${OUT}/${required} is missing" >&2
			echo "Run switch/deps/prepare_mesa_sdk_switch.sh first." >&2
			exit 1
		fi
	done
	echo "Ready: Switch Mesa SDK at ${OUT#${ROOT}/}"
	exit 0
fi

DEFAULT_DIST="${ROOT}/../mesa-nx/dist"
ZIP="${1:-${ARMSX2_SWITCH_MESA_SDK_ZIP:-${DEFAULT_DIST}/mesa-26.3.1-switch-unified-horizon-sdk.zip}}"
OUT="${2:-${ARMSX2_SWITCH_MESA_SDK_ROOT:-${ROOT}/switch/deps/switch_mesa_sdk}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"
NM="${BIN}/aarch64-none-elf-nm"
AR="${BIN}/aarch64-none-elf-ar"

if command -v bsdtar >/dev/null 2>&1; then
	BSDTAR="$(command -v bsdtar)"
elif [[ -x /c/Windows/System32/tar.exe ]]; then
	BSDTAR=/c/Windows/System32/tar.exe
else
	echo "A bsdtar-compatible archive tool is required to read the SDK zips" >&2
	exit 1
fi

if [[ ! -f "${ZIP}" ]]; then
	echo "Usage: $0 [mesa-switch-unified-horizon-sdk.zip] [output-directory]" >&2
	echo "Missing: ${ZIP}" >&2
	exit 1
fi

# The prepared prefix is reused while the zip is unchanged (a new libvulkan_local.o would relink everything).
STAMP_VALUE="$(sha256sum "${ZIP}" "${BASH_SOURCE[0]}" | awk '{ print $1 }' | tr '\n' ' ')"
STAMP="${OUT}/share/armsx2-switch-sdk.stamp"
if [[ -f "${STAMP}" && "$(cat "${STAMP}")" == "${STAMP_VALUE}" && -f "${OUT}/libvulkan_local.o" ]]; then
	echo "Switch Mesa SDK is up to date: ${OUT}"
	exit 0
fi

# The prefix inside an SDK zip: the directory holding lib/cmake/<package>/<file>.
zip_prefix() {
	"${BSDTAR}" -tf "$1" | awk -v marker="$2" '
		!found && index($0, marker) {
			sub(marker "$", "");
			print;
			found = 1;
		}'
}

SDK_ROOT="$(zip_prefix "${ZIP}" "/lib/cmake/OpenGL/OpenGLConfig.cmake")"
if [[ -z "${SDK_ROOT}" ]]; then
	echo "The archive is not a Mesa Switch SDK (OpenGLConfig.cmake is missing)." >&2
	exit 1
fi
case "${SDK_ROOT}" in
	*portlibs/switch) ;;
	*)
		echo "The archive is not a Switch SDK (prefix ${SDK_ROOT}, expected .../portlibs/switch)." >&2
		exit 1
		;;
esac

mkdir -p "$(dirname "${OUT}")"
WORK="$(mktemp -d "$(dirname "${OUT}")/switch-mesa-sdk.prepare.XXXXXX")"
trap 'rm -rf -- "${WORK}"' EXIT

echo "Extracting the Switch Mesa SDK from ${ZIP} ..."
"${BSDTAR}" -xf "${ZIP}" -C "${WORK}"
SDK_SOURCE="${WORK}/${SDK_ROOT}"
PREPARED="${WORK}/prepared"
mkdir -p "${PREPARED}"
cp -a "${SDK_SOURCE}/." "${PREPARED}/"
LIB="${PREPARED}/lib"

# OpenGLConfig.cmake's link group, minus the portlibs (libelf, libexpat, libzstd, libz) and libnx, linked last.
ARCHIVES=(libEGL.a libGL.a libglapi.a libvulkan.a libmesa_util_c11.a libblake3.a libmesa_util.a libmesa_util_simd.a
	libxmlconfig.a)
for required in "${ARCHIVES[@]}"; do
	if [[ ! -f "${LIB}/${required}" ]]; then
		echo "Switch Mesa SDK file is missing: lib/${required}" >&2
		exit 1
	fi
done
for required in include/EGL/egl.h include/vulkan/vulkan.h; do
	if [[ ! -f "${PREPARED}/${required}" ]]; then
		echo "Switch Mesa SDK file is missing: ${required}" >&2
		exit 1
	fi
done
if [[ "$(head -c 7 "${LIB}/libEGL.a")" != '!<arch>' ]]; then
	echo "lib/libEGL.a is not the unified SDK's archive; refusing to rewrite it" >&2
	exit 1
fi

# The roots: the public EGL and GL entry points, and the Vulkan ICD interface PCSX2's loader starts from with NVK's
# loaderless entry points.
ROOTS="${WORK}/roots.txt"
{
	for archive in libEGL.a libGL.a libglapi.a; do
		"${NM}" -g --defined-only "${LIB}/${archive}" 2>/dev/null |
			awk 'NF >= 3 && $2 ~ /^[TDBRW]$/ && $3 ~ /^(egl|gl)[A-Z]/ { print $3 }'
	done
	"${NM}" -g --defined-only "${LIB}/libvulkan.a" 2>/dev/null |
		awk 'NF >= 3 && $3 ~ /^(vk_icd|nvk_loaderless_)/ { print $3 }'
} | LC_ALL=C sort -u > "${ROOTS}"
if ! grep -qx "eglGetProcAddress" "${ROOTS}" || ! grep -qx "vk_icdGetInstanceProcAddr" "${ROOTS}"; then
	echo "The Switch SDK archives lack the expected EGL or Vulkan ICD entry points" >&2
	exit 1
fi
echo "Merging EGL, GL and NVK from $(wc -l < "${ROOTS}") entry points ..."

MERGED="${WORK}/libmesa_merged.o"
LOCAL="${PREPARED}/libmesa_local.o"
UNDEFINED_ROOTS=()
while IFS= read -r symbol; do
	UNDEFINED_ROOTS+=("-u" "${symbol}")
done < "${ROOTS}"
ARCHIVE_PATHS=()
for archive in "${ARCHIVES[@]}"; do
	ARCHIVE_PATHS+=("${LIB}/${archive}")
done
"${LD}" -r "${UNDEFINED_ROOTS[@]}" --start-group "${ARCHIVE_PATHS[@]}" --end-group -o "${MERGED}"

PUBLIC_VK_SYMBOLS="${WORK}/vulkan_public_symbols.txt"
"${NM}" -g --defined-only "${MERGED}" | awk '$NF ~ /^vk[A-Z]/ { print $NF }' | LC_ALL=C sort -u > "${PUBLIC_VK_SYMBOLS}"
if [[ ! -s "${PUBLIC_VK_SYMBOLS}" ]]; then
	echo "The merged Mesa object has no public Vulkan entry points to localize" >&2
	exit 1
fi
# newlib has no posix_memalign: common/Horizon/HorizonCompat.cpp provides it.
printf '%s\n' posix_memalign >> "${PUBLIC_VK_SYMBOLS}"
LC_ALL=C sort -u -o "${PUBLIC_VK_SYMBOLS}" "${PUBLIC_VK_SYMBOLS}"
"${OBJCOPY}" --localize-symbols="${PUBLIC_VK_SYMBOLS}" "${MERGED}" "${LOCAL}"

if "${NM}" -g --defined-only "${LOCAL}" | awk '$NF ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
	echo "The localized Mesa object still exports a public Vulkan entry point" >&2
	exit 1
fi
for symbol in \
	vk_icdGetInstanceProcAddr \
	vk_icdNegotiateLoaderICDInterfaceVersion \
	nvk_loaderless_GetInstanceProcAddr \
	nvk_loaderless_GetDeviceProcAddr \
	eglGetDisplay \
	eglGetProcAddress; do
	if ! "${NM}" -g --defined-only "${LOCAL}" |
		awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'; then
		echo "The localized Mesa object does not export ${symbol}" >&2
		exit 1
	fi
done

# libEGL.a and libvulkan.a now name the merged object's archive, with the portlibs and libraries it needs after it.
"${AR}" rcs "${LIB}/libmesa_local.a" "${LOCAL}"
printf 'INPUT(-lmesa_local -lstdc++ -lm -lelf -lzstd -lexpat -lz)\n' > "${LIB}/libEGL.a"
printf 'INPUT(-lmesa_local -lstdc++ -lm -lelf -lzstd -lexpat -lz)\n' > "${LIB}/libvulkan.a"
cp "${LOCAL}" "${PREPARED}/libvulkan_local.o"
cp "${LIB}/libmesa_local.a" "${PREPARED}/libvulkan_local.a"
rm -f "${LOCAL}"
mkdir -p "${PREPARED}/share"
printf '%s' "${STAMP_VALUE}" > "${PREPARED}/share/armsx2-switch-sdk.stamp"
basename "${ZIP}" > "${PREPARED}/share/armsx2-switch-sdk.source"

OLD="${OUT}.old.$$"
if [[ -e "${OUT}" ]]; then
	mv "${OUT}" "${OLD}"
fi
mv "${PREPARED}" "${OUT}"
if [[ -e "${OLD}" ]]; then
	rm -rf -- "${OLD}"
fi

echo "Prepared the Switch Mesa SDK at ${OUT}"
echo "Merged Mesa object (EGL, GL, NVK; vk* localized): ${OUT}/libvulkan_local.o"
