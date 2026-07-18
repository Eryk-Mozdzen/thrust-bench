#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "numpy",
#     "pandas",
#     "scipy",
#     "matplotlib",
# ]
# ///

import pandas as pd
import numpy as np
import scipy
import matplotlib.pyplot as plt
import sys

df = pd.read_csv(sys.argv[1]).dropna(how="all", axis="columns").dropna()

w = np.linspace(0, max(df["true_velocity"]), 100)

KF, _ = scipy.optimize.curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_thrust"]
)

KT, _ = scipy.optimize.curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_torque"]
)

df["power_electrical"] = df["true_voltage"] * df["true_current"]
df["power_mechanical"] = df["true_torque"] * df["true_velocity"]
df["efficiency"] = df["power_mechanical"] / df["power_electrical"]

print(f"Kf = {KF[0]:e}")
print(f"Kt = {KT[0]:e}")
print(
    f"peak efficiency {100*max(df["efficiency"]):.0f}% for {df.loc[df["efficiency"].idxmax(), "true_velocity"]:.0f} rad/s"
)

plt.figure()
plt.scatter(df["true_velocity"], df["true_thrust"], label="samples", c="black", s=1)
plt.plot(w, KF * (w**2), label="model", c="red")
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("thrust [N]")
plt.grid()
plt.legend()

plt.figure()
plt.scatter(df["true_velocity"], df["true_torque"], label="samples", c="black", s=1)
plt.plot(w, KT * (w**2), label="model", c="red")
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("torque [Nm]")
plt.grid()
plt.legend()

plt.figure()
plt.scatter(df["true_velocity"], 100 * df["efficiency"], label="efficiency", s=1)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("efficiency [%]")
plt.grid()
plt.legend()

plt.show()
