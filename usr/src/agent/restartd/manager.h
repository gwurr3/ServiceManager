/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/SYSTEMXVI.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2020 David MacKay.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef MANAGER_H_
#define MANAGER_H_

#include "S16/Repository.h"
#include "S16/RestarterServices.h"
#include "S16/ServiceNotification.h"

#include "timer.h"
#include "unit.h"

#define NOTIFY_SOCKET_PATH "/var/run/s16_sd_notify.sock"

typedef struct manager_s
{
    /* Handles */
    /* Kernel Queue */
    int kq;
    /* Repository handle */
    s16db_hdl_t h;
    /* Process-Tracker handle */
    S16ProcessTracker * pt;
    // rreq_list_t rreq_queue;
    /* Timerset */
    timerset_t ts;

    Unit_list_t units;

    /* Repository connection retrying */
    bool repo_up;
    int repo_retry_delay;
    long repo_retry_timer;
} Manager;

/* Set up the SystemD-style notification receiver. */
void sd_notify_srv_setup (int kq);
/* Clean up the SystemD notification receiver. */
void sd_notify_srv_cleanup ();
/* Check a KEvent received. */
void sd_notify_srv_investigate_kevent (struct kevent * ev);

/* Clean up everything after having forked. */
void manager_fork_cleanup ();

/* To be called when the service repository comes up. */
void manager_configd_came_up ();
Unit * manager_find_unit_for_pid (pid_t pid);
Unit * manager_find_unit_for_path (S16Path * path);

extern Manager manager;

#endif