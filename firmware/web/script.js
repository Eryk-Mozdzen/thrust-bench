const ws = new WebSocket("ws://" + location.host + ":81/data");

ws.binaryType = "arraybuffer";

ws.onerror = (e) => {
    console.error(e);
};

ws.onmessage = (event) => {
    const ids = [
        "thrust",
        "torque",
        "velocity",
        "temperature",
        "voltage",
        "current"
    ];

    const view = new DataView(event.data);

    for(let i = 0; i < ids.length; i++) {
        const value = view.getInt16(i * 2, true) / 10.0;
        document.getElementById(ids[i]).textContent = value.toFixed(1);
    }
};

function sendCommand(cmd) {
    if(ws.readyState === WebSocket.OPEN) {
        ws.send(cmd);
	}
}
