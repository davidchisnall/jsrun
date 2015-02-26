#!../jsrun 
function WorkerPool(count, workerFile)
{
	var pool = this;
	this.workers = new Array();
	this.queue = new Array();
	this.onMessage = function() {};
	this.onCompletion = function() {};
	for (var i=0 ; i<count ; i++)
	{
		var w = new Worker(workerFile);
		this.workers.push(w);
		w.onMessage = function(msg)
		{
			pool.onMessage(msg);
			var m = pool.queue.shift();
			if (m == undefined)
			{
				pool.workers.push(this);
				if (pool.workers.length == count)
				{
					pool.onCompletion();
				}
			}
			else
			{
				this.postMessage(m);
			}
		};
	}
	this.postMessage = function(msg)
	{
		var w = pool.workers.shift();
		if (w == undefined)
		{
			pool.queue.push(msg);
			return false;
		}
		w.postMessage(msg);
		return true;
	};
	Duktape.fin(this, function() { print("Worker pool finalized"); });
}

dirname = '.';
if (program_arguments.length > 0)
{
	dirname = program_arguments[0];
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

var pool = new WorkerPool(8, 'sha_worker.js');
pool.onMessage = function(msg)
{
	print(msg);
}

var lsR = new Worker("find_worker.js");
lsR.onMessage = function(path)
{
	if (path != 0)
	{
		pool.postMessage(path);
	}
	else
	{
		pool.onCompletion = function()
		{
			print('done!');
		}
	}

}
lsR.postMessage(dirname);


