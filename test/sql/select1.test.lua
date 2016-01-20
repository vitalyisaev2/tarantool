box.schema.space.create('test1')
box.schema.space.create('test2')

box.space.test1:create_index('primary', {type = 'TREE', parts = {1, 'NUM'}})
box.space.test2:create_index('primary', {type = 'TREE', parts = {1, 'NUM'}})

format = {}
format[1] = {name='f1', type='num'}
format[2] = {name='f2', type='num'}

box.space.test1:format(format)

format = {}
format[1] = {name='r1', type='num'}
format[2] = {name='r2', type='num'}

box.space.test2:format(format)

box.space.test1:insert({11, 22})
box.space.test2:insert({1, 2.2})

r = require('sql')
c = r.connect('my_table')

c:execute('SELECT * FROM test232') --no such table
c:execute('SELECT * FROM test1, test232') --no such table
c:execute('SELECT * FROM test232, test1') --no such table

c:execute('SELECT f1 FROM test1')
c:execute('SELECT f2 FROM test1')
c:execute('SELECT f2, f1 FROM test1')
c:execute('SELECT f1, f2 FROM test1')
c:execute('SELECT * FROM test1')
c:execute('SELECT max(f1, f2), min(f1, f2) FROM test1')
c:execute('SELECT "one", "two", f1 FROM test1')

c:execute('SELECT * FROM test1, test2')
c:execute('SELECT test1.f1, test2.r1 FROM test1, test2');
c:execute('SELECT * FROM test1 AS a, test1 AS b');

box.space.test1:drop()
box.space.test2:drop()

return 1