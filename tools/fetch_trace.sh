#!/usr/bin/env bash
# Downloads a slice of the Google cluster trace 2011 (clusterdata-2011-2), the trace cited by
# the Borg paper. The full trace is ~40 GB; a snapshot only needs machine_events plus the
# first few task_events parts (each part covers ~1.4 hours of the month).
#
#   tools/fetch_trace.sh [parts] [dir]      default: 6 parts (~8.5 h, ~11 MB) into data/trace
set -euo pipefail

PARTS=${1:-6}
DIR=${2:-data/trace}
BASE=https://commondatastorage.googleapis.com/clusterdata-2011-2

mkdir -p "$DIR/machine_events" "$DIR/task_events"
echo "machine_events -> $DIR/machine_events"
curl -fsSL -o "$DIR/machine_events/part-00000-of-00001.csv.gz" "$BASE/machine_events/part-00000-of-00001.csv.gz"
for ((i = 0; i < PARTS; i++)); do
  f=$(printf "part-%05d-of-00500.csv.gz" "$i")
  echo "task_events/$f"
  curl -fsSL -o "$DIR/task_events/$f" "$BASE/task_events/$f"
done
echo "done: $(du -sh "$DIR" | cut -f1) in $DIR"
