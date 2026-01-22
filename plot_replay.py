# plot_replay.py
# Build two plots from replay_log.csv:
# 1) v_cmd vs time with STOP (red) + AVOID (yellow) shading
# 2) vL & vR vs time with AVOID_RIGHT (orange) and AVOID_LEFT (green) shaded

import csv
import matplotlib.pyplot as plt

t, v_cmd, w_cmd, mode, vL, vR = [], [], [], [], [], []

# replay_log.csv should come from: ./rover_m4 --replay rover_inputs_15s_new.jsonl
with open("replay_log.csv", "r") as f:
    r = csv.DictReader(f)
    for row in r:
        t.append(float(row["time_s"]))
        v_cmd.append(float(row["v_cmd"]))
        w_cmd.append(float(row["w_cmd"]))
        mode.append(int(row["mode"]))
        # Column names from replay_log.csv written by your code
        vL.append(float(row["FL"]))   # left track/wheels
        vR.append(float(row["FR"]))   # right track/wheels

def shade_span(ax, t, mode, allowed_modes, color, label, alpha=0.18):
    """Shade contiguous regions where mode ∈ allowed_modes."""
    start = None
    labeled = False
    for i in range(len(t)):
        in_seg = mode[i] in allowed_modes
        if in_seg and start is None:
            start = t[i]
        end_now = (not in_seg and start is not None)
        last_pt = (i == len(t)-1 and start is not None)
        if end_now or last_pt:
            end = t[i] if end_now else t[i]
            ax.axvspan(start, end, color=color, alpha=alpha,
                       label=(label if not labeled else None))
            labeled = True
            start = None

# ---------------- Plot 1: v_cmd with STOP & AVOID shading ----------------
fig1, ax1 = plt.subplots()
ax1.plot(t, v_cmd, linewidth=1.8, label="v_cmd")
ax1.axhline(1.35, linestyle="--", linewidth=1.2, label="walking cap 1.35 m/s")

# Shade STOP in red; any AVOID (left/right) in yellow
shade_span(ax1, t, mode, {1},  "red",    "STOP")
shade_span(ax1, t, mode, {2,3},"yellow", "AVOID")

# Optional: vertical markers on mode changes
for i in range(1, len(t)):
    if mode[i] != mode[i-1]:
        ax1.axvline(t[i], linestyle=":", linewidth=1)

ax1.set_xlabel("time_s")
ax1.set_ylabel("v_cmd (m/s)")
ax1.set_title("Forward Speed vs Time (JSON replay)")
ax1.legend(loc="best")
fig1.tight_layout()
fig1.savefig("replay_v_cmd_shaded.png", dpi=220)

# ---------------- Plot 2: vL & vR with AVOID_RIGHT and AVOID_LEFT shading ----------------
fig2, ax2 = plt.subplots()
ax2.plot(t, vL, linewidth=1.8, label="Left wheel (vL)")
ax2.plot(t, vR, linewidth=1.8, label="Right wheel (vR)")

# Shade AVOID_RIGHT (mode == 3) in orange
shade_span(ax2, t, mode, {3}, "orange", "AVOID_RIGHT")
# Shade AVOID_LEFT (mode == 2) in green
shade_span(ax2, t, mode, {2}, "green", "AVOID_LEFT")

# Optional: vertical markers on mode changes
for i in range(1, len(t)):
    if mode[i] != mode[i-1]:
        ax2.axvline(t[i], linestyle=":", linewidth=1, alpha=0.6)

ax2.set_xlabel("time_s")
ax2.set_ylabel("wheel speed (m/s)")
ax2.set_title("Wheel Speeds vs Time (JSON replay)")
ax2.legend(loc="best")
fig2.tight_layout()
fig2.savefig("replay_wheels_avoid_modes.png", dpi=220)

print("Saved: replay_v_cmd_shaded.png and replay_wheels_avoid_modes.png")
