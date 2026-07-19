#!/usr/bin/env bash
set -euo pipefail

# Reproducible scheduler sweep used by docs/ASYNC_WORKER_SCHEDULER.md.
# Run from sketch/: bash sim/benchmark_scheduler.sh
make bin/feasibility_sim

run_case() {
    local high="$1"
    local low="$2"
    local resident="$3"
    for seed in 1 2 3; do
        timeout 12 ./bin/feasibility_sim \
            --workers 10 --branching 150 --survival 0.06 \
            --kernel-us 500 --progress-ms 0 --max-seconds 5 \
            --seed "$seed" --inflight-high "$high" \
            --inflight-low "$low" --resident "$resident" \
            | sed -n -e '/configuration:/p' -e '/throughput:/p' \
                -e '/worker scheduler:/p'
    done
}

run_case 150 1 2000
run_case 500 375 4000
run_case 1000 750 5000
run_case 2000 1500 8000
run_case 4000 3000 12000
