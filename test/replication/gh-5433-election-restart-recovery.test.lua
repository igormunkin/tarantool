test_run = require('test_run').new()
box.schema.user.grant('guest', 'super')

old_election_mode = box.cfg.election_mode
old_replication_timeout = box.cfg.replication_timeout

test_run:cmd('create server replica with rpl_master=default,\
              script="replication/replica.lua"')
test_run:cmd('start server replica with wait=True, wait_load=True')

--
-- gh-5433: leader, when elected, should resend rows to replicas starting from
-- the last acked vclock. Because they ignored all what was sent afterwards as
-- it didn't come from a leader.
--
-- There was a bug, that the local LSN wasn't passed to the restarted recovery
-- iterator, and it tried to find not existing files. This caused a reconnect
-- and a scary error message.
--

-- Increase local LSN so as without it being accounted during recovery cursor
-- restart it would fail, as it won't find the xlogs with first local LSN. Do
-- snapshots to mark the old files for drop.
s = box.schema.create_space('test', {is_local = true})
_ = s:create_index('pk')
for i = 1, 10 do s:replace({i}) end
box.snapshot()
for i = 11, 20 do s:replace({i}) end
box.snapshot()

-- The log may contain old irrelevant records. Add some garbage and in the next
-- lines check for not more than the added garbage. So the old text won't be
-- touched.
require('log').info(string.rep('a', 1000))

-- Small timeout to speed up the election.
box.cfg{                                                                        \
    replication_timeout = 0.1,                                                  \
    election_mode = 'candidate',                                                \
}
test_run:wait_cond(function() return box.info.election.state == 'leader' end)

test_run:switch('replica')
test_run:wait_cond(function() return box.info.election.leader ~= 0 end)

test_run:switch('default')
-- When leader was elected, it should have restarted the recovery iterator in
-- the relay. And should have accounted the local LSN. Otherwise will see the
-- XlogGapError.
assert(not test_run:grep_log('default', 'XlogGapError', 1000))
s:drop()

test_run:cmd('stop server replica')
test_run:cmd('delete server replica')
box.cfg{                                                                        \
    election_mode = old_election_mode,                                          \
    replication_timeout = old_replication_timeout,                              \
}
box.schema.user.revoke('guest', 'super')
