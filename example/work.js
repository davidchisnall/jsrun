var worker = new Worker("worker.js");
worker.onMessage = function(msg) {
	print('received from worker:', msg);
};
worker.postMessage("Message!", "fish!");
worker = null;

