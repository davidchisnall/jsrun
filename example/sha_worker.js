var sha = require('shalib');
var libc = require('libc_js');

function onMessage(file)
{
	var sb = new Object();
	if (libc.stat(file, sb) != 0)
	{
		print('unable to stat', file);
		postMessage('');
		return 0;
	}
	var length = sb.st_size;
	var buffer_size = 1024*256;
	var buffer = Duktape.Buffer(buffer_size);
	var f = libc.fopen(file, "r");
	var shactx = new Object();
	sha.SHA1_Init(shactx);
	for (var i = 0 ; i<length ; )
	{
		var read = libc.fread(buffer, 1, buffer_size, f);
		if (read == 0)
		{
			break;
		}
		sha.SHA1_Update(shactx,buffer, read);
		i += read;
	}
	libc.fclose(f);
	var digest = Duktape.Buffer(20);
	for (var i=0 ; i<20 ; i++)
	{
		digest[i] = 0;
	}
	sha.SHA1_Final(digest, shactx);
	var reply = 'SHA1('+file+')= ' + Duktape.enc('hex', digest);
	postMessage(reply);
	return 0;
}
