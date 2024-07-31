// SPDX-FileCopyrightText: 2017-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#define DEBUG_FLAG DEBUG_DRV

#include "driver.h"

BIT_FORMATTER_FUNCTION(opts, OPEN)

typedef struct gen_cmd gen_cmd_t;

typedef union proxy_store {
	store_t gen;
	struct {
		STORE(union proxy_store)
		const char *label;  // foreign
		uint ref_count;
		driver_t *real_driver;
		store_t *real_store;
		gen_cmd_t *pending_cmds, **pending_cmds_append;
		wakeup_t wakeup;
		uint fake_nextuid;
		char is_fake;  // Was "created" by dry-run
		char force_async;

		void (*expunge_callback)( message_t *msg, void *aux );
		void (*bad_callback)( void *aux );
		void *callback_aux;
	};
} proxy_store_t;

static void
proxy_store_deref( proxy_store_t *ctx )
{
	if (!--ctx->ref_count) {
		assert( !pending_wakeup( &ctx->wakeup ) );
		free( ctx );
	}
}

static int curr_tag;

#define GEN_CMD \
	uint ref_count; \
	int tag; \
	proxy_store_t *ctx; \
	gen_cmd_t *next; \
	void (*queued_cb)( gen_cmd_t *gcmd );

struct gen_cmd {
	GEN_CMD
};

static gen_cmd_t *
proxy_cmd_new( proxy_store_t *ctx, uint sz )
{
	gen_cmd_t *cmd = nfmalloc( sz );
	cmd->ref_count = 2;
	cmd->tag = ++curr_tag;
	cmd->ctx = ctx;
	ctx->ref_count++;
	return cmd;
}

static void
proxy_cmd_done( gen_cmd_t *cmd )
{
	if (!--cmd->ref_count) {
		proxy_store_deref( cmd->ctx );
		free( cmd );
	}
}

static void
proxy_wakeup( void *aux )
{
	proxy_store_t *ctx = (proxy_store_t *)aux;

	gen_cmd_t *cmd = ctx->pending_cmds;
	assert( cmd );
	if (!(ctx->pending_cmds = cmd->next))
		ctx->pending_cmds_append = &ctx->pending_cmds;
	else
		conf_wakeup( &ctx->wakeup, 0 );
	cmd->queued_cb( cmd );
	proxy_cmd_done( cmd );
}

static void
proxy_invoke( gen_cmd_t *cmd, const char *name )
{
	proxy_store_t *ctx = cmd->ctx;
	if (ctx->force_async) {
		debug( "%s[% 2d] Queue %s\n", ctx->label, cmd->tag, name );
		cmd->next = NULL;
		*ctx->pending_cmds_append = cmd;
		ctx->pending_cmds_append = &cmd->next;
		conf_wakeup( &ctx->wakeup, 0 );
	} else {
		cmd->queued_cb( cmd );
		proxy_cmd_done( cmd );
	}
}

static void
proxy_cancel_queued_cmds( proxy_store_t *ctx )
{
	if (ctx->pending_cmds) {
		// This would involve directly invoking the result callbacks with
		// DRV_CANCEL, for which we'd need another set of dispatch functions.
		// The autotest doesn't need that, so save the effort.
		error( "Fatal: Faking asynchronous cancelation is not supported.\n" );
		abort();
	}
}

#if 0
//# TEMPLATE GETTER
static @type@proxy_@name@( store_t *gctx )
{
	proxy_store_t *ctx = (proxy_store_t *)gctx;

	@type@rv;
	@pre_invoke@
	@indent_invoke@rv = ctx->real_driver->@name@( ctx->real_store );
	@post_invoke@
	debug( "%sCalled @name@@print_fmt_dry@, ret=@fmt@\n", ctx->label@print_pass_dry@, rv );
	return rv;
}
//# END

//# TEMPLATE REGULAR
static @type@proxy_@name@( store_t *gctx@decl_args@ )
{
	proxy_store_t *ctx = (proxy_store_t *)gctx;

	@pre_print_args@
	debug( "%sEnter @name@@print_fmt_dry@@print_fmt_args@\n", ctx->label@print_pass_dry@@print_pass_args@ );
	@print_args@
	@type@rv;
	@pre_invoke@
	@indent_invoke@rv = ctx->real_driver->@name@( ctx->real_store@pass_args@ );
	@post_invoke@
	debug( "%sLeave @name@, ret=@print_fmt_ret@\n", ctx->label, @print_pass_ret@ );
	return rv;
}
//# END

//# TEMPLATE REGULAR_VOID
static @type@proxy_@name@( store_t *gctx@decl_args@ )
{
	proxy_store_t *ctx = (proxy_store_t *)gctx;

	@pre_print_args@
	debug( "%sEnter @name@@print_fmt_dry@@print_fmt_args@\n", ctx->label@print_pass_dry@@print_pass_args@ );
	@print_args@
	@pre_invoke@
	@indent_invoke@ctx->real_driver->@name@( ctx->real_store@pass_args@ );
	@post_invoke@
	debug( "%sLeave @name@\n", ctx->label );
	@action@
}
//# END

//# TEMPLATE CALLBACK_VOID
	debug( "%s[% 2d] Callback enter @name@\n", ctx->label, cmd->tag );
	@print_cb_args@
//# END
//# TEMPLATE CALLBACK_STS
	debug( "%s[% 2d] Callback enter @name@, sts=%d\n", ctx->label, cmd->tag, sts );
//# END
//# TEMPLATE CALLBACK_STS_PRN
	debug( "%s[% 2d] Callback enter @name@, sts=%d\n", ctx->label, cmd->tag, sts );
	if (sts == DRV_OK) {
		@print_cb_args@
	}
//# END
//# TEMPLATE CALLBACK_STS_FMT
	if (sts == DRV_OK) {
		debug( "%s[% 2d] Callback enter @name@, sts=" stringify(DRV_OK) "@print_fmt_cb_args@\n", ctx->label, cmd->tag@print_pass_cb_args@ );
		@print_cb_args@
	} else {
		debug( "%s[% 2d] Callback enter @name@, sts=%d\n", ctx->label, cmd->tag, sts );
	}
//# END

//# TEMPLATE CALLBACK
typedef union {
	gen_cmd_t gen;
	struct {
		GEN_CMD
		void (*callback)( @decl_cb_args@void *aux );
		void *callback_aux;
		@decl_state@
	};
} @name@_cmd_t;

static void
proxy_@name@_cb( @decl_cb_args@void *aux )
{
	@name@_cmd_t *cmd = (@name@_cmd_t *)aux;
	proxy_store_t *ctx = cmd->ctx;

	@count_step@
	@print_cb_args_tpl@
	cmd->callback( @pass_cb_args@cmd->callback_aux );
	debug( "%s[% 2d] Callback leave @name@\n", ctx->label, cmd->tag );
	proxy_cmd_done( &cmd->gen );
}

static void
proxy_do_@name@( gen_cmd_t *gcmd )
{
	@name@_cmd_t *cmd = (@name@_cmd_t *)gcmd;
	proxy_store_t *ctx = cmd->ctx;

	@pre_print_args@
	debug( "%s[% 2d] Enter @name@@print_fmt_dry@@print_fmt_args@\n", ctx->label, cmd->tag@print_pass_dry@@print_pass_args@ );
	@print_args@
	@pre_invoke@
	@indent_invoke@ctx->real_driver->@name@( ctx->real_store@pass_args@, proxy_@name@_cb, cmd );
	@post_invoke@
	debug( "%s[% 2d] Leave @name@\n", ctx->label, cmd->tag );
}

static @type@proxy_@name@( store_t *gctx@decl_args@, void (*cb)( @decl_cb_args@void *aux ), void *aux )
{
	proxy_store_t *ctx = (proxy_store_t *)gctx;

	@name@_cmd_t *cmd = (@name@_cmd_t *)proxy_cmd_new( ctx, sizeof(@name@_cmd_t) );
	cmd->queued_cb = proxy_do_@name@;
	cmd->callback = cb;
	cmd->callback_aux = aux;
	@assign_state@
	proxy_invoke( &cmd->gen, "@name@" );
}
//# END

//# UNDEFINE list_store_print_fmt_cb_args
//# UNDEFINE list_store_print_pass_cb_args
//# DEFINE list_store_print_cb_args
	for (string_list_t *box = boxes; box; box = box->next)
		debug( "  %s\n", box->string );
//# END

//# DEFINE select_box_pre_invoke
	ctx->is_fake = 0;
//# END

//# DEFINE create_box_driable 1
//# DEFINE create_box_fake_invoke
	ctx->is_fake = 1;
//# END
//# DEFINE create_box_counted 1

//# DEFINE open_box_fakeable 1
//# DEFINE open_box_fake_invoke
	ctx->fake_nextuid = 1;
//# END
//# DEFINE open_box_fake_cb_args , 1

//# DEFINE get_uidnext_fakeable 1
//# DEFINE get_uidnext_fake_invoke
	rv = ctx->fake_nextuid;
//# END
//# DEFINE get_uidnext_post_real_invoke
	ctx->fake_nextuid = rv;
//# END

//# DEFINE get_supported_flags_fakeable 1
//# DEFINE get_supported_flags_fake_invoke
	rv = 255;
//# END

//# DEFINE confirm_box_empty_fakeable 1
//# DEFINE confirm_box_empty_fake_invoke
	rv = 1;
//# END

//# DEFINE delete_box_driable 1
//# DEFINE delete_box_fake_invoke
	ctx->is_fake = 0;
//# END
//# DEFINE delete_box_counted 1

//# DEFINE finish_delete_box_driable 1
//# DEFINE finish_delete_box_fake_invoke
	rv = DRV_OK;
//# END

//# DEFINE prepare_load_box_print_fmt_args , opts=%s
//# DEFINE prepare_load_box_print_pass_args , fmt_opts( opts ).str
//# DEFINE prepare_load_box_print_fmt_ret %s
//# DEFINE prepare_load_box_print_pass_ret fmt_opts( rv ).str

//# DEFINE load_box_pre_print_args
	char ubuf[12];
//# END
//# DEFINE load_box_print_fmt_args , [%u,%s] (find >= %u, paired <= %u, new > %u)
//# DEFINE load_box_print_pass_args , cmd->minuid, (cmd->maxuid == UINT_MAX) ? "inf" : (nfsnprintf( ubuf, sizeof(ubuf), "%u", cmd->maxuid ), ubuf), cmd->finduid, cmd->pairuid, cmd->newuid
//# DEFINE load_box_print_args
	if (cmd->excs.size) {
		debugn( "  excs:" );
		for (uint t = 0; t < cmd->excs.size; t++)
			debugn( " %u", cmd->excs.data[t] );
		debug( "\n" );
	}
//# END
//# DEFINE load_box_fakeable 1
//# DEFINE load_box_fake_cb_args , NULL, 0, 0
//# DEFINE load_box_print_fmt_cb_args , total=%d, recent=%d
//# DEFINE load_box_print_pass_cb_args , total_msgs, recent_msgs
//# DEFINE load_box_print_cb_args
	for (message_t *msg = msgs; msg; msg = msg->next) {
		if (msg->status & M_DEAD)
			continue;
		debug( "  uid=%-5u flags=%-4s size=%-6u tuid=%." stringify(TUIDL) "s\n",
		       msg->uid, (msg->status & M_FLAGS) ? fmt_flags( msg->flags ).str : "?", msg->size, *msg->tuid ? msg->tuid : "?" );
	}
//# END

//# UNDEFINE find_new_msgs_print_fmt_cb_args
//# UNDEFINE find_new_msgs_print_pass_cb_args
//# DEFINE find_new_msgs_print_cb_args
	for (message_t *msg = msgs; msg; msg = msg->next) {
		if (msg->status & M_DEAD)
			continue;
		debug( "  uid=%-5u tuid=%." stringify(TUIDL) "s\n", msg->uid, msg->tuid );
	}
//# END

//# DEFINE fetch_msg_print_fmt_args , uid=%u, want_flags=%s, want_date=%s
//# DEFINE fetch_msg_print_pass_args , cmd->msg->uid, !(cmd->msg->status & M_FLAGS) ? "yes" : "no", cmd->data->date ? "yes" : "no"
//# DEFINE fetch_msg_driable 1
//# DEFINE fetch_msg_fake_invoke
	cmd->data->data = strdup( "" );
	cmd->data->len = 0;
//# END
//# DEFINE fetch_msg_print_fmt_cb_args , flags=%s, date=%lld, size=%u
//# DEFINE fetch_msg_print_pass_cb_args , fmt_flags( cmd->data->flags ).str, (long long)cmd->data->date, cmd->data->len
//# DEFINE fetch_msg_print_cb_args
	if (DFlags & DEBUG_DRV_ALL) {
		printf( "%s=========\n", cmd->ctx->label );
		fwrite( cmd->data->data, cmd->data->len, 1, stdout );
		printf( "%s=========\n", cmd->ctx->label );
		fflush( stdout );
	}
//# END

//# DEFINE store_msg_print_fmt_args , flags=%s, date=%lld, size=%u, to_trash=%s
//# DEFINE store_msg_print_pass_args , fmt_flags( cmd->data->flags ).str, (long long)cmd->data->date, cmd->data->len, cmd->to_trash ? "yes" : "no"
//# DEFINE store_msg_print_args
	if (DFlags & DEBUG_DRV_ALL) {
		printf( "%s>>>>>>>>>\n", ctx->label );
		fwrite( cmd->data->data, cmd->data->len, 1, stdout );
		printf( "%s>>>>>>>>>\n", ctx->label );
		fflush( stdout );
	}
//# END
//# DEFINE store_msg_driable 1
//# DEFINE store_msg_fake_cb_args , cmd->to_trash ? 0 : ctx->fake_nextuid++
//# DEFINE store_msg_counted 1

//# DEFINE set_msg_flags_print_fmt_args , uid=%u, add=%s, del=%s
//# DEFINE set_msg_flags_print_pass_args , cmd->uid, fmt_flags( cmd->add ).str, fmt_flags( cmd->del ).str
//# DEFINE set_msg_flags_driable 1
//# DEFINE set_msg_flags_counted 1

//# DEFINE trash_msg_print_fmt_args , uid=%u
//# DEFINE trash_msg_print_pass_args , cmd->msg->uid
//# DEFINE trash_msg_driable 1
//# DEFINE trash_msg_counted 1

//# DEFINE close_box_driable 1
//# DEFINE close_box_fake_cb_args , 0
//# DEFINE close_box_counted 1

//# DEFINE cancel_cmds_print_cb_args
	proxy_cancel_queued_cmds( ctx );
//# END

//# DEFINE free_store_print_args
	proxy_cancel_queued_cmds( ctx );
//# END
//# DEFINE free_store_action
	proxy_store_deref( ctx );
//# END

//# DEFINE cancel_store_print_args
	proxy_cancel_queued_cmds( ctx );
//# END
//# DEFINE cancel_store_action
	proxy_store_deref( ctx );
//# END
#endif

//# SPECIAL set_callbacks
static void
proxy_set_callbacks( store_t *gctx, void (*exp_cb)( message_t *, void * ),
                     void (*bad_cb)( void * ), void *aux )
{
	proxy_store_t *ctx = (proxy_store_t *)gctx;

	ctx->expunge_callback = exp_cb;
	ctx->bad_callback = bad_cb;
	ctx->callback_aux = aux;
}

static void
proxy_invoke_expunge_callback( message_t *msg, proxy_store_t *ctx )
{
	ctx->ref_count++;
	debug( "%sCallback enter expunged message %u\n", ctx->label, msg->uid );
	ctx->expunge_callback( msg, ctx->callback_aux );
	debug( "%sCallback leave expunged message %u\n", ctx->label, msg->uid );
	proxy_store_deref( ctx );
}

static void
proxy_invoke_bad_callback( proxy_store_t *ctx )
{
	ctx->ref_count++;
	debug( "%sCallback enter bad store\n", ctx->label );
	ctx->bad_callback( ctx->callback_aux );
	debug( "%sCallback leave bad store\n", ctx->label );
	proxy_store_deref( ctx );
}

//# EXCLUDE alloc_store
store_t *
proxy_alloc_store( store_t *real_ctx, const char *label, int force_async )
{
	proxy_store_t *ctx;

	ctx = nfzalloc( sizeof(*ctx) );
	ctx->driver = &proxy_driver;
	ctx->gen.conf = real_ctx->conf;
	ctx->ref_count = 1;
	ctx->label = label;
	ctx->force_async = force_async;
	ctx->pending_cmds_append = &ctx->pending_cmds;
	ctx->real_driver = real_ctx->driver;
	ctx->real_store = real_ctx;
	ctx->real_driver->set_callbacks( ctx->real_store,
	                                 (void (*)( message_t *, void * ))proxy_invoke_expunge_callback,
	                                 (void (*)( void * ))proxy_invoke_bad_callback, ctx );
	init_wakeup( &ctx->wakeup, proxy_wakeup, ctx );
	return &ctx->gen;
}

//# EXCLUDE parse_store
//# EXCLUDE cleanup
//# EXCLUDE get_fail_state

#include "drv_proxy.inc"
