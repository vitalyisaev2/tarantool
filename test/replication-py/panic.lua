#!/usr/bin/env tarantool
os = require('os')
box.cfg({
    listen              = os.getenv("LISTEN"),
    slab_alloc_arena    = 0.1,
    panic_on_wal_error  = false,
})

require('console').listen(os.getenv('ADMIN'))
