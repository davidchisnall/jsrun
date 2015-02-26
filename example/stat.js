var libc = require("libc_js");
exports.stat = function(file) {
	var obj = new Object;
	libc.stat(file, obj);
	return obj;
}
