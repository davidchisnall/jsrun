var libc = require('libc_js');

function visit(dir, func)
{
	var d = libc.opendir(dir);
	if (!d)
	{
		return;
	}
	var file = libc.readdir(d)
	while (file)
	{
		// This is a bit ugly.  The field is a fixed-length array, so the FFI
		// generator copies back everything, including the bytes after the
		// trailing NULL.  We could possibly construct a string from any
		// fixed-length char array and attach the raw byte array as a property
		// (note: we really should be using TypedArray (once it exists) for all
		// arrays of primitive types)
		var fname = '';
		var len = file.d_name.length;
		for (var i=0 ; i<len ; i++)
		{
			var c = file.d_name[i];
			if (c == 0)
				break;
			fname = fname + String.fromCharCode(c);
		}
		//print(new String(file.d_name));
//print(Duktape.enc('jc', file, null, 2));
		file = libc.readdir(d)
		if (fname == '.' || fname == '..')
			continue;
		func(dir + '/' + fname);
	}
	libc.closedir(d);
}


function recursive_visit(filename, callback)
{
	var sb = new Object();
	libc.stat(filename, sb);
	if (sb.st_mode & 0040000)
	{
		visit(filename, function(filename) { recursive_visit(filename, callback); });
	}
	else
	{
		callback(filename);
	}
}


function handlepath(path)
{
	postMessage(path);
}

function onMessage(dirname)
{
	recursive_visit(dirname, handlepath);
	postMessage(0);
}
