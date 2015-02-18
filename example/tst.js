var clib = require("test");
var jslib = require("tests");

jslib.test("foo");

var sb = new Object();
clib.stat("tst.js", sb);
clib.puts("stat buffer:");
clib.puts(Duktape.enc('jc', sb, null, 2));

print('Anonymous enum charlie:', clib.charlie);
print('Enum example, value one:', clib.example.one);

var f = clib.fopen("example.out", "w");
clib.fwrite("some text", 9, 1, f);
var b = Duktape.Buffer(4);
b[0] = 65;
b[1] = 66;
b[2] = 67;
b[3] = 68;
clib.fwrite(b, 4, 1, f);
clib.fclose(f);
