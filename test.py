#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "asyncio",
#     "aiomqtt",
#     "pyjson",
#     "matplotlib",
# ]
# ///

import asyncio
import aiomqtt
import json
import struct
import time
import matplotlib.pyplot as plt


class Focus:
    def __init__(self):
        pass

    async def run(self, queue):
        async with aiomqtt.Client("localhost") as mqtt:
            await mqtt.subscribe("focus/state")
            async for message in mqtt.messages:
                state = json.loads(message.payload.decode())

                await queue.put(("focus", time.time(), state))


class Bench:
    def __init__(self):
        pass

    async def run(self, queue):
        async with aiomqtt.Client("localhost") as mqtt:
            await mqtt.subscribe("data")
            async for message in mqtt.messages:
                thrust, torque, velocity, temperature, voltage, current = struct.unpack(
                    "<6f", message.payload
                )

                await queue.put(
                    (
                        "bench",
                        time.time(),
                        {
                            "thrust": thrust,
                            "torque": torque,
                            "velocity": velocity,
                            "temperature": temperature,
                            "voltage": voltage,
                            "current": current,
                        },
                    )
                )


class Plotter:
    def __init__(self, signals):
        plt.ion()

        self.signals = signals
        self.values = {}

        self.fig, self.ax = plt.subplots()

        if isinstance(signals, tuple):
            self.values[signals[0]] = []
            self.values[signals[1]] = []
            self.scatter = self.ax.scatter([], [])
            self.ax.set_xlabel(signals[0])
            self.ax.set_ylabel(signals[1])
        else:
            self.lines = []
            for s in signals:
                self.values[s] = []
                (line,) = self.ax.plot([], [], label=s)
                self.lines.append(line)
            self.ax.set_xlabel("time [s]")

        self.ax.legend()
        self.ax.grid()

        plt.show()

    async def run(self, queue):
        while True:
            source, timestamp, data = await queue.get()

            if source == "bench":
                if hasattr(self, "lines"):
                    for s in self.signals:
                        self.values[s].append((timestamp, data[s]))

                if hasattr(self, "scatter"):
                    self.values[self.signals[0]].append(data[self.signals[0]])
                    self.values[self.signals[1]].append(data[self.signals[1]])

                for key, value in self.values.items():
                    self.values[key] = value[-200:]

                if hasattr(self, "lines"):
                    for line, signal in zip(self.lines, self.signals):
                        t0 = self.values[signal][0][0]
                        x = [t - t0 for t, _ in self.values[signal]]
                        y = [y for _, y in self.values[signal]]
                        line.set_data(x, y)

                if hasattr(self, "scatter") and (len(self.values) > 0):
                    x = [x for x in self.values[self.signals[0]]]
                    y = [y for y in self.values[self.signals[1]]]
                    self.scatter.set_offsets(list(zip(x, y)))

                self.ax.relim()
                self.ax.autoscale_view()
                self.fig.canvas.draw()
                self.fig.canvas.flush_events()


async def main():
    queue = asyncio.Queue()

    bench = Bench()
    focus = Focus()
    plotter1 = Plotter(["thrust", "torque"])
    plotter2 = Plotter(("velocity", "thrust"))

    await asyncio.gather(
        bench.run(queue),
        focus.run(queue),
        plotter1.run(queue),
        plotter2.run(queue),
    )


if __name__ == "__main__":
    asyncio.run(main())
