function onMessage(msg) {
	print('message received, starting loop on closing');
	postMessage("looping!");
	while (!closing) {};
	postMessage("terminated!");
}
