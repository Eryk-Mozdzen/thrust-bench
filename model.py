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

[kf], _, kf_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_thrust"]
)
[kt], _, kt_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_torque"]
)

print(f"kf = {kf:e}")
print(f"kt = {kt:e}")

for f_target in [5, 10, 15, 20]:
    print(
        f"{f_target:5.0f} N | {np.sqrt(f_target / kf):5.0f} rad/s | {np.abs(kt * (f_target / kf) ** 1.5):5.0f} W"
    )

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
plt.ylabel("propeller thrust [N]")
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
plt.ylabel("propeller torque [Nm]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

air_density = 1.2041  # TODO: calculate from temperature
rotor_radius = (0.5 * float(sys.argv[2])) * 0.0254
rotor_field = np.pi * (rotor_radius**2)

df["power_electrical"] = df["true_voltage"].abs() * df["true_current"].abs()
df["power_mechanical"] = df["true_torque"].abs() * df["true_velocity"].abs()
df["power_induced"] = df["true_thrust"].abs() * np.sqrt(
    df["true_thrust"].abs() / (2 * air_density * rotor_field)
)
df["drive_efficiency"] = df["power_mechanical"] / df["power_electrical"]
df["propeller_efficiency"] = df["power_induced"] / df["power_mechanical"]

eff_poly, _, eff_keep = curve_fit(
    lambda x, p1, p2, p3: x * np.poly1d([p1, p2, p3])(x),
    df["true_velocity"],
    df["drive_efficiency"],
)
eff_poly = np.poly1d([*eff_poly, 0])

eff_max_w = (
    eff_poly.deriv()
    .roots[
        np.isreal(eff_poly.deriv().roots)
        & (eff_poly.deriv(2)(eff_poly.deriv().roots).real < 0)
    ]
    .real
)

plt.figure()
plt.plot(w, 100 * eff_poly(w), c="red", label="model")
for ww in eff_max_w:
    print(f"    drive efficiency {100*eff_poly(ww):.0f}% (peak) for {ww:.0f} rad/s")
    plt.axvline(x=ww, color="red", linestyle="dashed")
    plt.axhline(y=100 * eff_poly(ww), color="red", linestyle="dashed", label="peak")
plt.scatter(
    df["true_velocity"].iloc[eff_keep],
    100 * df["drive_efficiency"].iloc[eff_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.scatter(
    df["true_velocity"].iloc[~eff_keep],
    100 * df["drive_efficiency"].iloc[~eff_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("drive efficiency [%]")
plt.xlim([0, None])
plt.ylim([0, 100])
plt.grid()
plt.legend()

[eff], _, eff_keep = curve_fit(
    lambda x, eta: eta,
    df["true_velocity"],
    df["propeller_efficiency"],
)
print(f"propeller efficiency {100*eff:.0f}%")
plt.figure()
plt.plot(w, 100 * eff * np.ones_like(w), c="red", label="model")
plt.scatter(
    df["true_velocity"].iloc[eff_keep],
    100 * df["propeller_efficiency"].iloc[eff_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.scatter(
    df["true_velocity"].iloc[~eff_keep],
    100 * df["propeller_efficiency"].iloc[~eff_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("propeller efficiency [%]")
plt.xlim([0, None])
plt.ylim([0, 100])
plt.grid()
plt.legend()

[torque_drag], _, torque_keep = curve_fit(
    lambda x, b: x + b, df["setpoint_torque"], df["true_torque"]
)
torque = np.linspace(df["setpoint_torque"].min(), df["setpoint_torque"].max(), 100)

plt.figure()
plt.plot(torque, torque + torque_drag, label="model", c="red")
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
