#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/allocator_benchmarks"
GPERFTOOLS_DIR="${GPERFTOOLS_DIR:-/home/ubuntu/gperftools}"
CXX="${CXX:-g++}"

CXXFLAGS=(
    -std=c++17
    -O3
    -Wall
    -Wextra
    -pthread
    -I"${ROOT_DIR}"
)

MEMPOOL_SOURCES=(
    "${ROOT_DIR}/CentralCache.cpp"
    "${ROOT_DIR}/PageCache.cpp"
    "${ROOT_DIR}/ThreadCache.cpp"
    "${ROOT_DIR}/ConcurrentAlloc.cpp"
    "${ROOT_DIR}/MallocReplacement.cpp"
)

BENCHMARK_SOURCE="${ROOT_DIR}/test/benchmark.cpp"
MEMPOOL_LIB="${BUILD_DIR}/libmempool_benchmark.so"
NATIVE_BIN="${BUILD_DIR}/benchmark_native"
MEMPOOL_BIN="${BUILD_DIR}/benchmark_mempool"
TCMALLOC_BIN="${BUILD_DIR}/benchmark_gperftools_tcmalloc"

find_tcmalloc_dir() {
    local candidate
    for candidate in \
        "${GPERFTOOLS_DIR}/lib" \
        "${GPERFTOOLS_DIR}/.libs" \
        "/usr/local/lib" \
        "/usr/lib" \
        "/usr/lib/x86_64-linux-gnu"
    do
        if [[ -e "${candidate}/libtcmalloc.so" ]] ||
           compgen -G "${candidate}/libtcmalloc.so.*" >/dev/null ||
           [[ -e "${candidate}/libtcmalloc.a" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

run_case() {
    local title="$1"
    local binary="$2"

    printf '\n===== %s =====\n' "${title}"
    "${binary}"
}

build_executable_if_missing() {
    local binary="$1"
    shift

    if [[ -x "${binary}" ]]; then
        printf 'Already exists, skip build: %s\n' "${binary}"
        return 0
    fi

    "$@"
}

build_file_if_missing() {
    local output="$1"
    shift

    if [[ -e "${output}" ]]; then
        printf 'Already exists, skip build: %s\n' "${output}"
        return 0
    fi

    "$@"
}

mkdir -p "${BUILD_DIR}"

printf 'Compiler: %s\n' "${CXX}"
printf 'Build dir: %s\n' "${BUILD_DIR}"

printf '\n[1/4] Native benchmark\n'
build_executable_if_missing "${NATIVE_BIN}" \
    "${CXX}" "${CXXFLAGS[@]}" "${BENCHMARK_SOURCE}" -o "${NATIVE_BIN}"

printf '[2/4] memPool replacement shared library\n'
build_file_if_missing "${MEMPOOL_LIB}" \
    "${CXX}" "${CXXFLAGS[@]}" -fPIC -shared "${MEMPOOL_SOURCES[@]}" -o "${MEMPOOL_LIB}"

printf '[3/4] Benchmark linked with memPool replacement\n'
build_executable_if_missing "${MEMPOOL_BIN}" \
    "${CXX}" "${CXXFLAGS[@]}" "${BENCHMARK_SOURCE}" \
    -L"${BUILD_DIR}" \
    -Wl,--no-as-needed -lmempool_benchmark \
    -Wl,-rpath,"${BUILD_DIR}" \
    -o "${MEMPOOL_BIN}"

TCMALLOC_LIB_DIR="$(find_tcmalloc_dir)" || {
    printf 'error: libtcmalloc not found. Set GPERFTOOLS_DIR=/path/to/gperftools.\n' >&2
    exit 1
}

printf '[4/4] Benchmark linked with gperftools tcmalloc: %s\n' "${TCMALLOC_LIB_DIR}"
build_executable_if_missing "${TCMALLOC_BIN}" \
    "${CXX}" "${CXXFLAGS[@]}" "${BENCHMARK_SOURCE}" \
    -L"${TCMALLOC_LIB_DIR}" \
    -Wl,--no-as-needed -ltcmalloc \
    -Wl,-rpath,"${TCMALLOC_LIB_DIR}" \
    -o "${TCMALLOC_BIN}"

run_case "native allocator" "${NATIVE_BIN}"
run_case "memPool linked malloc/new replacement" "${MEMPOOL_BIN}"
run_case "gperftools tcmalloc linked malloc/new replacement" "${TCMALLOC_BIN}"
