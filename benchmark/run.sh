#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${PREVIOUS_BENCHMARK_BUILD_DIR:-/private/tmp/previous-benchmark-build}"
source_config="${PREVIOUS_BENCHMARK_CONFIG:-$repo_dir/artifacts/OPENSTEP4.2.cfg}"
source_disk="${PREVIOUS_BENCHMARK_DISK:-$repo_dir/artifacts/OPENSTEP4.2.sd}"
warmup=60
duration=30
trials=1
skip_build=false
active_run_dir=""

cleanup() {
	if [[ -n "$active_run_dir" && -d "$active_run_dir" ]]; then
		/bin/rm -f "$active_run_dir/OPENSTEP4.2.sd" "$active_run_dir/benchmark.cfg"
		rmdir "$active_run_dir" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

usage() {
	printf 'Usage: %s [--warmup SECONDS] [--duration SECONDS] [--trials COUNT] [--no-build]\n' "$0"
}

while (( $# )); do
	case "$1" in
		--warmup) warmup="$2"; shift 2 ;;
		--duration) duration="$2"; shift 2 ;;
		--trials) trials="$2"; shift 2 ;;
		--no-build) skip_build=true; shift ;;
		--help|-h) usage; exit 0 ;;
		*) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
	esac
done

case "$warmup:$duration:$trials" in
	*[!0-9:]*|0:*|*:0:*|*:*:0) printf 'Durations and trial count must be positive whole numbers.\n' >&2; exit 2 ;;
esac

for required_file in "$source_config" "$source_disk"; do
	if [[ ! -f "$required_file" ]]; then
		printf 'Missing benchmark artifact: %s\n' "$required_file" >&2
		exit 1
	fi
done

if [[ "$skip_build" == false ]]; then
	cmake -S "$repo_dir" -B "$build_dir" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=arm64 \
		-DENABLE_TRACING=OFF \
		-DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -mcpu=apple-m1 -flto' \
		-DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -mcpu=apple-m1 -flto'
	cmake --build "$build_dir" --parallel "$(sysctl -n hw.logicalcpu)"
fi

app="$build_dir/src/Previous.app"
executable="$app/Contents/MacOS/Previous"
resources="$app/Contents/Resources"
if [[ ! -x "$executable" ]]; then
	printf 'Benchmark executable was not built: %s\n' "$executable" >&2
	exit 1
fi

timestamp="$(date '+%Y%m%d-%H%M%S')"
result_dir="$repo_dir/artifacts/benchmark-results/$timestamp"
mkdir -p "$result_dir"
summary="$result_dir/results.csv"
printf 'trial,elapsed_us,cycles,guest_mhz,configured_mhz,speed\n' > "$summary"

printf 'Results: %s\n' "$result_dir"
printf 'The source disk is copied with APFS copy-on-write cloning for each trial.\n'

for (( trial=1; trial<=trials; trial++ )); do
	run_dir="$(mktemp -d "/private/tmp/previous-benchmark-${timestamp}-${trial}.XXXXXX")"
	active_run_dir="$run_dir"
	disk="$run_dir/OPENSTEP4.2.sd"
	config="$run_dir/benchmark.cfg"
	log="$result_dir/trial-${trial}.log"
	profile="$result_dir/trial-${trial}.sample.txt"

	/bin/cp -c "$source_disk" "$disk"
	awk -v disk="$disk" -v resources="$resources" -v safe_dir="$run_dir" '
		/^\[/ { section=$0 }
		section == "[HardDisk]" && /^szImageName0 =/ { print "szImageName0 = " disk; next }
		/^bConfirmQuit =/ { print "bConfirmQuit = FALSE"; next }
		/^bShowConfigDialogAtStartup =/ { print "bShowConfigDialogAtStartup = FALSE"; next }
		/^bEthernetConnected =/ { print "bEthernetConnected = FALSE"; next }
		/^bNetworkTime =/ { print "bNetworkTime = FALSE"; next }
		/^szNFSPathName0 =/ { print "szNFSPathName0 = " safe_dir; next }
		/^szNFSPathName[1-3] =/ { sub(/=.*/, "="); print $0; next }
		/^szNFSHostName[0-3] =/ { sub(/=.*/, "="); print $0; next }
		/^szRom030FileName =/ { print "szRom030FileName = " resources "/Rev_1.0_v41.BIN"; next }
		/^szRom040FileName =/ { print "szRom040FileName = " resources "/Rev_2.5_v66.BIN"; next }
		/^szRomTurboFileName =/ { print "szRomTurboFileName = " resources "/Rev_3.3_v74.BIN"; next }
		{ print }
	' "$source_config" > "$config"

	printf 'Trial %d/%d: booting (warm-up %ss, measurement %ss)...\n' "$trial" "$trials" "$warmup" "$duration"
	"$executable" --config "$config" --benchmark-warmup "$warmup" \
		--benchmark-seconds "$duration" > "$log" 2>&1 &
	emulator_pid=$!

	while kill -0 "$emulator_pid" 2>/dev/null && ! grep -q '^BENCHMARK_BEGIN ' "$log"; do
		sleep 0.25
	done

	if kill -0 "$emulator_pid" 2>/dev/null; then
		profile_duration=$(( duration > 2 ? duration - 2 : duration ))
		/usr/bin/sample "$emulator_pid" "$profile_duration" 1 -mayDie -file "$profile" >/dev/null 2>&1 || true
	fi

	set +e
	wait "$emulator_pid"
	emulator_status=$?
	set -e

	result="$(grep '^BENCHMARK_RESULT ' "$log" | tail -1 || true)"
	if [[ -z "$result" ]]; then
		printf 'Trial %d failed (emulator exit %d). See %s\n' "$trial" "$emulator_status" "$log" >&2
		exit 1
	fi

	elapsed="$(printf '%s\n' "$result" | sed -E 's/.*elapsed_us=([^ ]+).*/\1/')"
	cycles="$(printf '%s\n' "$result" | sed -E 's/.*cycles=([^ ]+).*/\1/')"
	guest_mhz="$(printf '%s\n' "$result" | sed -E 's/.*guest_mhz=([^ ]+).*/\1/')"
	configured_mhz="$(printf '%s\n' "$result" | sed -E 's/.*configured_mhz=([^ ]+).*/\1/')"
	speed="$(printf '%s\n' "$result" | sed -E 's/.*speed=([^ ]+).*/\1/')"
	printf '%d,%s,%s,%s,%s,%s\n' "$trial" "$elapsed" "$cycles" "$guest_mhz" "$configured_mhz" "$speed" >> "$summary"
	printf 'Trial %d: %s MHz (%sx realtime at configured %s MHz)\n' "$trial" "$guest_mhz" "$speed" "$configured_mhz"

	/bin/rm -f "$disk" "$config"
	rmdir "$run_dir"
	active_run_dir=""
done

printf 'Summary: %s\n' "$summary"
