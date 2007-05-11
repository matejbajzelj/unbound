/*
 * util/module.h - DNS handling module interface
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the interface for DNS handling modules.
 */

#ifndef UTIL_MODULE_H
#define UTIL_MODULE_H
#include "util/storage/lruhash.h"
#include "util/data/msgreply.h"
#include "util/data/msgparse.h"
struct alloc_cache;
struct config_file;
struct slabhash;
struct query_info;
struct edns_data;
struct region;
struct worker;
struct module_qstate;

/** Maximum number of modules in operation */
#define MAX_MODULE 2

/**
 * Module environment.
 * Services and data provided to the module.
 */
struct module_env {
	/* --- data --- */
	/** config file with config options */
	struct config_file* cfg;
	/** shared message cache */
	struct slabhash* msg_cache;
	/** shared rrset cache */
	struct slabhash* rrset_cache;

	/* --- services --- */
	/** 
	 * Send DNS query to server. operate() should return with wait_reply.
	 * Later on a callback will cause operate() to be called with event
	 * timeout or reply.
	 * @param pkt: packet to send.
	 * @param addr: where to.
	 * @param addrlen: length of addr.
	 * @param timeout: seconds to wait until timeout.
	 * @param q: wich query state to reactivate upon return.
	 * @param use_tcp: set to true to send over TCP. 0 for UDP.
	 * return: false on failure (memory or socket related). no query was
	 *	sent.
	 */
	int (*send_query)(ldns_buffer* pkt, struct sockaddr_storage* addr,
		socklen_t addrlen, int timeout, struct module_qstate* q,
		int use_tcp);

	/** create a subquery. operate should then return with wait_subq */

	/** allocation service */
	struct alloc_cache* alloc;
	/** internal data for daemon - worker thread. */
	struct worker* worker;
	/** module specific data. indexed by module id. */
	void* modinfo[MAX_MODULE];
};

/**
 * External visible states of the module state machine 
 * Modules may also have an internal state.
 * Modules are supposed to run to completion or until blocked.
 */
enum module_ext_state {
	/** initial state - new query */
	module_state_initial = 0,
	/** waiting for reply to outgoing network query */
	module_wait_reply,
	/** module is waiting for another module */
	module_wait_module,
	/** module is waiting for sub-query */
	module_wait_subquery,
	/** module could not finish the query */
	module_error,
	/** module is finished with query */
	module_finished
};

/**
 * Events that happen to modules, that start or wakeup modules.
 */
enum module_ev {
	/** new query */
	module_event_new = 0,
	/** query passed by other module */
	module_event_pass,
	/** reply inbound from server */
	module_event_reply,
	/** timeout */
	module_event_timeout,
	/** other module finished */
	module_event_mod_done,
	/** subquery finished */
	module_event_subq_done,
	/** error */
	module_event_error
};

/**
 * Module state, per query.
 */
struct module_qstate {
	/** which query is being answered: name, type, class */
	struct query_info qinfo;
	/** hash value of the query qinfo */
	hashvalue_t query_hash;
	/** flags uint16 from query */
	uint16_t query_flags;
	/** edns data from the query */
	struct edns_data edns;

	/** buffer, store resulting reply here. 
	 * May be cleared when module blocks. */
	ldns_buffer* buf;
	/** comm_reply contains server replies */
	struct comm_reply* reply;
	/** region for temporary usage. May be cleared when module blocks. */
	struct region* scratch;
	/** region for this query. Cleared when query process finishes. */
	struct region* region;

	/** which module is executing */
	int curmod;
	/** module states */
	enum module_ext_state ext_state[MAX_MODULE];
	/** module specific data for query. indexed by module id. */
	void* minfo[MAX_MODULE];
	/** environment for this query */
	struct module_env* env;
	/** worker related state for this query */
	struct work_query* work_info;

	/** parent query, only nonNULL for subqueries */
	struct module_qstate* parent;
	/** pointer to first subquery below this one; makes list with next */
	struct module_qstate* subquery_first;
	/** pointer to next sibling subquery (not above or below this one) */
	struct module_qstate* subquery_next;
};

/** 
 * Module functionality block
 */
struct module_func_block {
	/** text string name of module */
	char* name;

	/** 
	 * init the module. Called once for the global state.
	 * This is the place to apply settings from the config file.
	 * @param env: module environment.
	 * @param id: module id number.
	 * return: 0 on error
	 */
	int (*init)(struct module_env* env, int id);
	/**
	 * de-init, delete, the module. Called once for the global state.
	 * @param env: module environment.
	 * @param id: module id number.
	 */
	void (*deinit)(struct module_env* env, int id);

	/**
	 * accept a new query, or work further on existing query.
	 * Changes the qstate->ext_state to be correct on exit.
	 * @param ev: event that causes the module state machine to 
	 *	(re-)activate.
	 * @param qstate: the query state. 
	 * @return: if at exit the ext_state is:
	 *	o wait_module: next module is started. (with pass event).
	 *	o error or finished: previous module is resumed.
	 *	o otherwise it waits until that event happens (assumes
	 *	  the service routine to make subrequest or send message
	 *	  have been called.
	 */
	void (*operate)(struct module_qstate* qstate, enum module_ev event, 
		int id);
	/**
	 * clear module specific data
	 */
	void (*clear)(struct module_qstate* qstate, int id);
};

/** 
 * Debug utility: module external qstate to string 
 * @param s: the state value.
 * @return descriptive string.
 */
const char* strextstate(enum module_ext_state s);

/** 
 * Debug utility: module event to string 
 * @param e: the module event value.
 * @return descriptive string.
 */
const char* strmodulevent(enum module_ev e);

#endif /* UTIL_MODULE_H */
