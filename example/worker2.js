onMessage = function(msg) {
	print(msg, 'received by the nested worker.');
	postMessage('finished');
};

print("nested worker started");
