#!/usr/bin/env bash

# ==============================================
# CDMA Simulation Experiment Runner
# ==============================================
#
# Saves CSV results into experiments/
#
# Usage:
#   chmod +x run_experiments.sh
#   ./run_experiments.sh
#
# ==============================================

set -u

BINARY="./cdma_udp_sim"
OUTDIR="experiments"

# ----------------------------------------------
# Simulation settings
# ----------------------------------------------

SLOTS=100
RUNS=3

# Case I: Varying traffic probability p
CASE1_N=5
P_VALUES=(0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0)

# Case II: Varying number of stations N
STATIONS=(2 3 4 5 6 8 10)
FIXED_P=1.0

mkdir -p "$OUTDIR"

# ----------------------------------------------
# Check executable
# ----------------------------------------------

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: $BINARY not found or not executable."
    echo "Running make..."
    make
fi

# ----------------------------------------------
# Extract final statistics from program output
# ----------------------------------------------

parse_result()
{
    awk '
    /Successful transmissions/ { success=$NF }
    /Collision slots/         { collision=$NF }
    /Idle slots/              { idle=$NF }

    /Average delay/           { delay=$(NF-1) }
    /Throughput/              { throughput=$(NF-1) }

    /Collision rate/          { rate=$NF }

    END {
        if (success == "")    success="NA"
        if (collision == "")  collision="NA"
        if (idle == "")       idle="NA"
        if (delay == "")      delay="NA"
        if (throughput == "") throughput="NA"
        if (rate == "")       rate="NA"

        printf "%s,%s,%s,%s,%s,%s\n",
               success,
               collision,
               idle,
               delay,
               throughput,
               rate
    }'
}

# ==============================================
# CASE I: CDMA Throughput vs Traffic Probability p
# ==============================================

CASE1="$OUTDIR/case1_cdma_vs_p.csv"

echo
echo "=============================================="
echo " CASE I: CDMA Throughput vs Traffic Probability p"
echo "=============================================="
echo "Stations : $CASE1_N"
echo "Slots    : $SLOTS"
echo "Runs     : $RUNS"
echo

echo "protocol,N,p,run,success,collision_slots,idle_slots,avg_delay,throughput,collision_rate" > "$CASE1"

for p in "${P_VALUES[@]}"
do
    for ((run=1; run<=RUNS; run++))
    do
        echo "p=$p  run=$run/$RUNS"

        result=$(
            "$BINARY" "$CASE1_N" "$SLOTS" "$p" 2>/dev/null |
            parse_result
        )

        IFS=',' read -r \
            success \
            collision \
            idle \
            delay \
            throughput \
            rate <<< "$result"

        echo "CDMA,$CASE1_N,$p,$run,$success,$collision,$idle,$delay,$throughput,$rate" \
            >> "$CASE1"
    done
done

echo
echo "Case I complete."
echo "Saved: $CASE1"

# ==============================================
# CASE II: CDMA Throughput vs Number of Stations N
# ==============================================

CASE2="$OUTDIR/case2_cdma_vs_stations.csv"

echo
echo "=============================================="
echo " CASE II: CDMA Throughput vs Number of Stations"
echo "=============================================="
echo "Stations      : ${STATIONS[*]}"
echo "Slots         : $SLOTS"
echo "Runs          : $RUNS"
echo "Traffic Prob p: $FIXED_P"
echo

echo "protocol,N,p,run,success,collision_slots,idle_slots,avg_delay,throughput,collision_rate" > "$CASE2"

for n in "${STATIONS[@]}"
do
    echo "Stations N=$n"

    for ((run=1; run<=RUNS; run++))
    do
        echo "CDMA  N=$n  run=$run/$RUNS"

        result=$(
            "$BINARY" "$n" "$SLOTS" "$FIXED_P" 2>/dev/null |
            parse_result
        )

        IFS=',' read -r \
            success \
            collision \
            idle \
            delay \
            throughput \
            rate <<< "$result"

        echo "CDMA,$n,$FIXED_P,$run,$success,$collision,$idle,$delay,$throughput,$rate" \
            >> "$CASE2"
    done
done

echo
echo "=============================================="
echo " EXPERIMENTS COMPLETE"
echo "=============================================="
echo "CSV files saved to $OUTDIR"
