#!/usr/bin/env bash
# Build sdaf-bench and run the recommended benchmark matrix.
#
# The output directory contains one text file per configuration, a combined
# transcript, a TSV manifest, machine/build metadata, and the exact executable
# used for the run.

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_file="$script_dir/sdaf-bench.c"
output_dir=""
compiler=${CC:-cc}
seconds=0.15
channels=4
zstd_level=3
with_zstd=1
quick=0

usage() {
    cat <<'EOF'
Usage: sdaf-run-benchmarks.sh [options]

Build sdaf-bench and run:
  * a large-buffer reference configuration;
  * 1,024, 4,096 and 16,384 samples/channel with transform blocks of
    64, 128 and 256 values, using the width-specific default ADC noise; and
  * 0, 0.5, 1, 2, 4, 8, 16 and 64 LSB RMS noise at 16,384 samples and a
    128-value transform block.

Options:
  --source FILE       Benchmark source (default: sdaf-bench.c beside script)
  --output-dir DIR    Result directory (default: timestamped directory)
  --cc COMMAND        C compiler (default: $CC or cc)
  --seconds S         Minimum time per measured operation (default: 0.15)
  --channels N        Interleaved channels (default: 4)
  --zstd-level N      Zstandard compression level (default: 3)
  --no-zstd           Build and run without Zstandard
  --quick             Short smoke-test matrix (not suitable for analysis)
  -h, --help          Show this help

The full run is intentionally substantial. Use --seconds to trade timing
stability for run time. Existing output directories are never overwritten.
EOF
}

require_value() {
    if [ "$#" -lt 2 ]; then
        printf 'Missing value after %s\n' "$1" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --source)
            require_value "$@"
            source_file=$2
            shift 2
            ;;
        --output-dir)
            require_value "$@"
            output_dir=$2
            shift 2
            ;;
        --cc)
            require_value "$@"
            compiler=$2
            shift 2
            ;;
        --seconds)
            require_value "$@"
            seconds=$2
            shift 2
            ;;
        --channels)
            require_value "$@"
            channels=$2
            shift 2
            ;;
        --zstd-level)
            require_value "$@"
            zstd_level=$2
            shift 2
            ;;
        --no-zstd)
            with_zstd=0
            shift
            ;;
        --quick)
            quick=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$source_file" ]; then
    printf 'Benchmark source not found: %s\n' "$source_file" >&2
    exit 1
fi

case $seconds in
    ''|*[!0-9.eE+-]*)
        printf 'Invalid --seconds value: %s\n' "$seconds" >&2
        exit 2
        ;;
esac
case $channels in
    ''|*[!0-9]*)
        printf 'Invalid --channels value: %s\n' "$channels" >&2
        exit 2
        ;;
esac

timestamp=$(date -u '+%Y%m%dT%H%M%SZ')
host=$(hostname 2>/dev/null || printf 'unknown-host')
host=$(printf '%s' "$host" | tr -c 'A-Za-z0-9._-' '_')
if [ -z "$output_dir" ]; then
    output_dir="$script_dir/sdaf-bench-results-$timestamp-$host"
fi

if ! mkdir "$output_dir" 2>/dev/null; then
    printf 'Cannot create output directory (it may already exist): %s\n' \
        "$output_dir" >&2
    exit 1
fi

binary="$output_dir/sdaf-bench"
metadata="$output_dir/system.txt"
manifest="$output_dir/manifest.tsv"
combined="$output_dir/all-results.txt"

compile_args=(-O3 -std=c11 -Wall -Wextra -pedantic)
link_args=(-lm)
if [ "$with_zstd" -eq 1 ]; then
    compile_args+=(-DSDAF_HAVE_ZSTD)
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libzstd; then
        # pkg-config output is intentionally split into compiler arguments.
        # shellcheck disable=SC2207
        zstd_cflags=($(pkg-config --cflags libzstd))
        # shellcheck disable=SC2207
        zstd_libs=($(pkg-config --libs libzstd))
        compile_args+=("${zstd_cflags[@]}")
        link_args+=("${zstd_libs[@]}")
    else
        link_args+=(-lzstd)
    fi
fi

printf 'Building %s with %s...\n' "$source_file" "$compiler"
if ! "$compiler" "${compile_args[@]}" "$source_file" \
        "${link_args[@]}" -o "$binary"; then
    if [ "$with_zstd" -eq 1 ]; then
        printf '\nBuild failed. Install the Zstandard development package or use --no-zstd.\n' >&2
    fi
    exit 1
fi

{
    printf 'SDAF benchmark run metadata\n'
    printf 'started_utc: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'hostname: %s\n' "$(hostname 2>/dev/null || printf unknown)"
    printf 'uname: %s\n' "$(uname -a 2>/dev/null || printf unavailable)"
    if [ -r /etc/os-release ]; then
        printf '\n/etc/os-release:\n'
        sed -n '1,40p' /etc/os-release
    fi
    if command -v sw_vers >/dev/null 2>&1; then
        printf '\nmacOS:\n'
        sw_vers
    fi
    if command -v sysctl >/dev/null 2>&1; then
        cpu_brand=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)
        if [ -n "$cpu_brand" ]; then
            printf '\ncpu: %s\n' "$cpu_brand"
        fi
    fi
    if command -v lscpu >/dev/null 2>&1; then
        printf '\nlscpu:\n'
        lscpu
    fi
    printf '\ncompiler:\n'
    "$compiler" --version 2>&1 | sed -n '1,5p'
    printf '\nbuild command:'
    printf ' %q' "$compiler" "${compile_args[@]}" "$source_file" \
        "${link_args[@]}" -o "$binary"
    printf '\nsource_sha256: '
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$source_file" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$source_file" | awk '{print $1}'
    else
        printf 'unavailable\n'
    fi
    printf 'binary_sha256: '
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$binary" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$binary" | awk '{print $1}'
    else
        printf 'unavailable\n'
    fi
    printf 'minimum_seconds: %s\n' "$seconds"
    printf 'channels: %s\n' "$channels"
    printf 'zstd_enabled: %s\n' "$with_zstd"
    printf 'zstd_level: %s\n' "$zstd_level"
    printf 'quick_mode: %s\n' "$quick"
} >"$metadata"

printf 'run_id\tsweep\tsamples_per_channel\tblock_values\tnoise_rms\toutput_file\n' \
    >"$manifest"
: >"$combined"
run_number=0

run_one() {
    sweep=$1
    samples=$2
    block=$3
    noise=$4
    run_number=$((run_number + 1))
    run_id=$(printf '%03d' "$run_number")
    if [ "$noise" = default ]; then
        noise_label=default
    else
        noise_label=${noise//./p}
    fi
    result_name="${run_id}-${sweep}-s${samples}-b${block}-n${noise_label}.txt"
    result_file="$output_dir/$result_name"

    args=(--samples "$samples" --channels "$channels"
          --block-values "$block" --seconds "$seconds"
          --zstd-level "$zstd_level")
    if [ "$noise" != default ]; then
        args+=(--noise-rms "$noise")
    fi

    printf '\n[%s] %s: samples=%s block=%s noise=%s\n' \
        "$run_id" "$sweep" "$samples" "$block" "$noise"
    {
        printf '===== run %s: %s =====\n' "$run_id" "$result_name"
        printf 'command:'
        printf ' %q' "$binary" "${args[@]}"
        printf '\n\n'
        "$binary" "${args[@]}"
        printf '\n'
    } | tee "$result_file" >>"$combined"

    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$run_id" "$sweep" "$samples" "$block" "$noise" "$result_name" \
        >>"$manifest"
}

if [ "$quick" -eq 1 ]; then
    run_one smoke 4096 128 default
    run_one noise 4096 128 4
else
    # Preserve a directly comparable large-buffer reference result.
    run_one reference 262144 128 default

    for samples in 1024 4096 16384; do
        for block in 64 128 256; do
            run_one record-block "$samples" "$block" default
        done
    done

    # Keep this independent of the record/block sweep: crossing every noise
    # level with every shape adds much runtime without answering a new question.
    for noise in 0 0.5 1 2 4 8 16 64; do
        run_one noise 16384 128 "$noise"
    done
fi

printf '\ncompleted_utc: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
    >>"$metadata"
printf '\nCompleted %d configurations. Results: %s\n' \
    "$run_number" "$output_dir"

