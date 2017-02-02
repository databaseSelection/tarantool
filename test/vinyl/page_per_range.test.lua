#!/usr/bin/env tarantool

env = require('test_run')
test_run = env.new()

page_size = 16 * 1024

space = box.schema.space.create('test', { engine = 'vinyl' })
pk = space:create_index('primary', { page_size = page_size, range_size = page_size })

test_run:cmd("setopt delimiter ';'")
l = string.rep('a', 16)
for i = 0, 9 do
    for j = 0, 3999 do
        space:replace({i * 4000 + j, l})
    end
    box.snapshot()
end
test_run:cmd("setopt delimiter ''");
#space:select() 

k = 0
for _, t in space:select() do k = k + t[1] end
k

test_run:cmd("setopt delimiter ';'")
k = 0;
for i = 0, 19 do
    t = space:get(i * 1500 + 45)
    k = k + t[1]
end;
test_run:cmd("setopt delimiter ''");
k

