# System XVI

![System XVI Logo](usr/src/doc/logotype.png)

### Overview

**A forewarning**: System XVI is not yet useable, not even in a limited fashion.
It remains still prototypical, and will require significant work before making
generally available: completion of basic features, making robust the code, and
creation of tests.
*The repository is made available to the public only under this proviso.*

System XVI, or S16, is a modern take on service management. It aims to be as or
more functional than its alternatives, while retaining a modular design in the
UNIX tradition.

We have an IRC channel; find it at
[irc.freenode.net #systemxvi](irc://irc.freenode.net/systemxvi). You might also
want to read the System XVI Manual. This is nowhere near complete, but you may
find the work in progress [here](https://system-xvi.readthedocs.io/en/latest/).

S16 aspires to be both soundly designed and practical: it should be capable of
handling as many use cases as possible, yet achieve this on the basis of its
simplicity and modularity. The twin temptations of under-flexibility and
bloatedness must be avoided.

##### The Four Motives of S16

 * **Interface Orientation**: the system should be designed to fit a clean and
   stable interface. A well-designed interface makes for an obvious
   implementation.
 * **Separation of Concerns**: individual components should not do much alone, but
   work in concert to create a grand system.
 * **Modularity**: components should be easily replaceable and extensible.
 * **Self-healing**: components that crash should be able to restart without
   forgetting system state or otherwise causing breakage.

The Four Motives are a distillation of the factors underpinning the success
of many other systems. From these four motives emerges a naturally clean,
lightweight, and flexible system.

#### Design

System XVI is comprised of several binaries which each do one thing well.

###### Daemons

 * `init` is responsible for *System Initialisation*: it has very few tasks to
    perform, which include reaping processes and starting `s16.restartd`.
 * `s16.configd` is charged with maintaining the *Service Repository*, which
   keeps a record of the state of services, and notifies interested subscribers,
   such as `s16.graphd` when changes to service configuration or state occur.
 * `s16.graphd` is the *Graph Engine*, which creates and maintains a graph of
   services. Using this graph it orders startup, sending requests to
   `s16.restartd` for services to start when their dependencies are satisfied.
 * `s16.restartd` is the *Master Restarter*: its task is to start and keep
   services running in response to requests from `s16.graphd`. If a service
   fails, it notifies `s16.configd` about this.

Some services need different forms of supervision, such as those which are
managed by `inetd`. Rather than bloat its core components with complexity,
System XVI prefers to let other programs handle this. `s16.graphd` entrusts
supervision of these kinds of services to *Delegated Restarters*, such as
`inetd`.

###### Utilities

 * `svccfg` is used to modify entries in the Service Repository. It can read JSON
   files in to import new services, as well as edit the configuration of existing
   services.
 * `svcadm` enables, disables, or restarts a service as requested.
 * `svcs` lists services and their status, with in-depth information available
   on request, such as why a service failed to start.

All these daemons and utilities communicate with each other through a JSON-RPC
interface. The interface is simple and stable; anyone who wants to reimplement a
part of System XVI is welcome to do so. They need only to implement the protocol
that is specified for that part.

### FAQ

> On what platforms is System XVI supported?

These systems are regularly tested to ensure they work:
 * GNU/Linux
 * FreeBSD
 * NetBSD
 * OpenBSD

The other two BSDs (DragonFly and macOS) are also supported but have
not been tested yet. On these platforms all core features should be available.
Acting as a system manager on macOS is nontrivial to implement and out of
scope, but as an auxiliary service manager, it should work.

Any other platform which is reasonably UNIX-like and has a Kernel Queues
available (either natively supported or via a library such as *libkqueue*)
should work to a lesser degree. On those platforms, no tracking of processes
across forks is available by default. You may write a *process-tracker* driver
if the platform provides a means of tracking the forking and exits of
processes.

> Under what licence is System XVI made available?

System XVI is released under the Common Development and Distribution License
(CDDL) version 1.1 only. Code under `external/` and `usr/src/vendor/` may be
licensed otherwise, as these are sources from other projects.
You may instead use System XVI under the terms of the GPLv3 if you wish, with
the exception of [graph.c](usr/src/agent/graphd/graph.c) (see within for reasoning.)

