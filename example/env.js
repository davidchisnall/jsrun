#!../jsrun
print('Environment:');
for (var key in environ)
{
	print(key, '=', environ[key]);
}
print('Arguments:');
for (var key in program_arguments)
{
	print(key, '=', program_arguments[key]);
}
