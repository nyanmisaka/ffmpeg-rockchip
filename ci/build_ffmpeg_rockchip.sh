#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <shared|static> [jobs]"
  exit 1
fi

VARIANT="$1"
JOBS="${2:-4}"

if [[ "$VARIANT" != "shared" && "$VARIANT" != "static" ]]; then
  echo "Invalid variant: $VARIANT"
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${ROOT_DIR}/.ci-build/${VARIANT}"
SRC_DIR="${WORK_DIR}/src"
BUILD_DIR="${WORK_DIR}/build"
PREFIX_DIR="${WORK_DIR}/prefix"
DIST_DIR="${ROOT_DIR}/dist"
FFMPEG_SRC="${ROOT_DIR}"

LIBDRM_TAG="libdrm-2.4.123"
MBEDTLS_TAG="v2.28.9"
MPP_BRANCH="jellyfin-mpp"
RGA_BRANCH="jellyfin-rga"

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${PREFIX_DIR}" "${DIST_DIR}"

export PATH="$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="${PREFIX_DIR}/lib:${LD_LIBRARY_PATH:-}"

setup_pkg_config_path() {
  local paths=()
  local d
  for d in "${PREFIX_DIR}/lib/pkgconfig" "${PREFIX_DIR}/lib64/pkgconfig" "${PREFIX_DIR}/share/pkgconfig"; do
    if [[ -d "${d}" ]]; then
      paths+=("${d}")
    fi
  done
  if [[ ${#paths[@]} -gt 0 ]]; then
    local joined
    joined="$(IFS=:; echo "${paths[*]}")"
    export PKG_CONFIG_PATH="${joined}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  fi
}

setup_pkg_config_path

git_clone_retry() {
  local repo_url="$1"
  local branch="$2"
  local dst_dir="$3"
  local max_retries="${4:-5}"
  local retry_delay_s="${5:-3}"
  local attempt=1

  while (( attempt <= max_retries )); do
    if git clone --depth=1 --branch "${branch}" "${repo_url}" "${dst_dir}"; then
      return 0
    fi

    if (( attempt == max_retries )); then
      echo "ERROR: failed to clone ${repo_url} (branch: ${branch}) after ${max_retries} attempts"
      return 1
    fi

    echo "WARN: clone failed for ${repo_url} (attempt ${attempt}/${max_retries}), retrying in ${retry_delay_s}s..."
    rm -rf "${dst_dir}"
    sleep "${retry_delay_s}"
    attempt=$((attempt + 1))
  done
}

pc_set_or_append_field() {
  local pc_file="$1"
  local key="$2"
  local value="$3"
  if grep -q "^${key}:" "${pc_file}"; then
    sed -i "s|^${key}:.*|${key}: ${value}|" "${pc_file}"
  else
    printf '%s: %s\n' "${key}" "${value}" >> "${pc_file}"
  fi
}

fetch_sources() {
  if [[ ! -d "${SRC_DIR}/libdrm/.git" ]]; then
    git clone --depth=1 --branch "${LIBDRM_TAG}" https://gitlab.freedesktop.org/mesa/drm.git "${SRC_DIR}/libdrm"
  fi
  if [[ ! -d "${SRC_DIR}/mbedtls" ]]; then
    curl -L --fail "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/${MBEDTLS_TAG}.tar.gz" -o "${SRC_DIR}/mbedtls.tar.gz"
    tar -xf "${SRC_DIR}/mbedtls.tar.gz" -C "${SRC_DIR}"
    mv "${SRC_DIR}/mbedtls-${MBEDTLS_TAG#v}" "${SRC_DIR}/mbedtls"
  fi
  if [[ ! -d "${SRC_DIR}/rkmpp/.git" ]]; then
    git_clone_retry "https://gitee.com/nyanmisaka/mpp.git" "${MPP_BRANCH}" "${SRC_DIR}/rkmpp" 5 4
  fi
  if [[ ! -d "${SRC_DIR}/rkrga/.git" ]]; then
    git_clone_retry "https://gitee.com/nyanmisaka/rga.git" "${RGA_BRANCH}" "${SRC_DIR}/rkrga" 5 4
  fi
}

build_libdrm() {
  local default_library="$1"
  rm -rf "${BUILD_DIR}/libdrm"
  meson setup "${SRC_DIR}/libdrm" "${BUILD_DIR}/libdrm" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --buildtype=release \
    --default-library="${default_library}" \
    -Dintel=disabled \
    -Dradeon=disabled \
    -Damdgpu=disabled \
    -Dnouveau=disabled \
    -Dvmwgfx=disabled \
    -Dfreedreno=disabled \
    -Dvc4=disabled \
    -Detnaviv=disabled \
    -Dman-pages=disabled \
    -Dtests=false
  ninja -C "${BUILD_DIR}/libdrm" -j"${JOBS}"
  ninja -C "${BUILD_DIR}/libdrm" install
}

build_mbedtls() {
  local static_on="$1"
  local shared_on="$2"
  cmake -S "${SRC_DIR}/mbedtls" -B "${BUILD_DIR}/mbedtls" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX_DIR}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DUSE_STATIC_MBEDTLS_LIBRARY="${static_on}" \
    -DUSE_SHARED_MBEDTLS_LIBRARY="${shared_on}" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF
  cmake --build "${BUILD_DIR}/mbedtls" -j"${JOBS}"
  cmake --install "${BUILD_DIR}/mbedtls"
}

build_mpp() {
  local shared_libs="$1"
  cmake -S "${SRC_DIR}/rkmpp" -B "${BUILD_DIR}/rkmpp" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX_DIR}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_DO_STRIP=OFF \
    -DCMAKE_C_FLAGS="-g1" \
    -DCMAKE_CXX_FLAGS="-g1" \
    -DBUILD_SHARED_LIBS="${shared_libs}" \
    -DBUILD_TEST=OFF
  cmake --build "${BUILD_DIR}/rkmpp" -j"${JOBS}"
  cmake --install "${BUILD_DIR}/rkmpp"
  setup_pkg_config_path

  if [[ "${shared_libs}" == "OFF" ]]; then
    local pc=""
    if [[ -f "${PREFIX_DIR}/lib/pkgconfig/rockchip_mpp.pc" ]]; then
      pc="${PREFIX_DIR}/lib/pkgconfig/rockchip_mpp.pc"
    elif [[ -f "${PREFIX_DIR}/lib64/pkgconfig/rockchip_mpp.pc" ]]; then
      pc="${PREFIX_DIR}/lib64/pkgconfig/rockchip_mpp.pc"
    fi

    if [[ -n "${pc}" ]]; then
      # Static mpp needs extra system libs; upstream .pc leaves Libs.private empty.
      pc_set_or_append_field "${pc}" "Libs.private" "-pthread -lrt -ldl"
    fi
  fi
}

build_rga() {
  local default_library="$1"
  if [[ "${default_library}" == "static" ]]; then
    # Upstream rga meson.build hardcodes shared_library(), which ignores
    # --default-library=static. Switch to library() so Meson honors
    # the selected default library type for the static variant.
    if grep -q 'shared_library(' "${SRC_DIR}/rkrga/meson.build"; then
      sed -i '0,/shared_library(/s//library(/' "${SRC_DIR}/rkrga/meson.build"
    fi
  fi
  rm -rf "${BUILD_DIR}/rkrga"
  meson setup "${SRC_DIR}/rkrga" "${BUILD_DIR}/rkrga" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --buildtype=release \
    -Db_strip=false \
    --default-library="${default_library}" \
    -Dc_args=-g1 \
    -Dcpp_args="-fpermissive -g1" \
    -Dlibdrm=false \
    -Dlibrga_demo=false
  ninja -C "${BUILD_DIR}/rkrga" -j"${JOBS}"
  ninja -C "${BUILD_DIR}/rkrga" install
  setup_pkg_config_path

  if [[ "${default_library}" == "static" ]]; then
    local pc=""
    if [[ -f "${PREFIX_DIR}/lib/pkgconfig/librga.pc" ]]; then
      pc="${PREFIX_DIR}/lib/pkgconfig/librga.pc"
    elif [[ -f "${PREFIX_DIR}/lib64/pkgconfig/librga.pc" ]]; then
      pc="${PREFIX_DIR}/lib64/pkgconfig/librga.pc"
    fi
    if [[ -n "${pc}" ]]; then
      # Static librga is C++; declare runtime libs for static link checks.
      pc_set_or_append_field "${pc}" "Libs.private" "-lstdc++ -pthread -ldl -lrt -lm"
      # Keep include semantics robust for headers like <rga/RgaApi.h>.
      pc_set_or_append_field "${pc}" "Cflags" "-I\${includedir} -I\${includedir}/rga"
    fi
  fi
}

build_ffmpeg() {
  local ffmpeg_shared_flag="$1"
  local ffmpeg_static_flag="$2"
  local pkg_config_flags="$3"
  local extra_ldflags
  local extra_libs
  local extra_libs_flag=()
  extra_ldflags="-L${PREFIX_DIR}/lib -Wl,-rpath,\$ORIGIN/../lib -Wl,-rpath,\$ORIGIN -Wl,--enable-new-dtags"
  extra_libs=""
  if [[ "${pkg_config_flags}" == *"--static"* ]]; then
    extra_libs="-lstdc++ -pthread -ldl -lrt -lm"
    extra_libs_flag=(--extra-libs="${extra_libs}")
  fi

  setup_pkg_config_path
  if ! pkg-config ${pkg_config_flags} --exists rockchip_mpp; then
    echo "ERROR: rockchip_mpp not found by pkg-config"
    echo "PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}"
    find "${PREFIX_DIR}" -maxdepth 4 -name 'rockchip_mpp.pc' -o -name 'rockchip_vpu.pc' || true
    exit 1
  fi
  if ! pkg-config ${pkg_config_flags} --exists librga; then
    echo "ERROR: librga not found by pkg-config"
    echo "PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}"
    find "${PREFIX_DIR}" -maxdepth 4 -name 'librga.pc' || true
    exit 1
  fi

  cd "${FFMPEG_SRC}"
  make distclean >/dev/null 2>&1 || true

  ./configure \
    --prefix="${PREFIX_DIR}" \
    --enable-version3 \
    --disable-stripping \
    --enable-libdrm \
    --enable-rkmpp \
    --enable-rkrga \
    --disable-libxcb \
    --disable-iconv \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-alsa \
    --disable-muxer=spdif \
    --disable-demuxer=spdif \
    --enable-mbedtls \
    --enable-pic \
    --extra-cflags="-fPIC -g1 -I${PREFIX_DIR}/include" \
    --extra-ldflags="${extra_ldflags}" \
    "${extra_libs_flag[@]}" \
    --pkg-config-flags="${pkg_config_flags}" \
    "${ffmpeg_shared_flag}" \
    "${ffmpeg_static_flag}"

  make -j"${JOBS}"
  make install
}

package_output() {
  local arch
  arch="$(uname -m)"
  local out_name
  out_name="ffmpeg-rockchip-${VARIANT}-${arch}"
  local out_dir="${DIST_DIR}/${out_name}"

  rm -rf "${out_dir}"
  mkdir -p "${out_dir}"
  cp -a "${PREFIX_DIR}"/* "${out_dir}/"

  if command -v patchelf >/dev/null 2>&1; then
    find "${out_dir}/bin" -maxdepth 1 -type f -executable \
      -exec patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN' {} +
    find "${out_dir}/lib" -maxdepth 1 -type f -name '*.so*' \
      -exec patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' {} +
  fi

  cat > "${out_dir}/BUILD_INFO.txt" <<INFO
variant=${VARIANT}
arch=${arch}
configure_flags=--enable-version3 --disable-stripping --enable-libdrm --enable-rkmpp --enable-rkrga --disable-libxcb --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-alsa --disable-muxer=spdif --disable-demuxer=spdif --enable-mbedtls --enable-pic --extra-cflags=-fPIC
debug_level=-g1
INFO

  tar -C "${DIST_DIR}" -czf "${DIST_DIR}/${out_name}.tar.gz" "${out_name}"
  echo "Created package: ${DIST_DIR}/${out_name}.tar.gz"
}

fetch_sources

if [[ "$VARIANT" == "shared" ]]; then
  build_libdrm shared
  build_mbedtls ON OFF
  build_mpp ON
  build_rga shared
  build_ffmpeg --enable-shared --disable-static ""
else
  build_libdrm static
  build_mbedtls ON OFF
  build_mpp OFF
  build_rga static
  build_ffmpeg --disable-shared --enable-static "--static"
fi

package_output
