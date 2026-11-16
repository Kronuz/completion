#!/bin/sh
# Builds completion_lane_probe in Release from a source tree: lane_build.sh SOURCE_DIR BUILD_DIR
# Prints the binary's architecture, as measurement.md asks, and its path.
set -e
cmake -S "$1" -B "$2" -DCMAKE_BUILD_TYPE=Release -DCOMPLETION_BUILD_BENCH=ON -DBUILD_TESTING=OFF > "$2.configure.log" 2>&1
cmake --build "$2" --target completion_lane_probe -j4 > "$2.build.log" 2>&1
file "$2/completion_lane_probe"
