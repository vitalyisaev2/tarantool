#ifndef TARANTOOL_CBUS_H_INCLUDED
#define TARANTOOL_CBUS_H_INCLUDED
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
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
#include "fiber.h"
#include "salad/stailq.h"
#include "rmean.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** cbus, cmsg - inter-cord bus and messaging */

struct cmsg;
struct cpipe;
typedef void (*cmsg_f)(struct cmsg *);

enum cbus_stat_name {
	CBUS_STAT_EVENTS,
	CBUS_STAT_LOCKS,
	CBUS_STAT_LAST,
};

extern const char *cbus_stat_strings[CBUS_STAT_LAST];

/**
 * One hop in a message travel route.  A message may need to be
 * delivered to many destinations before it can be dispensed with.
 * For example, it may be necessary to return a message to the
 * sender just to destroy it.
 *
 * Message travel route is an array of cmsg_hop entries. The first
 * entry contains a delivery function at the first destination,
 * and the next destination. Subsequent entries are alike. The
 * last entry has a delivery function (most often a message
 * destructor) and NULL for the next destination.
 */
struct cmsg_hop {
	/** The message delivery function. */
	cmsg_f f;
	/**
	 * The next destination to which the message
	 * should be routed after its delivered locally.
	 */
	struct cpipe *pipe;
};

/** A message traveling between cords. */
struct cmsg {
	/**
	 * A member of the linked list - fifo of the pipe the
	 * message is stuck in currently, waiting to get
	 * delivered.
	 */
	struct stailq_entry fifo;
	/** The message routing path. */
	struct cmsg_hop *route;
	/** The current hop the message is at. */
	struct cmsg_hop *hop;
};

static inline void
cmsg_init(struct cmsg *msg, struct cmsg_hop *route)
{
	/**
	 * The first hop can be done explicitly with cbus_push(),
	 * msg->hop thus points to the second hop.
	 */
	msg->hop = msg->route = route;
}

struct cpipe;

/**
 * A pool of worker fibers to handle messages,
 * so that each message is handled in its own fiber.
 */
struct fiber_pool {
	/** Cache of fibers which work on incoming messages. */
	struct rlist idle;
	/** The number of fibers in the pool. */
	int size;
	/** The limit on the number of fibers working on tasks. */
	int max_size;
	/**
	 * Fibers in leave the pool if they have nothing to do
	 * for longer than this.
	 */
	float idle_timeout;
	/** Staged messages (for fibers to work on) */
	struct stailq output;
};

/**
 * Initialize a fiber pool and connect it to a pipe. Currently
 * must be done before the pipe is actively used by a bus.
 */
void
fiber_pool_create(struct fiber_pool *pool, int max_pool_size,
		  float idle_timeout);


#define CACHELINE_SIZE 64
/** A  uni-directional FIFO queue from one cord to another. */
struct cpipe {
	/**
	 * The protected part of the pipe: only accessible
	 * in a critical section.
	 * The message flow in the pipe is:
	 *     input       <-- owned by the consumer thread
	 *       v
	 *     pipe        <-- shared, protected by a mutex
	 *       v
	 *     pool.output      <-- owned by the producer thread
	 */
	struct {
		struct stailq pipe;
		/** Peer pipe - the other direction of the bus. */
		struct cpipe *peer;
		struct cbus *bus;
	} __attribute__((aligned(CACHELINE_SIZE)));
	/** Stuff most actively used in the producer thread. */
	struct {
		/** Staging area for pushed messages */
		struct stailq input;
		/** Counters are useful for finer-grained scheduling. */
		int n_input;
		/**
		 * When pushing messages, keep the staged input size under
		 * this limit (speeds up message delivery and reduces
		 * latency, while still keeping the bus mutex cold enough).
		*/
		int max_input;
		/**
		 * Rather than flushing input into the pipe
		 * whenever a single message or a batch is
		 * complete, do it once per event loop iteration.
		 */
		struct ev_async flush_input;
		/** The producer thread. */
		struct ev_loop *producer;
		/** The consumer thread. */
		struct ev_loop *consumer;
	} __attribute__((aligned(CACHELINE_SIZE)));
	/** Stuff related to the consumer. */
	struct {
		/**
		 * Used to trigger task processing when
		 * the pipe becomes non-empty.
		 */
		struct ev_async fetch_output;
		struct fiber_pool *pool;
	} __attribute__((aligned(CACHELINE_SIZE)));
};

#undef CACHELINE_SIZE

/**
 * Initialize a pipe. Must be called by the consumer.
 */
void
cpipe_create(struct cpipe *pipe);

void
cpipe_destroy(struct cpipe *pipe);

/**
 * Set pipe max size of staged push area. The default is infinity.
 * If staged push cap is set, the pushed messages are flushed
 * whenever the area has more messages than the cap, and also once
 * per event loop.
 * Otherwise, the messages flushed once per event loop iteration.
 *
 * @todo: collect bus stats per second and adjust max_input once
 * a second to keep the mutex cold regardless of the message load,
 * while still keeping the latency low if there are few
 * long-to-process messages.
 */
static inline void
cpipe_set_max_input(struct cpipe *pipe, int max_input)
{
	pipe->max_input = max_input;
}

/**
 * Flush all staged messages into the pipe and eventually to the
 * consumer.
 */
static inline void
cpipe_flush_input(struct cpipe *pipe)
{
	assert(loop() == pipe->producer);

	/** Flush may be called with no input. */
	if (pipe->n_input > 0) {
		if (pipe->n_input < pipe->max_input) {
			/*
			 * Not much input, can deliver all
			 * messages at the end of the event loop
			 * iteration.
			 */
			ev_feed_event(pipe->producer,
				      &pipe->flush_input, EV_CUSTOM);
		} else {
			/*
			 * Wow, it's a lot of stuff piled up,
			 * deliver immediately.
			 */
			ev_invoke(pipe->producer,
				  &pipe->flush_input, EV_CUSTOM);
		}
	}
}

/**
 * Push a single message to the pipe input. The message is pushed
 * to a staging area. To be delivered, the input needs to be
 * flushed with cpipe_flush_input().
 */
static inline void
cpipe_push_input(struct cpipe *pipe, struct cmsg *msg)
{
	assert(loop() == pipe->producer);

	stailq_add_tail_entry(&pipe->input, msg, fifo);
	pipe->n_input++;
	if (pipe->n_input >= pipe->max_input)
		ev_invoke(pipe->producer, &pipe->flush_input, EV_CUSTOM);
}

/**
 * Push a single message and ensure it's delivered.
 * A combo of push_input + flush_input for cases when
 * it's not known at all whether there'll be other
 * messages coming up.
 */
static inline void
cpipe_push(struct cpipe *pipe, struct cmsg *msg)
{
	cpipe_push_input(pipe, msg);
	assert(pipe->n_input < pipe->max_input);
	if (pipe->n_input == 1)
		ev_feed_event(pipe->producer, &pipe->flush_input, EV_CUSTOM);
}

/**
 * Cord interconnect: two pipes one for each message flow
 * direction.
 */
struct cbus {
	/** Two pipes for two directions between two cords. */
	struct cpipe *pipe[2];
	/** cbus statistics */
	struct rmean *stats;
	/**
	 * A single mutex to protect all exchanges around the
	 * two pipes involved in the bus.
	 */
	pthread_mutex_t mutex;
	/** Condition for synchronized start of the bus. */
	pthread_cond_t cond;
};

void
cbus_create(struct cbus *bus);

void
cbus_destroy(struct cbus *bus);

/**
 * Connect the pipes: join cord1 input to the cord2 output,
 * and cord1 output to cord2 input.
 * Each cord must invoke this method in its own scope,
 * and provide its own callback to handle incoming messages
 * (pop_output_cb).
 * This call synchronizes two threads, and after this method
 * returns, cpipe_push/cpipe_pop are safe to use.
 *
 * @param  bus the bus
 * @param  pipe the pipe for which this thread is a consumer
 *
 * @retval the pipe for this this thread is a producer
 *
 * @example:
 *	cpipe_create(&in);
 *	struct cpipe *out = cbus_join(bus, &in);
 *	cpipe_set_max_input(out, 128);
 *	cpipe_push(out, msg);
 */
struct cpipe *
cbus_join(struct cbus *bus, struct cpipe *pipe);

/**
 * Stop listening for events on the bus.
 */
void
cbus_leave(struct cbus *bus);

/**
 * A helper message to wakeup caller whenever an event
 * occurs.
 */
struct cmsg_notify
{
	struct cmsg base;
	struct fiber *fiber;
};

void
cmsg_notify_init(struct cmsg_notify *msg);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_CBUS_H_INCLUDED */
