var libc = require("test");
exports.stat = function(file) {
	var obj = new Object;
	libc.stat(file, obj);
	return obj;
}
