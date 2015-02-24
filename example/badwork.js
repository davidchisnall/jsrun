#!../jsrun
var w = new Worker('badworker.js');
w.onMessage = function(msg)
{
	print(msg, 'received from worker');
	w.terminate();
}
w.postMessage('loop');
print('exiting');
