box.schema.user.grant('guest', 'read,write,execute', 'universe')
net_box = require('net.box')
fiber = require('fiber')
LISTEN = require('uri').parse(box.cfg.listen)

-- create first space
s = box.schema.create_space('test')
i = s:create_index('primary')
cn = net_box:new(LISTEN.host, LISTEN.service)

-- check that schema is correct
cn.space.test ~= nil
old_schema_id = cn._schema_id

-- create one more space
s2 = box.schema.create_space('test2')
i2 = s2:create_index('primary')

----------------------------------
-- TEST #1 simple reload
----------------------------------
-- check that schema is not fresh
cn.space.test2 == nil
cn._schema_id == old_schema_id

-- exec request with reload
cn.space.test:select{}
cn._schema_id > old_schema_id

----------------------------------
-- TEST #2 parallel select/reload
----------------------------------
env = require('test_run')
test_run = env.new()

requests = 0
reloads = 0

test_run:cmd('setopt delimiter ";"')
function selector()
    while true do
        cn.space.test:select{}
        requests = requests + 1
        fiber.sleep(0.01)
    end
end

function reloader()
    while true do
        cn:reload_schema()
        reloads = reloads + 1
        fiber.sleep(0.001)
    end
end;
test_run:cmd('setopt delimiter ""');

request_fiber = fiber.create(selector)
reload_fiber = fiber.create(reloader)

-- Check that each fiber works
while requests < 10 or reloads < 10 do fiber.sleep(0.01) end
requests < reloads

-- cleanup
request_fiber:cancel()
reload_fiber:cancel()
s:drop()
s2:drop()
box.schema.user.revoke('guest', 'read,write,execute', 'universe')
