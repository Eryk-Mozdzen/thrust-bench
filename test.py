#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "asyncio",
#     "aiomqtt",
#     "msgpack",
#     "datetime",
#     "PyQt6",
#     "pyqtgraph",
#     "qasync",
# ]
# ///

import asyncio
import aiomqtt
import msgpack
import struct
import datetime
import numpy as np
import pyqtgraph as pg
from PyQt6.QtWidgets import QApplication
from qasync import QEventLoop
from collections import deque
import sys


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
                        datetime.datetime.now(datetime.UTC),
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
                        datetime.datetime.now(datetime.UTC),
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
    def __init__(
        self,
        signals,
        max_points=200,
        refresh_rate=10,
        autoscale=True,
    ):
        pg.setConfigOption("background", "w")
        pg.setConfigOption("foreground", "k")
        pg.setConfigOptions(antialias=True)
        self.signals = signals
        self.max_points = max_points
        self.refresh_period = 1.0 / refresh_rate
        self.autoscale = autoscale
        self.time_window = 10
        self.last_draw = 0
        self.start = datetime.datetime.now(datetime.UTC)
        self.win = pg.GraphicsLayoutWidget(show=True, title="Telemetry")
        self.plot = self.win.addPlot()
        self.values = {}
        if isinstance(signals, tuple):
            self.mode = "scatter"
            self.values[signals[0]] = deque(maxlen=max_points)
            self.values[signals[1]] = deque(maxlen=max_points)
            self.scatter = pg.ScatterPlotItem(size=6)
            self.plot.addItem(self.scatter)
            self.plot.setLabel("bottom", signals[0])
            self.plot.setLabel("left", signals[1])
        else:
            self.plot.addLegend()
            colors = [
                "#1f77b4",
                "#d62728",
                "#2ca02c",
                "#ff7f0e",
                "#9467bd",
                "#17becf",
            ]
            self.mode = "line"
            self.lines = []
            for i, signal in enumerate(signals):
                self.values[signal] = deque(maxlen=max_points)
                line = self.plot.plot(
                    pen=pg.mkPen(colors[i % len(colors)], width=2),
                    name=signal,
                )
                self.lines.append(line)
            self.plot.setLabel("bottom", "time [s]")
        self.plot.showGrid(x=True, y=True)
        if not autoscale:
            self.plot.enableAutoRange(False, False)

    def _timestamp_to_float(self, timestamp):
        if isinstance(timestamp, datetime.datetime):
            return (timestamp - self.start).total_seconds()
        return float(timestamp)

    async def run(self, queue):
        while True:
            timestamp, data = await queue.get()
            if self.mode == "line":
                t = self._timestamp_to_float(timestamp)
                for signal in self.signals:
                    value = data.get(signal)
                    if value is not None:
                        self.values[signal].append((t, value))
            else:
                x = data.get(self.signals[0])
                y = data.get(self.signals[1])
                if x is not None and y is not None:
                    self.values[self.signals[0]].append(x)
                    self.values[self.signals[1]].append(y)
            now = asyncio.get_event_loop().time()
            if now - self.last_draw < self.refresh_period:
                continue
            self.last_draw = now
            self.update()

    def update(self):
        if self.mode == "line":
            latest_time = None
            for line, signal in zip(self.lines, self.signals):
                points = self.values[signal]
                if not points:
                    continue
                arr = np.asarray(points, dtype=np.float64)
                line.setData(arr[:, 0], arr[:, 1])
                latest_time = arr[-1, 0]
            if latest_time is not None:
                self.plot.setXRange(
                    max(0, latest_time - self.time_window), latest_time, padding=0
                )
        else:
            x = np.asarray(self.values[self.signals[0]], dtype=np.float64)
            y = np.asarray(self.values[self.signals[1]], dtype=np.float64)
            if len(x):
                self.scatter.setData(x, y)


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
    app = QApplication(sys.argv)

    loop = QEventLoop(app)
    asyncio.set_event_loop(loop)

    with loop:
        loop.run_until_complete(main())
