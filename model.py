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


def curve_fit(model, x_data, y_data, **kwargs):
    keep = np.ones(len(x_data), dtype=bool)

    while True:
        popt, pcov = scipy.optimize.curve_fit(model, x_data, y_data, **kwargs)

        residuals = y_data - model(x_data, *popt)
        sigma = residuals.std(ddof=1)

        mask = np.abs(residuals) <= (3 * sigma)

        if mask.all():
            break

        keep[np.where(keep)[0][~mask]] = False

        x_data = x_data[mask].copy()
        y_data = y_data[mask].copy()

    return popt, pcov, keep


df = pd.read_csv(sys.argv[1]).dropna()

w = np.linspace(0, max(df["true_velocity"]), 100)

kf, _, kf_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_thrust"]
)
kt, _, kt_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_torque"]
)

print(f"kf = {kf[0]:e}")
print(f"kt = {kt[0]:e}")

plt.figure()
plt.plot(w, kf * (w**2), label="model", c="red")
plt.scatter(
    df["true_velocity"].iloc[kf_keep],
    df["true_thrust"].iloc[kf_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["true_velocity"].iloc[~kf_keep],
    df["true_thrust"].iloc[~kf_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("thrust [N]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

plt.figure()
plt.plot(w, kt * (w**2), label="model", c="red")
plt.scatter(
    df["true_velocity"].iloc[kt_keep],
    df["true_torque"].iloc[kt_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["true_velocity"].iloc[~kt_keep],
    df["true_torque"].iloc[~kt_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("torque [Nm]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

df["power_electrical"] = df["true_voltage"] * df["true_current"]
df["power_mechanical"] = df["true_torque"] * df["true_velocity"]
df["efficiency"] = df["power_mechanical"] / df["power_electrical"]

print(
    f"peak efficiency {100*max(df["efficiency"]):.0f}% for {df.loc[df["efficiency"].idxmax(), "true_velocity"]:.0f} rad/s"
)

plt.figure()
plt.scatter(df["true_velocity"], 100 * df["efficiency"], c="black", s=1)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("efficiency [%]")
plt.ylim(0, 100 * df["efficiency"].max())
plt.grid()

torque_line, _, torque_keep = curve_fit(
    lambda x, a, b: (a * x) + b, df["setpoint_torque"], df["true_torque"]
)
a, b = torque_line
x = np.linspace(0, max(df["setpoint_torque"]), 100)
print(a, b)

plt.figure()
plt.plot(x, (a * x) + b, label="model", c="red")
plt.scatter(
    df["setpoint_torque"].iloc[torque_keep],
    df["true_torque"].iloc[torque_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["setpoint_torque"].iloc[~torque_keep],
    df["true_torque"].iloc[~torque_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("setpoint torque [Nm]")
plt.ylabel("true torque [Nm]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

plt.show()
