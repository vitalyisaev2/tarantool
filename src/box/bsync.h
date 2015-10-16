#ifndef TARANTOOL_BOX_BSYNC_H_INCLUDED
#define TARANTOOL_BOX_BSYNC_H_INCLUDED
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdbool.h>

#include "queue.h"
#include "fiber.h"
#include "vclock.h"
#include "recovery.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

void bsync_init(struct recovery *r);
void bsync_start(struct recovery *r, int rows_per_wal);
int bsync_write(struct recovery *r, struct txn *txn, struct wal_request *req);
void bsync_writer_stop(struct recovery *r);

bool bsync_push_connection(int remote_id);
void bsync_push_localhost(int remote_id);

void bsync_bootstrap(struct recovery *r);

/*
 * Return id of server who will send snapshot
 */
int bsync_join();

/*
 * Return id of server who will send xlogs
 */
int bsync_subscribe();
int bsync_replica_stop();

bool bsync_process_join(int fd, struct tt_uuid *uuid,
			void (*on_join)(const struct tt_uuid *));

bool bsync_process_subscribe(int fd, struct tt_uuid *uuid,
			struct recovery *state);

bool bsync_follow(struct recovery *r);

void bsync_replication_fail(struct recovery *r);

void bsync_replica_fail();

void bsync_commit_local(uint32_t server_id, uint64_t lsn);

struct lua_State;
int
lbox_info_bsync(struct lua_State *L);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_BSYNC_H_INCLUDED */