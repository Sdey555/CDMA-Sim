import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

EXPERIMENT_DIR = Path("experiments")
GRAPH_DIR = EXPERIMENT_DIR / "graphs"
GRAPH_DIR.mkdir(parents=True, exist_ok=True)

CASE1_FILE = EXPERIMENT_DIR / "case1_cdma_vs_p.csv"
CASE2_FILE = EXPERIMENT_DIR / "case2_cdma_vs_stations.csv"

def save_plot(filename):
    plt.tight_layout()
    plt.savefig(GRAPH_DIR / filename, dpi=300, bbox_inches="tight")
    plt.close()

if CASE1_FILE.exists():
    df1 = pd.read_csv(CASE1_FILE)
    df1_avg = df1.groupby(["protocol", "N", "p"], as_index=False).agg({
        "success": "mean",
        "throughput": "mean",
        "avg_delay": "mean",
        "collision_rate": "mean"
    })
    
    plt.figure(figsize=(7, 5))
    plt.plot(df1_avg["p"], df1_avg["throughput"], marker="o", color="blue", label="CDMA Throughput")
    plt.title("CDMA: Throughput vs Transmission Probability (p) [N=5]")
    plt.xlabel("Transmission Probability (p)")
    plt.ylabel("Throughput (frames/slot)")
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend()
    save_plot("case1_cdma_throughput_vs_p.png")
    print(f"Generated: {GRAPH_DIR / 'case1_cdma_throughput_vs_p.png'}")

if CASE2_FILE.exists():
    df2 = pd.read_csv(CASE2_FILE)
    df2_avg = df2.groupby(["protocol", "N", "p"], as_index=False).agg({
        "success": "mean",
        "throughput": "mean",
        "avg_delay": "mean",
        "collision_rate": "mean"
    })
    
    plt.figure(figsize=(7, 5))
    plt.plot(df2_avg["N"], df2_avg["throughput"], marker="s", color="green", label="CDMA Throughput (p=1.0)")
    plt.title("CDMA: Throughput vs Number of Stations (N)")
    plt.xlabel("Number of Stations (N)")
    plt.ylabel("Throughput (frames/slot)")
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend()
    save_plot("case2_cdma_throughput_vs_stations.png")
    print(f"Generated: {GRAPH_DIR / 'case2_cdma_throughput_vs_stations.png'}")
