/*++
/* NAME
/*	postscreen_misc 3
/* SUMMARY
/*	postscreen misc routines
/* SYNOPSIS
/*	#include <postscreen.h>
/*
/*	char	*ps_format_delta_time(buf, tv, delta)
/*	VSTRING	*buf;
/*	struct timeval tv;
/*	DELTA_TIME *delta;
/*
/*	void	ps_conclude(state)
/*	PS_STATE *state;
/*
/*	void	ps_hangup_event(state)
/*	PS_STATE *state;
/* DESCRIPTION
/*	ps_format_delta_time() computes the time difference between
/*	tv (past) and the present, formats the time difference with
/*	sub-second resolution in a human-readable way, and returns
/*	the integer time difference in seconds through the delta
/*	argument.
/*
/*	ps_conclude() logs when a client passes all necessary tests,
/*	updates the postscreen cache for any testes that were passed,
/*	and either forwards the connection to a real SMTP server or
/*	replies with the text in state->error_reply and hangs up the
/*	connection (by default, state->error_reply is set to a
/*	default 421 reply).
/*
/*	ps_hangup_event() cleans up after a client connection breaks
/*	unexpectedly. If logs the test where the break happened,
/*	and how much time as spent in that test before the connection
/*	broke.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <iostuff.h>
#include <format_tv.h>

/* Global library. */

#include <mail_params.h>

/* Application-specific. */

#include <postscreen.h>

/* ps_format_delta_time - pretty-formatted delta time */

char   *ps_format_delta_time(VSTRING *buf, struct timeval tv, DELTA_TIME *delta)
{
    DELTA_TIME pdelay;
    struct timeval now;

    GETTIMEOFDAY(&now);
    PS_CALC_DELTA(pdelay, now, tv);
    VSTRING_RESET(buf);
    format_tv(buf, pdelay.dt_sec, pdelay.dt_usec, SIG_DIGS, var_delay_max_res);
    *delta = pdelay;
    return (STR(buf));
}

/* ps_conclude - bring this session to a conclusion */

void    ps_conclude(PS_STATE *state)
{
    const char *myname = "ps_conclude";

    if (msg_verbose)
	msg_info("flags for %s: %s",
		 myname, ps_print_state_flags(state->flags, myname));

    /*
     * Handle clients that passed at least one test other than permanent
     * whitelisting, and that didn't fail any test including permanent
     * blacklisting. There may still be unfinished tests; those tests will
     * need to be completed when the client returns in a later session.
     */
    if (state->flags & PS_STATE_MASK_ANY_FAIL)
	state->flags &= ~PS_STATE_MASK_ANY_PASS;

    /*
     * Log our final blessing when all unfinished tests were completed.
     */
    if ((state->flags & PS_STATE_MASK_ANY_PASS) != 0
	&& (state->flags & PS_STATE_MASK_ANY_PASS) ==
	PS_STATE_FLAGS_TODO_TO_PASS(state->flags & PS_STATE_MASK_ANY_TODO))
	msg_info("PASS %s [%s]:%s", (state->flags & PS_STATE_FLAG_NEW) == 0 ?
		 "OLD" : "NEW", PS_CLIENT_ADDR_PORT(state));

    /*
     * Update the postscreen cache. This still supports a scenario where a
     * client gets whitelisted in the course of multiple sessions, as long as
     * that client does not "fail" any test.
     */
    if ((state->flags & PS_STATE_MASK_ANY_UPDATE) != 0
	&& ps_cache_map != 0) {
	ps_print_tests(ps_temp, state);
	ps_cache_update(ps_cache_map, state->smtp_client_addr, STR(ps_temp));
    }

    /*
     * Either hand off the socket to a real SMTP engine, or say bye-bye.
     */
    if ((state->flags & PS_STATE_FLAG_NOFORWARD) == 0) {
	ps_send_socket(state);
    } else {
	if ((state->flags & PS_STATE_FLAG_HANGUP) == 0)
	    (void) PS_SEND_REPLY(state, state->final_reply);
	msg_info("DISCONNECT [%s]:%s", PS_CLIENT_ADDR_PORT(state));
	ps_free_session_state(state);
    }
}

/* ps_hangup_event - handle unexpected disconnect */

void    ps_hangup_event(PS_STATE *state)
{
    DELTA_TIME elapsed;

    /*
     * Sessions can break at any time, even after the client passes all tests
     * (some MTAs including Postfix don't send QUIT when connection reuse is
     * enabled). This must not be treated as a protocol test failure.
     * 
     * Log the current test phase, and the elapsed time after the start of that
     * phase.
     */
    state->flags |= PS_STATE_FLAG_HANGUP;
    msg_info("HANGUP after %s from [%s]:%s in %s",
	     ps_format_delta_time(ps_temp, state->start_time, &elapsed),
	     PS_CLIENT_ADDR_PORT(state), state->test_name);
    state->flags |= PS_STATE_FLAG_NOFORWARD;
    ps_conclude(state);
}
