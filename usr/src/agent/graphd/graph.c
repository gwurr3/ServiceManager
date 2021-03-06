/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Portions Copyright 2020 David MacKay.
 */

/*
 * Desc: This file implements the dependency graph engine. It was written
 * shortly after making an extensive study of SMF's graph.c, therefore that
 * copyright is included.
 *
 * We create a vertex for each service, each instance, and each dependency
 * group. Edges representing dependencies are generated like so:
 * - Service -> Service's Depgroups
 * - Service -> Service's Instances
 * - Instance -> Service's Depgroups (inherited)
 * - Instance -> Instance's Depgroups
 * - Depgroups -> Depgroup's Dependents (Services and Instances)
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphd.h"
#include "utstring.h"

vertex_list_t graph;

#define PS(x) S16PathToString (x->path)

void print_all ();

edge_t * edge_new (vertex_t * from, vertex_t * to)
{
    edge_t * e = malloc (sizeof (edge_t));
    e->from = from;
    e->to = to;
    return e;
}

void vtx_edge_add (vertex_t * v, vertex_t * to)
{
    edge_list_add (&v->dependencies, edge_new (v, to));
    edge_list_add (&to->dependents, edge_new (to, v));
}

void vtx_online (vertex_t * v, void * reason)
{
    s16note_list_add (
        &notes,
        s16note_new (
            N_STATE_CHANGE, SC_ONLINE, v->path, (int)(intptr_t)reason));
}

void vtx_offline (vertex_t * v, void * reason)
{
    s16note_list_add (
        &notes,
        s16note_new (
            N_STATE_CHANGE, SC_OFFLINE, v->path, (int)(intptr_t)reason));
}

void vtx_enable (vertex_t * v)
{
    s16note_list_add (&notes,
                      s16note_new (N_STATE_CHANGE,
                                   SC_OFFLINE,
                                   v->path,
                                   (int)(intptr_t)kS16RestartOnRestart));
}

/* n.b. we don't really need a reason for this; cut it all out and generate an
 * kS16RestartOnRestart event? */
void vtx_disable (vertex_t * v, void * reason)
{
    s16note_list_add (
        &notes,
        s16note_new (
            N_STATE_CHANGE, SC_DISABLED, v->path, (int)(intptr_t)reason));
}

static void vtx_dependencies_do (vertex_t * v, void (*fun) (vertex_t *, void *),
                                 void * extra)
{
    LL_each (&v->dependencies, it) fun (it->val->to, extra);
}

static void vtx_dependents_do (vertex_t * v, void (*fun) (vertex_t *, void *),
                               void * extra)
{
    LL_each (&v->dependents, it) fun (it->val->to, extra);
}

bool vtx_is_running (vertex_t * v)
{
    return v->state == kS16StateOnline || v->state == kS16StateDegraded;
}

static bool /* continue? */
vtx_is_reachable_internal (vertex_t * v, vertex_t * to, vertex_list_t * seen,
                           vertex_list_t * pathTo)
{
    bool cont = true;

    if (vertex_list_find_eq (seen, v))
        return false;
    vertex_list_add (seen, v);

    if (v->dg_type == kS16ExcludeAll)
        return false;

    if (v == to)
    {
        vertex_list_lpush (pathTo, v);
        return false;
    }

    list_foreach (edge, &v->dependencies, it)
    {
        cont = vtx_is_reachable_internal (it->val->to, to, seen, pathTo);
        if (!cont)
            break;
    }

    if (vertex_list_size (pathTo))
        vertex_list_lpush (pathTo, v);

    return cont;
}

/* Returns true if a vertex is reachable from another. */
bool vtx_is_reachable (vertex_t * from, vertex_t * to, vertex_list_t * pathTo)
{
    vertex_list_t seen = vertex_list_new ();

    vtx_is_reachable_internal (from, to, &seen, pathTo);
    vertex_list_destroy (&seen);

    if (!vertex_list_empty (pathTo))
        return true;
    else
        return false;
}

/*
 * Adds a dependency to the given vertex, checking if it is cyclic.
 * If cyclic, returns 1 and does not add the edges.
 * Otherwise returns 0 and adds the edges.
 */
int vtx_dependency_add (vertex_t * v, vertex_t * to, vertex_list_t * pathTo)
{
    if (vtx_is_reachable (to, v, pathTo))
    {
        S16LogPath (kS16LogError, v->path, "Cyclical dependency\n");
        return 1;
    }
    else
    {
        vtx_edge_add (v, to);
        return 0;
    }
}

void graph_init () { graph = vertex_list_new (); }

bool match_vertex_by_path (vertex_t * cand, void * path)
{
    return S16PathEqual (cand->path, (S16Path *)path);
}

vertex_t * vtx_find_by_path (const S16Path * name)
{
    return list_it_val (
        vertex_list_find (&graph, match_vertex_by_path, (void *)name));
}

vertex_t * vtx_find_or_add (S16Path * path, vertex_type_t type,
                            S16DependencyGroupType dg_type,
                            S16DependencyGroupRestartOnCondition restart_on)
{
    vertex_t * nv = vtx_find_by_path (path);

    if (nv)
        return nv;

    nv = calloc (1, sizeof (vertex_t));

    nv->path = S16PathCopy (path);

    nv->dependencies = edge_list_new ();
    nv->dependents = edge_list_new ();

    nv->type = type;
    nv->dg_type = dg_type;
    nv->restart_on = restart_on;

    nv->state = kS16StateUninitialised;

    vertex_list_add (&graph, nv);

    return nv;
}

void vtx_setup (vertex_t * v);

int setup_dep (S16Path * path, vertex_t * vg, vertex_list_t * pathTo)
{
    vertex_t * vdep;

    assert ((vdep = vtx_find_by_path (path)));

    if (vtx_dependency_add (vg, vdep, pathTo))
        return 1;

    vtx_setup (vdep);
    return 0;
}

S16Path * make_depgroup_path (const S16Path * path, int cnt)
{
    S16Path * dgp = S16PathCopy (path);
    bool inst = dgp->inst;
    char ** ot = inst ? &dgp->inst : &dgp->svc;
    size_t len_inst = snprintf (NULL, 0, "%s>%d", *ot, cnt);
    char * dgn = malloc (len_inst + 1);

    sprintf (dgn, "%s#depgroups/%d", *ot, cnt);
    free (*ot);
    *ot = dgn;
    return dgp;
}

int setup_depgroup (vertex_t * v, S16DependencyGroup * dg, S16Path * dgp,
                    vertex_list_t * pathTo)
{
    vertex_t * dgv;

    dgv = vtx_find_or_add (dgp, V_DEPGROUP, dg->type, dg->restart_on);
    S16PathDestroy (dgp);
    vtx_dependency_add (v, dgv, pathTo);

    v->is_setup = 1;

    list_foreach (path, &dg->paths, it)
    {
        int nerr = setup_dep (it->val, dgv, pathTo);
        if (nerr)
            return nerr;
    }

    return 0;
}

/* Updates a vertex with fresh data from the handle. To be called after updating
 * the handle. */
void vtx_update (vertex_t * v)
{
    int cnt = 0;
    // bool old_enabled = v->is_enabled;
    vertex_list_t pathTo = vertex_list_new ();
    depgroup_list_t * depgroups = NULL;

    if (v->type == V_INST)
    {
        S16ServiceInstance * inst = s16db_lookup_path (&hdl, v->path).i;
        depgroups = &inst->depgroups;
    }
    else if (v->type == V_SVC)
    {
        S16Service * svc = s16db_lookup_path (&hdl, v->path).s;
        depgroups = &svc->depgroups;
    }
    else
    {
        return;
    }

    list_foreach (depgroup, depgroups, it)
    {
        int err;
        S16Path * dgn = make_depgroup_path (v->path, cnt);

        cnt++;

        if ((err = setup_depgroup (v, it->val, dgn, &pathTo)))
        {
            printf ("kS16LogErrorOR: Cyclical dependencies\n");
        }
    }
}

void vtx_setup (vertex_t * v)
{
    if (v->is_setup)
        return;

    v->is_setup = 1;
    v->is_enabled = 1;

    vtx_update (v);
}

/* Satisfiability logic */

typedef enum
{
    SATISFIED,
    UNSATISFIED,
    UNSATISFIABLE,
} satisfied_t;

satisfied_t vtx_satisfies (vertex_t * v, bool recurse);
satisfied_t depgroup_is_satisfied (vertex_t * v, bool recurse);

bool vtx_inst_can_come_up (vertex_t * v)
{
    return (v->is_enabled && !v->to_offline && !v->to_disable) &&
           (depgroup_is_satisfied (v, true) == SATISFIED);
}

satisfied_t vtx_inst_satisfies (vertex_t * v, bool recurse)
{
    assert (v->type == V_INST);

    /* if not setup by now, it is not a valid instance, and needs administrative
     * intervention to correct */
    if (!v->is_setup || !v->is_enabled)
        return UNSATISFIABLE;

    switch (v->state)
    {
    case kS16StateUninitialised:
        return UNSATISFIED;
    case kS16StateDisabled:
        return UNSATISFIABLE;
    case kS16StateOffline:
        if (!recurse)
            return UNSATISFIED;
        else /* determine if satisfiable */
            return depgroup_is_satisfied (v, recurse) == UNSATISFIABLE
                       ? UNSATISFIABLE
                       : UNSATISFIED;
    case kS16StateMaintenance:
        return UNSATISFIABLE;
    case kS16StateOnline:
    case kS16StateDegraded:
        return SATISFIED;
    }
    S16LogPath (kS16LogError, v->path, "Should NOT be here!\n");
    abort ();
}

satisfied_t vtx_inst_satisfies_optional (vertex_t * v, bool recurse)
{
    assert (v->type == V_INST);

    /* if not setup by now, it is not a valid instance, and needs administrative
     * intervention to correct */
    if (!v->is_setup)
        return SATISFIED;

    switch (v->state)
    {
    case kS16StateUninitialised:
        return UNSATISFIED;
    case kS16StateOffline:
        if (!recurse)
            return UNSATISFIED;
        else /* determine if satisfiable */
            return depgroup_is_satisfied (v, recurse) == UNSATISFIABLE
                       ? SATISFIED
                       : UNSATISFIED;
    case kS16StateDisabled:
    case kS16StateMaintenance:
    case kS16StateOnline:
    case kS16StateDegraded:
        return SATISFIED;
    }
    S16LogPath (kS16LogError, v->path, "Should NOT be here!\n");
    abort ();
}

satisfied_t vtx_inst_satisfies_exclusion (vertex_t * v)
{
    assert (v->type == V_INST);

    /* If not yet setup, it's an invalid instance - satisfies exclusion. */
    if (!v->is_setup)
        return SATISFIED;

    switch (v->state)
    {
    case kS16StateUninitialised:
    case kS16StateOffline:
        /* We may be awaiting disabling. */
        return UNSATISFIED;
    case kS16StateMaintenance:
    case kS16StateDisabled:
        return SATISFIED;
    case kS16StateOnline:
    case kS16StateDegraded:
        /* If we are awaiting disabling, then we may not be unsatisfiable. */
        return v->is_enabled ? UNSATISFIABLE : UNSATISFIED;
    }
    S16LogPath (kS16LogError, v->path, "Should NOT be here!\n");
    abort ();
}

satisfied_t vtx_satisfies (vertex_t * v, bool recurse)
{
    if (v->type == V_INST)
        return vtx_inst_satisfies (v, recurse);
    else
        return depgroup_is_satisfied (v, recurse);
}

satisfied_t depgroup_is_satisfied (vertex_t * v, bool recurse)
{
    switch (v->dg_type)
    {
    case kS16RequireAll:
    {
        satisfied_t sat = SATISFIED;

        list_foreach (edge, &v->dependencies, it)
        {
            satisfied_t esat = vtx_satisfies (it->val->to, recurse);
            if (esat != SATISFIED)
                sat = (sat == UNSATISFIABLE) ? UNSATISFIABLE : esat;
        }

        return sat;
    }

    case kS16RequireAny:
    {
        bool sat = UNSATISFIABLE;

        if (edge_list_empty (&v->dependencies))
            return SATISFIED;

        list_foreach (edge, &v->dependencies, it)
        {
            satisfied_t esat = vtx_satisfies (it->val->to, recurse);
            if (esat == SATISFIED)
                return SATISFIED;
            if (esat == UNSATISFIED)
                sat = esat;
        }

        return sat;
    }

    case kS16OptionalAll:
    {
        satisfied_t sat = SATISFIED;

        list_foreach (edge, &v->dependencies, it)
        {
            satisfied_t esat;
            vertex_t * dv = it->val->to;

            assert (dv->type != V_DEPGROUP);

            if (dv->type == V_INST)
            {
                esat = vtx_inst_satisfies_optional (dv, recurse);
                if (esat != SATISFIED)
                    sat = (sat == UNSATISFIABLE) ? UNSATISFIABLE : esat;
            }
            if (dv->type == V_SVC)
            {
                list_foreach (edge, &dv->dependencies, iit)
                {
                    vertex_t * ddv = iit->val->to;
                    esat = vtx_inst_satisfies_optional (ddv, recurse);
                    if (esat != SATISFIED)
                        sat = (sat == UNSATISFIABLE) ? UNSATISFIABLE : esat;
                }
            }
        }

        S16LogPath (kS16LogInfo,
                    v->path,
                    "Optional_all: %s\n",
                    sat == SATISFIED ? "Satisfied" : "Not satisfied");

        return sat;
    }

    case kS16ExcludeAll:
    {
        satisfied_t sat = SATISFIED;

        list_foreach (edge, &v->dependencies, it)
        {
            satisfied_t esat;
            vertex_t * dv = it->val->to;

            assert (dv->type != V_DEPGROUP);

            if (dv->type == V_INST)
            {
                esat = vtx_inst_satisfies_exclusion (dv);
                if (esat != SATISFIED)
                    sat = (sat == UNSATISFIABLE) ? UNSATISFIABLE : esat;
            }
            if (dv->type == V_SVC)
            {
                list_foreach (edge, &dv->dependencies, iit)
                {
                    esat = vtx_inst_satisfies_exclusion (dv);
                    if (esat != SATISFIED)
                        sat = (sat == UNSATISFIABLE) ? UNSATISFIABLE : esat;
                }
            }
        }

        return sat;
    }
    }
    S16LogPath (kS16LogInfo, v->path, "Should NOT be here!\n");
    abort ();
}

vertex_t * install_inst (vertex_t * vsv, S16ServiceInstance * inst)
{
    vertex_t * in =
        vtx_find_or_add (inst->path, V_INST, kS16RequireAll, kS16RestartOnAny);

    return in;
}

vertex_t * graph_install_service (S16Service * svc)
{
    vertex_t * sv =
        vtx_find_or_add (svc->path, V_SVC, kS16RequireAll, kS16RestartOnAny);

    if (sv->is_setup)
        return sv;
    else
    {
        for (inst_list_it it = list_begin (&svc->insts); it != NULL;
             it = inst_list_it_next (it))
        {
            vertex_t * iv = install_inst (sv, it->val);
            vtx_edge_add (sv, iv);
        }
    }

    sv->is_setup = 1;

    return sv;
}

void graph_setup_all ()
{
    s16note_t * note;

    /* This stuff needs to be moved to a test */

#define processNotes()                                                         \
    while ((note = s16note_list_lpop (&notes)))                                \
    graph_process_note (note)

    list_foreach (vertex, &graph, it) vtx_setup (it->val);
    // print_all ();
    list_foreach (vertex, &graph, it) if (it->val->type == V_INST &&
                                          vtx_inst_can_come_up (it->val))
    {
        /* send 'go online' */
        s16note_list_add (
            &notes, s16note_new (N_STATE_CHANGE, SC_OFFLINE, it->val->path, 0));
    }
    processNotes ();

    // print_all ();

    printf ("Now trying disable...\n");
    s16note_list_add (
        &notes,
        s16note_new (
            N_ADMIN_REQ, A_DISABLE, S16PathNew ("a", "i"), kS16RestartOnNone));
    processNotes ();

    printf ("Now trying enable again...\n");

    s16note_list_add (
        &notes,
        s16note_new (
            N_ADMIN_REQ, A_ENABLE, S16PathNew ("a", "i"), kS16RestartOnNone));
    processNotes ();

    printf ("Now trying an offline/online..\n");

    s16note_list_add (&notes,
                      s16note_new (N_STATE_CHANGE,
                                   SC_OFFLINE,
                                   S16PathNew ("a", "i"),
                                   kS16RestartOnNone));
    processNotes ();

    print_all ();
}

void vtx_notify_start (vertex_t * v, void * vreason)
{
    int reason = (int)(intptr_t)vreason;
    switch (v->type)
    {
    case V_INST:
        if (vtx_inst_can_come_up (v))
        {
            if (vtx_is_running (v))
            {
                /* if restarton > on_error, then restart... */
                S16LogPath (
                    kS16LogDebug, v->path, "Not bringing up as already up.\n");
                if (reason > kS16RestartOnError)
                {
                    S16LogPath (
                        kS16LogDebug, v->path, "Sending reset command?\n");
                }
            }
            else
            {
                S16LogPath (kS16LogInfo,
                            v->path,
                            "Bringing up because dependency went up\n");
                s16note_list_add (
                    &notes,
                    s16note_new (N_STATE_CHANGE, SC_ONLINE, v->path, 0));
            }
        }
        break;

    case V_DEPGROUP:
    case V_SVC:
        vtx_dependents_do (
            v, vtx_notify_start, (void *)(intptr_t)v->restart_on);
    }
}

void vtx_notify_stop (vertex_t * v, void * vreason)
{
    S16DependencyGroupRestartOnCondition reason = (intptr_t)vreason;
    switch (v->type)
    {
    case V_INST:
        /* Note: We won't have had this propagated to us unless one of our
         * depgroups is restart-on > the reason */
        if (!vtx_is_running (v))
        {
            /* if restarton > on_error, then restart... */
            S16LogPath (
                kS16LogDebug, v->path, "Not bringing down as already down.\n");
        }
        else
        {
            S16LogPath (kS16LogDebug,
                        v->path,
                        "Bringing down in response to dependency down.\n");

            s16note_list_add (
                &notes,
                s16note_new (N_STATE_CHANGE, SC_OFFLINE, v->path, reason));
        }
        break;

    case V_DEPGROUP:
        /* don't propagate stops to exclude-all groups */
        if (v->dg_type == kS16ExcludeAll)
            break;

        /* if we only restart on, for example, kS16RestartOnError (1), and
         * reason is only kS16RestartOnRestart (2), then we don't need to
         * propagate it. */
        S16LogPath (kS16LogInfo,
                    v->path,
                    "v->Restart_on: %d < Restart: %d?\n",
                    v->restart_on,
                    reason);
        if (v->restart_on < reason)
            break;

        /* otherwise FALLTHROUGH */
    case V_SVC:
        vtx_dependents_do (v, vtx_notify_stop, vreason);
    }
}

void vtx_notify_misc (vertex_t * v, void * reason)
{
    if (v->type == V_INST)
    {
        if (vtx_inst_can_come_up (v) && !vtx_is_running (v))
            vtx_online (v, reason);
    }
    vtx_dependents_do (v, vtx_notify_misc, reason);
}

void vtx_notify_admin_disable (vertex_t * v, void * reason)
{
    switch (v->type)
    {
    case V_INST:
        if (v->state != kS16StateOnline && v->state != kS16StateDegraded)
            S16LogPath (
                kS16LogDebug, v->path, "Not bringing down as already down.\n");
        v->to_offline = true;
        vtx_dependents_do (v, vtx_notify_admin_disable, reason);
        break;

    case V_DEPGROUP:
        /* If a vertex is an kS16ExcludeAll one, we don't mark it. Neither if
         * the restart_on mode is kS16RestartOnNone or kS16RestartOnError. */
        if (v->dg_type == kS16ExcludeAll ||
            (v->restart_on == kS16RestartOnNone ||
             v->restart_on == kS16RestartOnError))
            return;
    case V_SVC:
        vtx_dependents_do (v, vtx_notify_admin_disable, reason);
    }
}

bool vtx_can_go_down (vertex_t * v, bool root)
{
    list_foreach (edge, &v->dependents, it)
    {
        /* check for to_offline; if we didn't apply it, we don't want to go
         * down.*/
        if (it->val->to->type == V_INST && !it->val->to->to_offline)
            continue;
        else if (!vtx_can_go_down (it->val->to, false))
            return false;
    }
    /* If not root (i.e. we have been invoked by others) we object. */
    if (v->type == V_INST &&
        (v->state == kS16StateOnline || v->state == kS16StateDegraded) && !root)
        return false;

    return true;
}

void vtx_offline_if_possible (vertex_t * v, void * reason)
{
    if (!v->to_offline)
        return;

    if (v->type == V_INST)
    {
        if (vtx_can_go_down (v, true))
            vtx_offline (v, reason);
    }
}

/* called after an inst goes offline; offline its dependencies if they are due
 * to go offline. */
void vtx_offline_dependency (vertex_t * v, void * reason)
{
    if (v->type == V_INST && !v->to_offline)
        return;

    if (v->type == V_INST)
    {
        if (vtx_can_go_down (v, true))
            vtx_offline (v, reason);
    }
    else
    {
        vtx_dependencies_do (v, vtx_offline_dependency, reason);
    }
}

void vtx_process_admin_req (vertex_t * v, s16note_admin_type_t type, int reason)
{
    switch (type)
    {
    case A_DISABLE:
        v->to_disable = true;
        v->to_offline = true;
        v->is_enabled = false;

        S16LogPath (kS16LogInfo,
                    v->path,
                    "Received administrative request to disable. Shutting "
                    "down any dependencies first.\n");

        vtx_dependents_do (
            v, vtx_notify_admin_disable, (void *)(intptr_t)reason);
        if (vtx_can_go_down (v, true))
            S16LogPath (kS16LogInfo,
                        v->path,
                        "No subnodes to deal with; can disable directly.\n");
        list_foreach (vertex, &graph, it)
        {
            vtx_offline_if_possible (it->val,
                                     (void *)(intptr_t)kS16RestartOnRestart);
        }
        break;

    case A_ENABLE:
        v->to_disable = false;
        v->to_offline = false;
        v->is_enabled = true;

        S16LogPath (kS16LogInfo,
                    v->path,
                    "Received administrative request to enable.\n");

        vtx_enable (v);
        break;
    default:
        S16Log (kS16LogError, "Admin req type not handled.\n");
    }
}

void vtx_process_state_change (vertex_t * v, s16note_sc_type_t type, int reason)
{
    bool to_offline = v->to_offline;

    switch (type)
    {
    case SC_ONLINE:
        S16LogPath (kS16LogInfo, v->path, "-> Online.\n");
        v->state = kS16StateOnline;
        vtx_dependents_do (v, vtx_notify_start, (void *)(intptr_t)reason);
        break;

    case SC_OFFLINE:
        S16LogPath (kS16LogInfo, v->path, "-> Offline.\n");
        v->state = kS16StateOffline;
        v->to_offline = false;
        if (to_offline)
        {
            vtx_dependencies_do (
                v, vtx_offline_dependency, (void *)(intptr_t)reason);
            if (v->to_disable)
                vtx_disable (v, (void *)(intptr_t)reason);
        }
        else if (vtx_inst_can_come_up (v))
            vtx_online (v, (void *)(intptr_t)reason);

        vtx_dependents_do (v, vtx_notify_stop, (void *)(intptr_t)reason);

        break;

    case SC_DISABLED:
        S16LogPath (kS16LogInfo, v->path, "-> Disabled.\n");
        v->to_offline = false;
        v->to_disable = false;
        v->state = kS16StateDisabled;

        vtx_dependents_do (v, vtx_notify_misc, (void *)(intptr_t)reason);

        break;

    default:
        S16Log (kS16LogError, "State change type not handled.\n");
    }
}

void graph_process_note (s16note_t * note)
{
    vertex_t * v = vtx_find_by_path (note->path);
    if (note->note_type == N_ADMIN_REQ)
        vtx_process_admin_req (v, note->type, note->reason);
    else if (note->note_type == N_STATE_CHANGE)
        vtx_process_state_change (
            vtx_find_by_path (note->path), note->type, note->reason);
    else
        S16Log (kS16LogError, "Note type not handled.\n");
}

#define TypeStr(x)                                                             \
    x->type == V_SVC ? "Svc" : x->type == V_INST ? "Inst" : "DGroup"

/* Sorry that this is so bad. I'll do it properly sometime soon. */
void print_all ()
{
    char * buf = calloc (16800, 1);
    list_foreach (vertex, &graph, it)
    {
        char lbuf[256];
        if (it->val->type == V_SVC)
            sprintf (lbuf,
                     "\"%s\" [shape=cylinder] %s\n",
                     S16PathToString (it->val->path),
                     depgroup_is_satisfied (it->val, false)
                         ? "[style=filled, fillcolor=green]"
                         : "");
        else if (it->val->type == V_INST)
            sprintf (lbuf,
                     "\"%s\" [shape=component] %s\n",
                     S16PathToString (it->val->path),
                     it->val->state == kS16StateOnline
                         ? "[style=filled, fillcolor=green]"
                         : "");
        else if (it->val->type == V_DEPGROUP)
        {
            const char * dgts;
            switch (it->val->dg_type)
            {
            case kS16RequireAll:
                dgts = "require-all";
                break;
            case kS16RequireAny:
                dgts = "require-any";
                break;
            case kS16OptionalAll:
                dgts = "optional-all";
                break;
            case kS16ExcludeAll:
                dgts = "exclude-all";
                break;
            }
            sprintf (lbuf,
                     "\"%s\" [shape=note, label=\"%s\\n%s\"]\n",
                     S16PathToString (it->val->path),
                     S16PathToString (it->val->path),
                     dgts);
        }

        strcat (buf, lbuf);

        for (edge_list_it ite = list_begin (&((it->val)->dependents));
             ite != NULL;
             ite = list_next (ite))
        {
            char lbuf[256];
            sprintf (lbuf,
                     "\"%s\" -> \"%s\" [label=\"depends on\"];\n",
                     S16PathToString (ite->val->to->path),
                     S16PathToString (ite->val->from->path));
            strcat (buf, lbuf);
        }
    }
    printf ("digraph {\n%s}\n", buf);
}