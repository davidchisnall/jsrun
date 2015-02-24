onMessage = function(msg) {
	print(msg, 'received by the worker.');
	postMessage('finished');
};

print("worker started");

var worker = new Worker("worker2.js");
worker.onMessage = function(msg) {
	print('received from nested worker:', msg);
};
worker.postMessage("Message!", "fish!");

worker = new Worker("worker2.js");
worker.onMessage = function(msg) {
	print('received from nested worker:', msg);
	postMessage('second worker finished');
};
worker.postMessage("Message!", "fish!");

print('closing: ', closing);

worker = null;
