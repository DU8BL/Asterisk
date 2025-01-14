/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Russell Bryant <russell@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Automatic channel service routines
 *
 * \author Mark Spencer <markster@digium.com> 
 * \author Russell Bryant <russell@digium.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 147386 $")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "asterisk/pbx.h"
#include "asterisk/frame.h"
#include "asterisk/sched.h"
#include "asterisk/options.h"
#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/file.h"
#include "asterisk/translate.h"
#include "asterisk/manager.h"
#include "asterisk/chanvars.h"
#include "asterisk/linkedlists.h"
#include "asterisk/indications.h"
#include "asterisk/lock.h"
#include "asterisk/utils.h"

#define MAX_AUTOMONS 1500

struct asent {
	struct ast_channel *chan;
	/*! This gets incremented each time autoservice gets started on the same
	 *  channel.  It will ensure that it doesn't actually get stopped until 
	 *  it gets stopped for the last time. */
	unsigned int use_count;
	unsigned int orig_end_dtmf_flag:1;
	AST_LIST_HEAD_NOLOCK(, ast_frame) deferred_frames;
	AST_LIST_ENTRY(asent) list;
};

static AST_LIST_HEAD_STATIC(aslist, asent);
static ast_cond_t as_cond;

static pthread_t asthread = AST_PTHREADT_NULL;

static int as_chan_list_state;

static void *autoservice_run(void *ign)
{
	for (;;) {
		struct ast_channel *mons[MAX_AUTOMONS];
		struct asent *ents[MAX_AUTOMONS];
		struct ast_channel *chan;
		struct asent *as;
		int i, x = 0, ms = 50;
		struct ast_frame *f = NULL;
		struct ast_frame *defer_frame = NULL;

		AST_LIST_LOCK(&aslist);

		/* At this point, we know that no channels that have been removed are going
		 * to get used again. */
		as_chan_list_state++;

		if (AST_LIST_EMPTY(&aslist)) {
			ast_cond_wait(&as_cond, &aslist.lock);
		}

		AST_LIST_TRAVERSE(&aslist, as, list) {
			if (!as->chan->_softhangup) {
				if (x < MAX_AUTOMONS) {
					ents[x] = as;
					mons[x++] = as->chan;
				} else {
					ast_log(LOG_WARNING, "Exceeded maximum number of automatic monitoring events.  Fix autoservice.c\n");
				}
			}
		}

		AST_LIST_UNLOCK(&aslist);

		if (!x) {
			continue;
		}

		chan = ast_waitfor_n(mons, x, &ms);
		if (!chan) {
			continue;
		}

		f = ast_read(chan);
	
		if (!f) {
			struct ast_frame hangup_frame = { 0, };
			/* No frame means the channel has been hung up.
			 * A hangup frame needs to be queued here as ast_waitfor() may
			 * never return again for the condition to be detected outside
			 * of autoservice.  So, we'll leave a HANGUP queued up so the
			 * thread in charge of this channel will know. */

			hangup_frame.frametype = AST_FRAME_CONTROL;
			hangup_frame.subclass = AST_CONTROL_HANGUP;

			defer_frame = &hangup_frame;
		} else {

			/* Do not add a default entry in this switch statement.  Each new
			 * frame type should be addressed directly as to whether it should
			 * be queued up or not. */

			switch (f->frametype) {
			/* Save these frames */
			case AST_FRAME_DTMF_END:
			case AST_FRAME_CONTROL:
			case AST_FRAME_TEXT:
			case AST_FRAME_IMAGE:
			case AST_FRAME_HTML:
				defer_frame = f;
				break;

			/* Throw these frames away */
			case AST_FRAME_DTMF_BEGIN:
			case AST_FRAME_VOICE:
			case AST_FRAME_VIDEO:
			case AST_FRAME_NULL:
			case AST_FRAME_IAX:
			case AST_FRAME_CNG:
			case AST_FRAME_MODEM:
				break;
			}
		}

		if (defer_frame) {
			for (i = 0; i < x; i++) {
				struct ast_frame *dup_f;
				
				if (mons[i] != chan) {
					continue;
				}
				
				if ((dup_f = ast_frdup(defer_frame))) {
					AST_LIST_INSERT_TAIL(&ents[i]->deferred_frames, dup_f, frame_list);
				}
				
				break;
			}
		}

		if (f) {
			ast_frfree(f);
		}
	}

	asthread = AST_PTHREADT_NULL;

	return NULL;
}

int ast_autoservice_start(struct ast_channel *chan)
{
	int res = 0;
	struct asent *as;

	/* Check if the channel already has autoservice */
	AST_LIST_LOCK(&aslist);
	AST_LIST_TRAVERSE(&aslist, as, list) {
		if (as->chan == chan) {
			as->use_count++;
			break;
		}
	}
	AST_LIST_UNLOCK(&aslist);

	if (as) {
		/* Entry exists, autoservice is already handling this channel */
		return 0;
	}

	if (!(as = ast_calloc(1, sizeof(*as))))
		return -1;
	
	/* New entry created */
	as->chan = chan;
	as->use_count = 1;

	ast_channel_lock(chan);
	as->orig_end_dtmf_flag = ast_test_flag(chan, AST_FLAG_END_DTMF_ONLY) ? 1 : 0;
	if (!as->orig_end_dtmf_flag)
		ast_set_flag(chan, AST_FLAG_END_DTMF_ONLY);
	ast_channel_unlock(chan);

	AST_LIST_LOCK(&aslist);

	if (AST_LIST_EMPTY(&aslist) && asthread != AST_PTHREADT_NULL) {
		ast_cond_signal(&as_cond);
	}

	AST_LIST_INSERT_HEAD(&aslist, as, list);

	if (asthread == AST_PTHREADT_NULL) { /* need start the thread */
		if (ast_pthread_create_background(&asthread, NULL, autoservice_run, NULL)) {
			ast_log(LOG_WARNING, "Unable to create autoservice thread :(\n");
			/* There will only be a single member in the list at this point,
			   the one we just added. */
			AST_LIST_REMOVE(&aslist, as, list);
			free(as);
			asthread = AST_PTHREADT_NULL;
			res = -1;
		} else {
			pthread_kill(asthread, SIGURG);
			pthread_setname_np(asthread, "autoservice_run");
		}
	}

	AST_LIST_UNLOCK(&aslist);

	return res;
}

int ast_autoservice_stop(struct ast_channel *chan)
{
	int res = -1;
	struct asent *as, *removed = NULL;
	struct ast_frame *f;
	int chan_list_state;

	AST_LIST_LOCK(&aslist);

	/* Save the autoservice channel list state.  We _must_ verify that the channel
	 * list has been rebuilt before we return.  Because, after we return, the channel
	 * could get destroyed and we don't want our poor autoservice thread to step on
	 * it after its gone! */
	chan_list_state = as_chan_list_state;

	/* Find the entry, but do not free it because it still can be in the
	   autoservice thread array */
	AST_LIST_TRAVERSE_SAFE_BEGIN(&aslist, as, list) {	
		if (as->chan == chan) {
			as->use_count--;
			if (as->use_count < 1) {
				AST_LIST_REMOVE_CURRENT(&aslist, list);
				removed = as;
			}
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END

	if (removed && asthread != AST_PTHREADT_NULL) {
		pthread_kill(asthread, SIGURG);
	}

	AST_LIST_UNLOCK(&aslist);

	if (!removed) {
		return 0;
	}

	/* Wait while autoservice thread rebuilds its list. */
	while (chan_list_state == as_chan_list_state) {
		usleep(1000);
	}

	/* Now autoservice thread should have no references to our entry
	   and we can safely destroy it */

	if (!chan->_softhangup) {
		res = 0;
	}

	if (!as->orig_end_dtmf_flag) {
		ast_clear_flag(chan, AST_FLAG_END_DTMF_ONLY);
	}

	while ((f = AST_LIST_REMOVE_HEAD(&as->deferred_frames, frame_list))) {
		ast_queue_frame(chan, f);
		ast_frfree(f);
	}

	free(as);

	return res;
}

void ast_autoservice_init(void)
{
	ast_cond_init(&as_cond, NULL);
}
