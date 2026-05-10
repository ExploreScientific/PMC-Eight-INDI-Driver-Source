#!/usr/bin/env bash
set -euo pipefail

VERSION="${PMC8_BETA_VERSION:-PMC8-INDI-2.2.1-es1}"
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"${SOURCE_ROOT}/build-pmc8-beta"}"
OUTPUT_DIR="${OUTPUT_DIR:-"${SOURCE_ROOT}/artifacts"}"
SKIP_BUILD="${SKIP_BUILD:-0}"

usage() {
    cat <<USAGE
Usage: $0 [--skip-build] [--build-dir DIR] [--output-dir DIR]

Builds and packages the Explore Scientific PMC-Eight INDI beta driver.

Environment:
  PMC8_BETA_VERSION   Release label, default ${VERSION}
  BUILD_DIR           CMake build directory
  OUTPUT_DIR          Artifact output directory

USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

detect_platform() {
    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Linux) os="linux" ;;
        Darwin) os="macos" ;;
        *) os="$(echo "$os" | tr '[:upper:]' '[:lower:]')" ;;
    esac

    case "$arch" in
        x86_64|amd64) arch="x86_64" ;;
        aarch64|arm64) arch="arm64" ;;
        armv7l|armhf) arch="armhf" ;;
    esac

    echo "${os}-${arch}"
}

PLATFORM="${PMC8_BETA_PLATFORM:-$(detect_platform)}"
PACKAGE_NAME="pmc8-indi-beta-${VERSION}-${PLATFORM}"
STAGE_DIR="${BUILD_DIR}/${PACKAGE_NAME}"
DRIVER_PATH="${BUILD_DIR}/drivers/telescope/indi_pmc8_telescope"
SERVER_PATH="${BUILD_DIR}/indiserver/indiserver"

case "${PLATFORM}" in
    macos-*) INSTALL_RPATH="@loader_path/../lib" ;;
    *) INSTALL_RPATH="\$ORIGIN/../lib" ;;
esac

if [[ "${SKIP_BUILD}" != "1" ]]; then
    cmake -S "${SOURCE_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
        -DCMAKE_INSTALL_RPATH="${INSTALL_RPATH}"
    cmake --build "${BUILD_DIR}" --target indiserver indi_pmc8_telescope --parallel
fi

if [[ ! -x "${DRIVER_PATH}" ]]; then
    echo "Driver executable not found: ${DRIVER_PATH}" >&2
    exit 1
fi

if [[ ! -x "${SERVER_PATH}" ]]; then
    echo "INDI server executable not found: ${SERVER_PATH}" >&2
    exit 1
fi

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/bin" "${STAGE_DIR}/lib" "${STAGE_DIR}/doc"
cp "${DRIVER_PATH}" "${STAGE_DIR}/bin/indi_pmc8_telescope"
cp "${SERVER_PATH}" "${STAGE_DIR}/bin/indiserver"
cp "${SOURCE_ROOT}/explorescientific/pmc8-whats-new.md" "${STAGE_DIR}/doc/pmc8-whats-new.md"

copy_library_family() {
    local pattern="$1"
    local found=0

    while IFS= read -r -d '' lib; do
        cp -a "${lib}" "${STAGE_DIR}/lib/"
        found=1
    done < <(find "${BUILD_DIR}/libs" -name "${pattern}" -print0)

    if [[ "${found}" != "1" ]]; then
        echo "Warning: no library matched ${pattern}" >&2
    fi
}

case "${PLATFORM}" in
    macos-*)
        copy_library_family "libindidriver*.dylib"
        copy_library_family "libindiAlignmentDriver*.dylib"
        copy_library_family "libindiclient*.dylib"
        ;;
    *)
        copy_library_family "libindidriver.so*"
        copy_library_family "libindiAlignmentDriver.so*"
        copy_library_family "libindiclient.so*"
        ;;
esac

cat > "${STAGE_DIR}/README-INSTALL.md" <<'README'
# Explore Scientific PMC-Eight INDI Beta Driver

This artifact contains the beta `indi_pmc8_telescope` executable, a matching `indiserver`, and the matching INDI shared libraries built from the same source tree.

It is not a complete KStars distribution. It replaces the PMC-Eight telescope driver executable and installs a matching INDI server/runtime layer used by an existing INDI/KStars installation.

## Install

From inside the extracted artifact directory:

```bash
sudo ./install.sh
```

By default the installer copies the driver to:

```text
/usr/local/bin/indi_pmc8_telescope
```

It also installs:

```text
/usr/local/bin/indiserver
/usr/local/lib/libindi*.so or /usr/local/lib/libindi*.dylib
```

To install somewhere else:

```bash
sudo ./install.sh /opt/homebrew/bin
```

## Verify

```bash
which indi_pmc8_telescope
which indiserver
indi_pmc8_telescope --help
```

Restart KStars/Ekos after installation.

## Restore

The installer backs up existing files in the install directories with a timestamped `.bak-YYYYMMDD-HHMMSS` suffix.

README

cat > "${STAGE_DIR}/install.sh" <<'INSTALL'
#!/usr/bin/env bash
set -euo pipefail

PREFIX="${1:-/usr/local}"
if [[ "${PREFIX}" == */bin ]]; then
    BINDIR="${PREFIX}"
else
    BINDIR="${PREFIX}/bin"
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBDIR="${PREFIX%/}/lib"

install_one() {
    local src="$1"
    local dst="$2"
    local mode="$3"

    if [[ ! -e "${src}" ]]; then
        echo "Missing file: ${src}" >&2
        exit 1
    fi

    if [[ -e "${dst}" ]]; then
        local ts
        ts="$(date +%Y%m%d-%H%M%S)"
        cp -a "${dst}" "${dst}.bak-${ts}"
        echo "Backed up existing file to ${dst}.bak-${ts}"
    fi

    install -m "${mode}" "${src}" "${dst}"
    echo "Installed ${dst}"
}

mkdir -p "${BINDIR}" "${LIBDIR}"

install_one "${SRC_DIR}/bin/indi_pmc8_telescope" "${BINDIR}/indi_pmc8_telescope" 755
install_one "${SRC_DIR}/bin/indiserver" "${BINDIR}/indiserver" 755

for lib in "${SRC_DIR}"/lib/libindi*; do
    [[ -e "${lib}" ]] || continue
    install_one "${lib}" "${LIBDIR}/$(basename "${lib}")" 755
done

if command -v ldconfig >/dev/null 2>&1; then
    ldconfig || true
fi

echo
echo "Restart KStars/Ekos before testing the beta server/driver."
INSTALL

chmod +x "${STAGE_DIR}/install.sh"

{
    echo "Package: ${PACKAGE_NAME}"
    echo "Version: ${VERSION}"
    echo "Platform: ${PLATFORM}"
    echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Source commit: $(git -C "${SOURCE_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo
    echo "Executable:"
    file "${STAGE_DIR}/bin/indi_pmc8_telescope" || true
    file "${STAGE_DIR}/bin/indiserver" || true
    echo
    echo "Bundled INDI libraries:"
    ls -l "${STAGE_DIR}/lib" || true
    echo
    if command -v ldd >/dev/null 2>&1; then
        echo "Linux shared library dependencies:"
        ldd "${STAGE_DIR}/bin/indi_pmc8_telescope" || true
        echo
        ldd "${STAGE_DIR}/bin/indiserver" || true
    elif command -v otool >/dev/null 2>&1; then
        echo "macOS shared library dependencies:"
        otool -L "${STAGE_DIR}/bin/indi_pmc8_telescope" || true
        echo
        otool -L "${STAGE_DIR}/bin/indiserver" || true
    fi
} > "${STAGE_DIR}/MANIFEST.txt"

mkdir -p "${OUTPUT_DIR}"

if [[ "${PLATFORM}" == macos-* ]]; then
    (cd "${BUILD_DIR}" && zip -qr "${OUTPUT_DIR}/${PACKAGE_NAME}.zip" "${PACKAGE_NAME}")
    echo "${OUTPUT_DIR}/${PACKAGE_NAME}.zip"
else
    tar -C "${BUILD_DIR}" -czf "${OUTPUT_DIR}/${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
    echo "${OUTPUT_DIR}/${PACKAGE_NAME}.tar.gz"
fi
