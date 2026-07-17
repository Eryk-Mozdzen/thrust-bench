#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "asyncio",
#     "aiomqtt",
#     "msgpack",
#     "datetime",
#     "matplotlib",
# ]
# ///

import asyncio
import aiomqtt
import msgpack
import struct
import datetime
import matplotlib.pyplot as plt
from collections import deque


class Bus:
    def __init__(self):
        self.lock = asyncio.Lock()
        self.values = {
            "true_thrust": None,
            "true_torque": None,
            "true_velocity": None,
            "true_temperature": None,
            "true_voltage": None,
            "true_current": None,
            "focus_voltage": None,
            "focus_velocity": None,
        }
        self.subscribers = []

    def subscribe(self):
        q = asyncio.Queue()
        self.subscribers.append(q)
        return q

    async def put(self, item):
        timestamp, data = item

        async with self.lock:
            self.values.update(data)
            snapshot = self.values.copy()

        message = (timestamp, snapshot)

        for q in self.subscribers:
            await q.put(message)


class Focus:
    def __init__(self):
        pass

    async def run(self, queue):
        async with aiomqtt.Client("localhost") as client:
            await client.subscribe("focus/state")
            async for message in client.messages:
                state = msgpack.unpackb(message.payload)

                await queue.put(
                    (
                        datetime.datetime.now(),
                        {
                            "focus_voltage": state["supply"],
                            "focus_velocity": state["velocity"],
                        },
                    )
                )


class Bench:
    def __init__(self):
        pass

    async def run(self, queue):
        async with aiomqtt.Client("localhost") as client:
            await client.subscribe("data")
            async for message in client.messages:
                thrust, torque, velocity, temperature, voltage, current = struct.unpack(
                    "<6f", message.payload
                )

                await queue.put(
                    (
                        datetime.datetime.now(),
                        {
                            "true_thrust": thrust,
                            "true_torque": torque,
                            "true_velocity": velocity,
                            "true_temperature": temperature,
                            "true_voltage": voltage,
                            "true_current": current,
                        },
                    )
                )


class Plotter:
    def __init__(self, signals, max_points=100, refresh_rate=10):
        plt.ion()

        self.signals = signals
        self.max_points = max_points
        self.refresh_period = 1.0 / refresh_rate

        self.fig, self.ax = plt.subplots()

        self.last_draw = 0
        self.start = datetime.datetime.now()

        self.values = {}

        if isinstance(signals, tuple):
            self.mode = "scatter"

            self.values[signals[0]] = deque(maxlen=max_points)
            self.values[signals[1]] = deque(maxlen=max_points)

            self.artist = self.ax.scatter([], [])

            self.ax.set_xlabel(signals[0])
            self.ax.set_ylabel(signals[1])

        else:
            self.mode = "line"

            self.lines = []

            for s in signals:
                self.values[s] = deque(maxlen=max_points)

                (line,) = self.ax.plot([], [], label=s)
                self.lines.append(line)

            self.ax.legend()
            self.ax.set_xlabel("time [s]")

        self.ax.grid()
        plt.show(block=False)

    async def run(self, queue):
        while True:
            timestamp, data = await queue.get()

            if self.mode == "line":
                for s in self.signals:
                    self.values[s].append(
                        ((timestamp - self.start).total_seconds(), data[s])
                    )

            else:
                self.values[self.signals[0]].append(data[self.signals[0]])
                self.values[self.signals[1]].append(data[self.signals[1]])

            now = asyncio.get_event_loop().time()

            if now - self.last_draw < self.refresh_period:
                continue

            self.last_draw = now
            self.update()

    def update(self):
        if self.mode == "line":

            for line, signal in zip(self.lines, self.signals):
                points = self.values[signal]

                if points:
                    x, y = zip(*points)
                    line.set_data(x, y)

            self.ax.relim()
            self.ax.autoscale_view()

        else:
            x = self.values[self.signals[0]]
            y = self.values[self.signals[1]]

            self.artist.set_offsets(list(zip(x, y)))

        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()


class Recorder:
    def __init__(self):
        filepath = f"recording_{datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d_%H:%M:%S")}.csv"
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
    bus = Bus()

    focus = Focus()
    bench = Bench()
    plotter1 = Plotter(["true_velocity", "focus_velocity"])
    plotter2 = Plotter(["true_torque"])
    plotter3 = Plotter(("true_velocity", "true_thrust"))
    plotter4 = Plotter(("true_velocity", "true_torque"))
    recorder = Recorder()

    await asyncio.gather(
        focus.run(bus),
        bench.run(bus),
        plotter1.run(bus.subscribe()),
        plotter2.run(bus.subscribe()),
        plotter3.run(bus.subscribe()),
        plotter4.run(bus.subscribe()),
        recorder.run(bus.subscribe()),
    )


if __name__ == "__main__":
    asyncio.run(main())
