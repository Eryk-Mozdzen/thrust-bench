const ids = [
	"thrust",
	"torque",
	"velocity",
	"temperature",
	"voltage",
	"current"
];

function update(d) {
	for(const id of ids) {
		if(d[id] !== undefined) {
			document.getElementById(id).textContent = d[id];
		}
	}
}

async function poll() {
	try {
		const r = await fetch("/data");
		const d = await r.json();
		update(d);
	} catch(e) {

	}
}

setInterval(poll, 100);

function sendCommand(cmd) {
	fetch("/command", {
		method: "POST",
		body: cmd
	});
}
