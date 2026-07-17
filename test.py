#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "asyncio",
#     "aiomqtt",
#     "pyjson",
#     "datetime",
#     "matplotlib",
# ]
# ///

import asyncio
import aiomqtt
import json
import struct
import datetime
import matplotlib.pyplot as plt


class Queue(asyncio.Queue):
    def __init__(self):
        super().__init__()

        self.values = {}

        self.values["thrust"] = None
        self.values["torque"] = None
        self.values["velocity"] = None
        self.values["temperature"] = None
        self.values["voltage"] = None
        self.values["current"] = None

    async def put(self, item):
        _, timestamp, data = item

        for key, value in data.items():
            self.values[key] = value

        await super().put((timestamp, self.values))


class Focus:
    def __init__(self):
        pass

    async def run(self, queue):
        async with aiomqtt.Client("localhost") as mqtt:
            await mqtt.subscribe("focus/state")
            async for message in mqtt.messages:
                state = json.loads(message.payload.decode())

                await queue.put(("focus", datetime.datetime.now(), state))


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
                        datetime.datetime.now(),
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
            self.begin = datetime.datetime.now()
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
            timestamp, data = await queue.get()

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
                    x = [
                        (t - self.begin).total_seconds() for t, _ in self.values[signal]
                    ]
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


class Writer:
    def __init__(self, filepath):
        self.file = open(filepath, "w")

    async def run(self, queue):
        header = True

        while True:
            timestamp, data = await queue.get()

            if header:
                header = False
                self.file.write("timestamp,")
                for key in data.keys():
                    self.file.write(f"{key},")
                self.file.write("\n")

            self.file.write(f"{timestamp},")
            for _, value in data.items():
                self.file.write(f"{value},")
            self.file.write("\n")

            self.file.flush()


async def main():
    queue = Queue()

    bench = Bench()
    focus = Focus()
    plotter1 = Plotter(["thrust", "torque"])
    plotter2 = Plotter(("velocity", "thrust"))
    writer = Writer(
        f"recording_{datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d_%H:%M:%S")}.csv"
    )

    await asyncio.gather(
        bench.run(queue),
        focus.run(queue),
        plotter1.run(queue),
        plotter2.run(queue),
        writer.run(queue),
    )


if __name__ == "__main__":
    asyncio.run(main())
