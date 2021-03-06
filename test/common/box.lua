#!/usr/bin/env tarantool
local os = require('os')
local fiber = require('fiber')
local console = require('console')

-- Don't bind tarantool to an IPv6 port, because tests are unable
-- to resolve 'localhost' to ::1 or unable to connect to an IPv6
-- port: don't sure what exactly going in a wrong way.
--
-- XXX: This should be fixed with gh-121.
local listen = os.getenv('LISTEN')
if listen:find(':') then
    local host, port = unpack(listen:split(':'))
    if host == 'localhost' and tonumber(port) ~= nil then
        listen = ('127.0.0.1:%d'):format(port)
    end
end
box.cfg{
   listen = listen,
}

console.listen(os.getenv('ADMIN'))

lp = {
   test = 'test',
   test_empty = '',
   test_big = string.rep('1234567890', 6)
}

box.once('init', function()
    for k, v in pairs(lp) do
        box.schema.user.create(k, { password = v })
        if k == 'test' then
            -- Read and write are needed due to Tarantool
            -- 1.7.6-27-g7ef5be2 in CI and
            -- https://github.com/tarantool/tarantool/issues/3017
            -- Create grant is needed to create a table with tnt_execute().
            box.schema.user.grant('test', 'read,write,execute,create',
                                  'universe')
        end
    end

    local test = box.schema.space.create('test')
    test:create_index('primary',   {type = 'TREE', unique = true, parts = {1, 'unsigned'}})
    test:create_index('secondary', {type = 'TREE', unique = false, parts = {2, 'unsigned', 3, 'string'}})
    box.schema.user.grant('test', 'read,write', 'space', 'test')

    local msgpack = box.schema.space.create('msgpack')
    msgpack:create_index('primary', {parts = {1, 'unsigned'}})
    box.schema.user.grant('test', 'read,write', 'space', 'msgpack')
    msgpack:insert{1, 'float as key', {[2.7] = {1, 2, 3}}}
    msgpack:insert{2, 'array as key', {[{2, 7}] = {1, 2, 3}}}
    msgpack:insert{3, 'array with float key as key', {[{[2.7] = 3, [7] = 7}] = {1, 2, 3}}}
    msgpack:insert{6, 'array with string key as key', {['megusta'] = {1, 2, 3}}}

    box.schema.func.create('test_4')
    box.schema.user.grant('guest', 'execute', 'function', 'test_4');
end)

function test_1()
    require('log').error('1')
    return true, {
        c = {
            ['106'] = {1, 1428578535},
            ['2']   = {1, 1428578535}
        },
        pc = {
            ['106'] = {1, 1428578535, 9243},
            ['2']   = {1, 1428578535, 9243}
        },
        s = {1, 1428578535},
        u = 1428578535,
        v = {}
    }, true
end

function test_2()
    return { k2 = 'v', k1 = 'v2'}
end

function test_3(x, y)
    return x + y
end

function test_4()
    return box.session.user()
end

function test_5()
    if box.session.push == nil then
        return false
    end
    for i = 1, 10 do
        box.session.push({ position = i, value = 'i love maccartney' })
    end
    return true
end
