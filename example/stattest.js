var s = require('stat');

print(Duktape.enc('jc', s.stat('tst.js'), null, 2));

