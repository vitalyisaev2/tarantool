#!/usr/bin/env tarantool

tarantool_path = io.popen("which tarantool", "r"):read()

require("top")

-- init space test1
if box.space.test1 then
    box.space.test1:drop()
end
box.schema.space.create('test1')
box.space.test1:create_index('primary', {parts={1, 'NUM'}, type='TREE'})
local format = {}
format[1] = {name='ID', type='num'}
box.space.test1:format(format)
for i = 1, 3 do
    box.space.test1:insert({i})
end

-- init space test2 
if box.space.test2 then
    box.space.test2:drop()
end
box.schema.space.create('test2')
box.space.test2:create_index('primary', {parts={1, 'NUM'}, type='TREE'})
local format = {}
format[1] = {name='ID', type='num'}
box.space.test2:format(format)
for i = 1, 3 do
    box.space.test2:insert({i})
end

-- initialization params
local arg0 = ffi.cast('char *',  tarantool_path)
local arg1 = ffi.cast('char *', "select1.test")
local argv = ffi.new('char *[2]')
argv[0] = arg0
argv[1] = arg1

-- run main
fixture.main(2, argv)

os.exit(0)
