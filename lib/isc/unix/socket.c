/*
 * Copyright (C) 1998  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/thread.h>
#include <isc/mutex.h>
#include <isc/condition.h>
#include <isc/socket.h>

#include "util.h"

#ifndef _WIN32
#define WINAPI /* we're not windows */
#endif

#define ISC_TASK_SEND(a, b) do { \
	INSIST(isc_task_send(a, b) == ISC_R_SUCCESS); \
} while (0);

#define SOFT_ERROR(e)	((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)

#if 1
#define ISC_SOCKET_DEBUG
#endif

#if defined(ISC_SOCKET_DEBUG)
#define TRACE_WATCHER	0x0001
#define TRACE_LISTEN	0x0002
#define TRACE_CONNECT	0x0004
#define TRACE_RECV	0x0008
#define TRACE_SEND    	0x0010
#define TRACE_MANAGER	0x0020

int trace_level = 0xffffffff;
#define XTRACE(l, a)	if (l & trace_level) printf a
#define XENTER(l, a)	if (l & trace_level) printf("ENTER %s\n", (a))
#define XEXIT(l, a)	if (l & trace_level) printf("EXIT %s\n", (a))
#else
#define XTRACE(l, a)
#define XENTER(l, a)
#define XEXIT(l, a)
#endif

/*
 * internal event used to send readable/writable events to our internal
 * functions.
 */
typedef struct rwintev {
	isc_event_t			common;	   /* Sender is the socket. */
	isc_task_t *			task;	   /* task to send these to */
	isc_socketevent_t *		done_ev;   /* the done event to post */
	isc_boolean_t			partial;   /* partial i/o ok */
	isc_boolean_t			canceled;  /* I/O was canceled */
	isc_boolean_t			posted;	   /* event posted to task */
	LINK(struct rwintev)		link;	   /* next event */
} rwintev_t;

typedef struct ncintev {
	isc_event_t			common;	   /* Sender is the socket */
	isc_task_t *			task;	   /* task to send these to */
	isc_socket_newconnev_t *	done_ev;   /* the done event */
	isc_boolean_t			canceled;  /* accept was canceled */
	isc_boolean_t			posted;	   /* event posted to task */
	LINK(struct ncintev)		link;	   /* next event */
} ncintev_t;

typedef struct cnintev {
	isc_event_t			common;	   /* Sender is the socket */
	isc_task_t *			task;	   /* task to send these to */
	isc_socket_connev_t *		done_ev;   /* the done event */
	isc_boolean_t			canceled;  /* connect was canceled */
	isc_boolean_t			posted;	   /* event posted to task */
} cnintev_t;

#define SOCKET_MAGIC		0x494f696fU	/* IOio */
#define VALID_SOCKET(t)		((t) != NULL && (t)->magic == SOCKET_MAGIC)

struct isc_socket {
	/* Not locked. */
	unsigned int			magic;
	isc_socketmgr_t *		manager;
	isc_mutex_t			lock;
	isc_sockettype_t		type;

	/* Locked by socket lock. */
	unsigned int			references;
	int				fd;
	isc_result_t			recv_result;
	isc_result_t			send_result;
	LIST(rwintev_t)			recv_list;
	LIST(rwintev_t)			send_list;
	LIST(ncintev_t)			accept_list;
	cnintev_t *			connect_ev;
	isc_boolean_t			pending_recv;
	isc_boolean_t			pending_send;
	isc_boolean_t			pending_accept;
	isc_boolean_t			listener;  /* is a listener socket */
	isc_boolean_t			connected;
	isc_boolean_t			connecting; /* connect pending */
	rwintev_t *			riev; /* allocated recv intev */
	rwintev_t *			wiev; /* allocated send intev */
	cnintev_t *			ciev; /* allocated accept intev */
	isc_sockaddr_t			address;  /* remote address */
	int				addrlength; /* remote addrlen */
};

#define SOCKET_MANAGER_MAGIC		0x494f6d67U	/* IOmg */
#define VALID_MANAGER(m)		((m) != NULL && \
					 (m)->magic == SOCKET_MANAGER_MAGIC)
struct isc_socketmgr {
	/* Not locked. */
	unsigned int			magic;
	isc_memctx_t *			mctx;
	isc_mutex_t			lock;
	/* Locked by manager lock. */
	unsigned int			nsockets;  /* sockets managed */
	isc_thread_t			watcher;
	fd_set				read_fds;
	fd_set				write_fds;
	isc_socket_t *			fds[FD_SETSIZE];
	int				fdstate[FD_SETSIZE];
	int				maxfd;
	int				pipe_fds[2];
};

#define CLOSED		0	/* this one must be zero */
#define MANAGED		1
#define CLOSE_PENDING	2

static void send_recvdone_event(isc_socket_t *, rwintev_t **,
				isc_socketevent_t **, isc_result_t);
static void send_senddone_event(isc_socket_t *, rwintev_t **,
				isc_socketevent_t **, isc_result_t);
static void done_event_destroy(isc_event_t *);
static void free_socket(isc_socket_t **);
static isc_result_t allocate_socket(isc_socketmgr_t *, isc_sockettype_t,
				    isc_socket_t **);
static void destroy(isc_socket_t **);
static void internal_accept(isc_task_t *, isc_event_t *);
static void internal_connect(isc_task_t *, isc_event_t *);
static void internal_recv(isc_task_t *, isc_event_t *);
static void internal_send(isc_task_t *, isc_event_t *);

#define SELECT_POKE_SHUTDOWN		(-1)
#define SELECT_POKE_NOTHING		(-2)
#define SELECT_POKE_RESCAN		(-3) /* XXX implement */

/*
 * Poke the select loop when there is something for us to do.
 * We assume that if a write completes here, it will be inserted into the
 * queue fully.  That is, we will not get partial writes.
 */
static void
select_poke(isc_socketmgr_t *mgr, int msg)
{
	int cc;

	cc = write(mgr->pipe_fds[1], &msg, sizeof(int));
	if (cc < 0) /* XXX need to handle EAGAIN, EINTR here */
		FATAL_ERROR(__FILE__, __LINE__,
			    "write() failed during watcher poke: %s",
			    strerror(errno));
}

/*
 * read a message on the internal fd.
 */
static int
select_readmsg(isc_socketmgr_t *mgr)
{
	int msg;
	int cc;

	cc = read(mgr->pipe_fds[0], &msg, sizeof(int));
	if (cc < 0) {
		if (SOFT_ERROR(errno))
			return (SELECT_POKE_NOTHING);

		FATAL_ERROR(__FILE__, __LINE__,
			    "read() failed during watcher poke: %s",
			    strerror(errno));

		return (SELECT_POKE_NOTHING);
	}

	return (msg);
}

/*
 * Make a fd non-blocking
 */
static isc_result_t
make_nonblock(int fd)
{
	int ret;
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	ret = fcntl(fd, F_SETFL, flags);

	if (ret == -1) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "fcntl(%d, F_SETFL, %d): %s",
				 fd, flags, strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	return (ISC_R_SUCCESS);
}

#ifdef ISC_SOCKET_DEBUG
static void
socket_dump(isc_socket_t *sock)
{
	rwintev_t *	rwiev;
	ncintev_t *	aiev;

	printf("--------\nDump of socket %p\n", sock);
	printf("fd: %d, references %u\n", sock->fd, sock->references);

	printf("recv queue:\n");
	rwiev = HEAD(sock->recv_list);
	while (rwiev != NULL) {
		printf("\tintev %p, done_ev %p, task %p, "
		       "canceled %d, posted %d",
		       rwiev, rwiev->done_ev, rwiev->task, rwiev->canceled,
		       rwiev->posted);
		rwiev = NEXT(rwiev, link);
	}

	printf("send queue:\n");
	rwiev = HEAD(sock->send_list);
	while (rwiev != NULL) {
		printf("\tintev %p, done_ev %p, task %p, "
		       "canceled %d, posted %d",
		       rwiev, rwiev->done_ev, rwiev->task, rwiev->canceled,
		       rwiev->posted);
		rwiev = NEXT(rwiev, link);
	}

	printf("accept queue:\n");
	aiev = HEAD(sock->accept_list);
	while (aiev != NULL) {
		printf("\tintev %p, done_ev %p, task %p, "
		       "canceled %d, posted %d\n",
		       aiev, aiev->done_ev, aiev->task, aiev->canceled,
		       aiev->posted);
		aiev = NEXT(aiev, link);
	}

	printf("--------\n");
}
#endif

/*
 * Handle freeing a done event when needed.
 */
static void
done_event_destroy(isc_event_t *ev)
{
	isc_socket_t *sock = ev->sender;
	isc_boolean_t kill_socket = ISC_FALSE;

	/*
	 * detach from the socket.  We would have already detached from the
	 * task when we actually queue this event up.
	 */
	LOCK(&sock->lock);
		
	REQUIRE(sock->references > 0);
	sock->references--;
	XTRACE(TRACE_MANAGER, ("done_event_destroy: sock %p, ref cnt == %d\n",
			       sock, sock->references));

	if (sock->references == 0)
		kill_socket = ISC_TRUE;
	UNLOCK(&sock->lock);
	
	if (kill_socket)
		destroy(&sock);
}

/*
 * Kill.
 *
 * Caller must ensure locking.
 */
static void
destroy(isc_socket_t **sockp)
{
	isc_socket_t *sock = *sockp;
	isc_socketmgr_t *manager = sock->manager;

	XTRACE(TRACE_MANAGER,
	       ("destroy sockp = %p, sock = %p\n", sockp, sock));

	LOCK(&manager->lock);

	/*
	 * Noone has this socket open, so the watcher doesn't have to be
	 * poked, and the socket doesn't have to be locked.
	 */
	manager->fds[sock->fd] = NULL;
	manager->fdstate[sock->fd] = CLOSE_PENDING;
	select_poke(sock->manager, sock->fd);
	manager->nsockets--;
	XTRACE(TRACE_MANAGER, ("nsockets == %d\n", manager->nsockets));

	/*
	 * XXX should reset manager->maxfd here
	 */

	UNLOCK(&manager->lock);

	free_socket(sockp);
}

static isc_result_t
allocate_socket(isc_socketmgr_t *manager, isc_sockettype_t type,
		isc_socket_t **socketp)
{
	isc_socket_t *sock;

	sock = isc_mem_get(manager->mctx, sizeof *sock);

	if (sock == NULL)
		return (ISC_R_NOMEMORY);

	sock->magic = SOCKET_MAGIC;
	sock->references = 0;

	sock->manager = manager;
	sock->type = type;
	sock->fd = -1;

	/*
	 * set up list of readers and writers to be initially empty
	 */
	INIT_LIST(sock->recv_list);
	INIT_LIST(sock->send_list);
	INIT_LIST(sock->accept_list);
	sock->pending_recv = ISC_FALSE;
	sock->pending_send = ISC_FALSE;
	sock->pending_accept = ISC_FALSE;
	sock->listener = ISC_FALSE;
	sock->connecting = ISC_FALSE;
	sock->connected = ISC_FALSE;

	sock->recv_result = ISC_R_SUCCESS;
	sock->send_result = ISC_R_SUCCESS;

	/*
	 * initialize the lock
	 */
	if (isc_mutex_init(&sock->lock) != ISC_R_SUCCESS) {
		sock->magic = 0;
		isc_mem_put(manager->mctx, sock, sizeof *sock);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed");
		return (ISC_R_UNEXPECTED);
	}

	*socketp = sock;

	return (ISC_R_SUCCESS);
}

/*
 * This event requires that the various lists be empty, that the reference
 * count be 1, and that the magic number is valid.  The other socket bits,
 * like the lock, must be initialized as well.  The fd associated must be
 * marked as closed, by setting it to -1 on close, or this routine will
 * also close the socket.
 */
static void
free_socket(isc_socket_t **socketp)
{
	isc_socket_t *sock = *socketp;

	REQUIRE(sock->references == 0);
	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(!sock->connecting);
	REQUIRE(!sock->pending_recv);
	REQUIRE(!sock->pending_send);
	REQUIRE(!sock->pending_accept);
	REQUIRE(EMPTY(sock->recv_list));
	REQUIRE(EMPTY(sock->send_list));
	REQUIRE(EMPTY(sock->accept_list));

	sock->magic = 0;

	(void)isc_mutex_destroy(&sock->lock);

	isc_mem_put(sock->manager->mctx, sock, sizeof *sock);

	*socketp = NULL;
}

/*
 * Create a new 'type' socket managed by 'manager'.  The sockets
 * parameters are specified by 'expires' and 'interval'.  Events
 * will be posted to 'task' and when dispatched 'action' will be
 * called with 'arg' as the arg value.  The new socket is returned
 * in 'socketp'.
 */
isc_result_t
isc_socket_create(isc_socketmgr_t *manager, isc_sockettype_t type,
		  isc_socket_t **socketp)
{
	isc_socket_t *sock = NULL;
	isc_result_t ret;

	REQUIRE(VALID_MANAGER(manager));
	REQUIRE(socketp != NULL && *socketp == NULL);

	XENTER(TRACE_MANAGER, "isc_socket_create");
	
	ret = allocate_socket(manager, type, &sock);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	switch (type) {
	case isc_socket_udp:
		sock->fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
		break;
	case isc_socket_tcp:
		sock->fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
		break;
	}
	if (sock->fd < 0) {
		free_socket(&sock);

		switch (errno) {
		case EMFILE:
		case ENFILE:
		case ENOBUFS:
			return (ISC_R_NORESOURCES);
			break;
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "socket() failed: %s",
					 strerror(errno));
			return (ISC_R_UNEXPECTED);
			break;
		}
	}

	if (make_nonblock(sock->fd) != ISC_R_SUCCESS) {
		free_socket(&sock);
		return (ISC_R_UNEXPECTED);
	}

	sock->references = 1;
	*socketp = sock;

	LOCK(&manager->lock);

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */

	manager->fds[sock->fd] = sock;
	manager->fdstate[sock->fd] = MANAGED;
	manager->nsockets++;
	XTRACE(TRACE_MANAGER, ("nsockets == %d\n", manager->nsockets));
	if (manager->maxfd < sock->fd)
		manager->maxfd = sock->fd;

	UNLOCK(&manager->lock);

	XEXIT(TRACE_MANAGER, "isc_socket_create");

	return (ISC_R_SUCCESS);
}

/*
 * Attach to a socket.  Caller must explicitly detach when it is done.
 */
void
isc_socket_attach(isc_socket_t *sock, isc_socket_t **socketp)
{
	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(socketp != NULL && *socketp == NULL);

	LOCK(&sock->lock);
	sock->references++;
	UNLOCK(&sock->lock);
	
	*socketp = sock;
}

/*
 * Dereference a socket.  If this is the last reference to it, clean things
 * up by destroying the socket.
 */
void 
isc_socket_detach(isc_socket_t **socketp)
{
	isc_socket_t *sock;
	isc_boolean_t kill_socket = ISC_FALSE;

	REQUIRE(socketp != NULL);
	sock = *socketp;
	REQUIRE(VALID_SOCKET(sock));

	XENTER(TRACE_MANAGER, "isc_socket_detach");

	LOCK(&sock->lock);
	REQUIRE(sock->references > 0);
	sock->references--;
	if (sock->references == 0)
		kill_socket = ISC_TRUE;
	UNLOCK(&sock->lock);
	
	if (kill_socket)
		destroy(&sock);

	XEXIT(TRACE_MANAGER, "isc_socket_detach");

	*socketp = NULL;
}

/*
 * I/O is possible on a given socket.  Schedule an event to this task that
 * will call an internal function to do the I/O.  This will charge the
 * task with the I/O operation and let our select loop handler get back
 * to doing something real as fast as possible.
 *
 * The socket and manager must be locked before calling this function.
 */
static void
dispatch_read(isc_socket_t *sock)
{
	rwintev_t *iev;
	isc_event_t *ev;

	iev = HEAD(sock->recv_list);
	ev = (isc_event_t *)iev;

	INSIST(!sock->pending_recv);

	sock->pending_recv = ISC_TRUE;

	XTRACE(TRACE_WATCHER, ("dispatch_read:  posted event %p to task %p\n",
			       ev, iev->task));

	iev->posted = ISC_TRUE;

	ISC_TASK_SEND(iev->task, &ev);
}

static void
dispatch_write(isc_socket_t *sock)
{
	rwintev_t *iev;
	isc_event_t *ev;

	iev = HEAD(sock->send_list);
	ev = (isc_event_t *)iev;

	INSIST(!sock->pending_send);
	sock->pending_send = ISC_TRUE;

	iev->posted = ISC_TRUE;

	ISC_TASK_SEND(iev->task, &ev);
}

static void
dispatch_listen(isc_socket_t *sock)
{
	ncintev_t *iev;
	isc_event_t *ev;

	iev = HEAD(sock->accept_list);
	ev = (isc_event_t *)iev;

	INSIST(!sock->pending_accept);

	sock->pending_accept = ISC_TRUE;

	iev->posted = ISC_TRUE;

	ISC_TASK_SEND(iev->task, &ev);
}

static void
dispatch_connect(isc_socket_t *sock)
{
	cnintev_t *iev;

	INSIST(sock->connecting);

	iev = sock->connect_ev;

	iev->posted = ISC_TRUE;

	ISC_TASK_SEND(iev->task, (isc_event_t **)&iev);
}

/*
 * Dequeue an item off the given socket's read queue, set the result code
 * in the done event to the one provided, and send it to the task it was
 * destined for.
 *
 * Caller must have the socket locked.
 */
static void
send_recvdone_event(isc_socket_t *sock, rwintev_t **iev,
		    isc_socketevent_t **dev, isc_result_t resultcode)
{
	REQUIRE(!EMPTY(sock->recv_list));
	REQUIRE(iev != NULL);
	REQUIRE(*iev != NULL);
	REQUIRE(dev != NULL);
	REQUIRE(*dev != NULL);

	DEQUEUE(sock->recv_list, *iev, link);
	(*dev)->result = resultcode;
	ISC_TASK_SEND((*iev)->task, (isc_event_t **)dev);
	(*iev)->done_ev = NULL;
	isc_event_free((isc_event_t **)iev);
}
static void
send_senddone_event(isc_socket_t *sock, rwintev_t **iev,
		    isc_socketevent_t **dev, isc_result_t resultcode)
{
	REQUIRE(!EMPTY(sock->send_list));
	REQUIRE(iev != NULL);
	REQUIRE(*iev != NULL);
	REQUIRE(dev != NULL);
	REQUIRE(*dev != NULL);

	DEQUEUE(sock->send_list, *iev, link);
	(*dev)->result = resultcode;
	ISC_TASK_SEND((*iev)->task, (isc_event_t **)dev);
	(*iev)->done_ev = NULL;
	isc_event_free((isc_event_t **)iev);
}

static void
send_ncdone_event(ncintev_t **iev,
		  isc_socket_newconnev_t **dev, isc_result_t resultcode)
{
	REQUIRE(iev != NULL);
	REQUIRE(*iev != NULL);
	REQUIRE(dev != NULL);
	REQUIRE(*dev != NULL);

	(*dev)->result = resultcode;
	ISC_TASK_SEND((*iev)->task, (isc_event_t **)dev);
	(*iev)->done_ev = NULL;

	isc_event_free((isc_event_t **)iev);
}

/*
 * Call accept() on a socket, to get the new file descriptor.  The listen
 * socket is used as a prototype to create a new isc_socket_t.  The new
 * socket is referenced twice (one for the task which is receiving this
 * message, and once for the message itself) so the task does not need to
 * attach to the socket again.  The task is not attached at all.
 */
static void
internal_accept(isc_task_t *task, isc_event_t *ev)
{
	isc_socket_t *sock;
	isc_socketmgr_t *manager;
	isc_socket_newconnev_t *dev;
	ncintev_t *iev;
	struct sockaddr addr;
	u_int addrlen;
	int fd;
	isc_result_t result = ISC_R_SUCCESS;

	sock = ev->sender;
	REQUIRE(VALID_SOCKET(sock));

	iev = (ncintev_t *)ev;
	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	LOCK(&sock->lock);
	XTRACE(TRACE_LISTEN,
	       ("internal_accept called, locked parent sock %p\n", sock));

	REQUIRE(sock->pending_accept);
	REQUIRE(sock->listener);
	REQUIRE(!EMPTY(sock->accept_list));
	REQUIRE(iev->task == task);

	sock->pending_accept = ISC_FALSE;

	/*
	 * Has this event been canceled?
	 */
	if (iev->canceled) {
		DEQUEUE(sock->accept_list, iev, link);
		isc_event_free((isc_event_t **)iev);
		if (!EMPTY(sock->accept_list))
			select_poke(sock->manager, sock->fd);

		UNLOCK(&sock->lock);

		return;
	}

	/*
	 * Try to accept the new connection.  If the accept fails with
	 * EAGAIN or EINTR, simply poke the watcher to watch this socket
	 * again.
	 */
	addrlen = sizeof(addr);
	fd = accept(sock->fd, &addr, &addrlen);
	if (fd < 0) {
		if (SOFT_ERROR(errno)) {
			select_poke(sock->manager, sock->fd);
			UNLOCK(&sock->lock);
			return;
		}

		/*
		 * If some other error, ignore it as well and hope
		 * for the best, but log it.
		 */
		XTRACE(TRACE_LISTEN, ("internal_accept: accept returned %s\n",
				      strerror(errno)));

		fd = -1;
		result = ISC_R_UNEXPECTED;
	}

	if (fd != -1 && (make_nonblock(fd) != ISC_R_SUCCESS)) {
		close(fd);
		fd = -1;

		result = ISC_R_UNEXPECTED;

		free_socket(&dev->newsocket);
	}

	DEQUEUE(sock->accept_list, iev, link);

	if (!EMPTY(sock->accept_list))
		select_poke(sock->manager, sock->fd);

	UNLOCK(&sock->lock);

	/*
	 * The accept succeeded.  Pull off the done event and set the
	 * fd and other information in the socket descriptor here.  These
	 * were preallocated for us.
	 */
	dev = iev->done_ev;
	iev->done_ev = NULL;

	/*
	 * -1 means the new socket didn't happen.
	 */
	if (fd != -1) {
		dev->newsocket->fd = fd;

		/*
		 * Save away the remote address
		 */
		dev->newsocket->addrlength = addrlen;
		memcpy(&dev->newsocket->address, &addr, addrlen);
		dev->addrlength = addrlen;
		memcpy(&dev->address, &addr, addrlen);

		LOCK(&manager->lock);
		manager->fds[fd] = dev->newsocket;
		manager->fdstate[fd] = MANAGED;
		if (manager->maxfd < fd)
			manager->maxfd = fd;
		manager->nsockets++;
		UNLOCK(&manager->lock);

		XTRACE(TRACE_LISTEN, ("internal_accept: newsock %p, fd %d\n",
				      dev->newsocket, fd));
	}

	send_ncdone_event(&iev, &dev, result);
}

static void
internal_recv(isc_task_t *task, isc_event_t *ev)
{
	rwintev_t *iev;
	isc_socketevent_t *dev;
	isc_socket_t *sock;
	int cc;
	size_t read_count;
	struct sockaddr addr;
	u_int addrlen;

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (isc_socket_t *)ev->sender;
	LOCK(&sock->lock);

	INSIST(sock->pending_recv == ISC_TRUE);
	sock->pending_recv = ISC_FALSE;

	XTRACE(TRACE_RECV,
	       ("internal_recv: sock %p, fd %d\n", sock, sock->fd));

	/*
	 * Pull the first entry off the list, and look at it.  If it is
	 * NULL, or not ours, something bad happened.
	 */
	iev = HEAD(sock->recv_list);
	INSIST(iev != NULL);
	INSIST(iev->task == task);

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.  If some sort of quantum read count is
	 * desired before giving up control, make certain to process markers
	 * regardless of quantum.
	 */
	do {
		iev = HEAD(sock->recv_list);
		dev = iev->done_ev;

		/*
		 * check for canceled I/O
		 */
		if (iev->canceled) {
			DEQUEUE(sock->recv_list, iev, link);
			isc_event_free((isc_event_t **)&iev);
			goto next;
		}

		/*
		 * If this is a marker event, post its completion and
		 * continue the loop.
		 */
		if (dev->common.type == ISC_SOCKEVENT_RECVMARK) {
			send_recvdone_event(sock, &iev, &dev,
					    sock->recv_result);
			goto next;
		}

		/*
		 * It must be a read request.  Try to satisfy it as best
		 * we can.
		 */
		read_count = dev->region.length - dev->n;
		if (sock->type == isc_socket_udp) {
			addrlen = sizeof(addr);
			cc = recvfrom(sock->fd, dev->region.base + dev->n,
				      read_count, 0,
				      (struct sockaddr *)&addr,
				      &addrlen);
			memcpy(&dev->address, &addr, addrlen);
			dev->addrlength = addrlen;
		} else {
			cc = recv(sock->fd, dev->region.base + dev->n,
				  read_count, 0);
			memcpy(&dev->address, &sock->address,
			       (size_t)sock->addrlength);
			dev->addrlength = sock->addrlength;
		}			

		XTRACE(TRACE_RECV,
		       ("internal_recv:  read(%d) %d\n", sock->fd, cc));

		/*
		 * check for error or block condition
		 */
		if (cc < 0) {
			if (SOFT_ERROR(errno))
				goto poke;

#define SOFT_OR_HARD(_system, _isc) \
	if (errno == _system) { \
		if (sock->connected) { \
			if (sock->type == isc_socket_tcp) \
				sock->recv_result = _isc; \
			send_recvdone_event(sock, &iev, &dev, _isc); \
		} \
		goto next; \
	}

			SOFT_OR_HARD(ECONNREFUSED, ISC_R_CONNREFUSED);
			SOFT_OR_HARD(ENETUNREACH, ISC_R_NETUNREACH);
			SOFT_OR_HARD(EHOSTUNREACH, ISC_R_HOSTUNREACH);
#undef SOFT_OR_HARD

			/*
			 * This might not be a permanent error.
			 */
			if (errno == ENOBUFS) {
				send_recvdone_event(sock, &iev, &dev,
						    ISC_R_NORESOURCES);

				goto next;
			}

			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal read: %s", strerror(errno));

			sock->recv_result = ISC_R_UNEXPECTED;  /* XXX */
			send_recvdone_event(sock, &iev, &dev,
					    ISC_R_UNEXPECTED); /* XXX */

			goto next;
		}

		/*
		 * read of 0 means the remote end was closed.  Run through
		 * the event queue and dispatch all the events with an EOF
		 * result code.  This will set the EOF flag in markers as
		 * well, but that's really ok.
		 */
		if (cc == 0) {
			do {
				send_recvdone_event(sock, &iev, &dev,
						    ISC_R_EOF);
				iev = HEAD(sock->recv_list);
			} while (iev != NULL);

			goto poke;
		}

		/*
		 * if we read less than we expected, update counters,
		 * poke.
		 */
		if ((size_t)cc < read_count) {
			dev->n += cc;

			/*
			 * If partial reads are allowed, we return whatever
			 * was read with a success result, and continue
			 * the loop.
			 */
			if (iev->partial) {
				send_recvdone_event(sock, &iev, &dev,
						    ISC_R_SUCCESS);
				goto next;
			}

			/*
			 * Partials not ok.  Exit the loop and notify the
			 * watcher to wait for more reads
			 */
			goto poke;
		}

		/*
		 * Exactly what we wanted to read.  We're done with this
		 * entry.  Post its completion event.
		 */
		if ((size_t)cc == read_count) {
			dev->n += read_count;
			send_recvdone_event(sock, &iev, &dev, ISC_R_SUCCESS);
		}

	next:
	} while (!EMPTY(sock->recv_list));

 poke:
	if (!EMPTY(sock->recv_list))
		select_poke(sock->manager, sock->fd);

	UNLOCK(&sock->lock);
}

static void
internal_send(isc_task_t *task, isc_event_t *ev)
{
	rwintev_t *iev;
	isc_socketevent_t *dev;
	isc_socket_t *sock;
	int cc;
	size_t write_count;

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (isc_socket_t *)ev->sender;
	LOCK(&sock->lock);

	INSIST(sock->pending_send == ISC_TRUE);
	sock->pending_send = ISC_FALSE;

	XTRACE(TRACE_SEND,
	       ("internal_send: sock %p, fd %d\n", sock, sock->fd));

	/*
	 * Pull the first entry off the list, and look at it.  If it is
	 * NULL, or not ours, something bad happened.
	 */
	iev = HEAD(sock->send_list);
	INSIST(iev != NULL);
	INSIST(iev->task == task);

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.  If some sort of quantum write count is
	 * desired before giving up control, make certain to process markers
	 * regardless of quantum.
	 */
	do {
		iev = HEAD(sock->send_list);
		dev = iev->done_ev;

		/*
		 * check for canceled I/O
		 */
		if (iev->canceled) {
			DEQUEUE(sock->send_list, iev, link);
			isc_event_free((isc_event_t **)&iev);
			goto next;
		}

		/*
		 * If this is a marker event, post its completion and
		 * continue the loop.
		 */
		if (dev->common.type == ISC_SOCKEVENT_SENDMARK) {
			send_senddone_event(sock, &iev, &dev,
					    sock->send_result);
			goto next;
		}

		/*
		 * It must be a write request.  Try to satisfy it as best
		 * we can.
		 */
		write_count = dev->region.length - dev->n;
		if (sock->type == isc_socket_udp)
			cc = sendto(sock->fd, dev->region.base + dev->n,
				    write_count, 0,
				    (struct sockaddr *)&dev->address,
				    (int)dev->addrlength);

		else
			cc = send(sock->fd, dev->region.base + dev->n,
				  write_count, 0);

		/*
		 * check for error or block condition
		 */
		if (cc < 0) {
			if (SOFT_ERROR(errno))
				goto poke;

#define SOFT_OR_HARD(_system, _isc) \
	if (errno == _system) { \
		if (sock->connected) { \
			if (sock->type == isc_socket_tcp) \
				sock->recv_result = _isc; \
			send_senddone_event(sock, &iev, &dev, _isc); \
		} \
		goto next; \
	}

			SOFT_OR_HARD(ECONNREFUSED, ISC_R_CONNREFUSED);
			SOFT_OR_HARD(ENETUNREACH, ISC_R_NETUNREACH);
			SOFT_OR_HARD(EHOSTUNREACH, ISC_R_HOSTUNREACH);
#undef SOFT_OR_HARD

			/*
			 * This might not be a permanent error.
			 */
			if (errno == ENOBUFS) {
				send_recvdone_event(sock, &iev, &dev,
						    ISC_R_NORESOURCES);

				goto next;
			}

			/*
			 * The other error types depend on wether or not the
			 * socket is UDP or TCP.  If it is UDP, some errors
			 * that we expect to be fatal under TCP are merely
			 * annoying, and are really soft errors.
			 *
			 * However, these soft errors are still returned as
			 * a status.
			 */
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_send: %s",
					 strerror(errno));
			sock->send_result = ISC_R_UNEXPECTED;
			send_senddone_event(sock, &iev, &dev,
					    ISC_R_UNEXPECTED);

			goto next;
		}

		if (cc == 0)
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_send: send() returned 0");

		/*
		 * if we write less than we expected, update counters,
		 * poke.
		 */
		if ((size_t)cc < write_count) {
			dev->n += cc;

			goto poke;
		}

		/*
		 * Exactly what we wanted to write.  We're done with this
		 * entry.  Post its completion event.
		 */
		if ((size_t)cc == write_count) {
			dev->n += write_count;
			send_senddone_event(sock, &iev, &dev, ISC_R_SUCCESS);

			goto next;
		}

	next:
	} while (!EMPTY(sock->send_list));

 poke:
	if (!EMPTY(sock->send_list))
		select_poke(sock->manager, sock->fd);

	UNLOCK(&sock->lock);
}

/*
 * This is the thread that will loop forever, always in a select or poll
 * call.
 *
 * When select returns something to do, track down what thread gets to do
 * this I/O and post the event to it.
 */
static isc_threadresult_t
WINAPI
watcher(void *uap)
{
	isc_socketmgr_t *manager = uap;
	isc_socket_t *sock;
	isc_boolean_t done;
	int ctlfd;
	int cc;
	fd_set readfds;
	fd_set writefds;
	int msg;
	isc_boolean_t unlock_sock;
	int i;
	rwintev_t *iev;
	ncintev_t *nciev;
	int maxfd;

	/*
	 * Get the control fd here.  This will never change.
	 */
	LOCK(&manager->lock);
	ctlfd = manager->pipe_fds[0];

	done = ISC_FALSE;
	while (!done) {
		do {
			readfds = manager->read_fds;
			writefds = manager->write_fds;
			maxfd = manager->maxfd + 1;

#ifdef ISC_SOCKET_DEBUG
			XTRACE(TRACE_WATCHER, ("select maxfd %d\n", maxfd));
			for (i = 0 ; i < FD_SETSIZE ; i++) {
				int printit;

				printit = 0;

				if (FD_ISSET(i, &readfds)) {
					printf("watcher: select r on %d\n", i);
					printit = 1;
				}
				if (FD_ISSET(i, &writefds)) {
					printf("watcher: select w on %d\n", i);
					printit = 1;
				}

				if (printit && manager->fds[i] != NULL)
					socket_dump(manager->fds[i]);
			}
#endif
					
			UNLOCK(&manager->lock);

			cc = select(maxfd, &readfds, &writefds, NULL, NULL);
			XTRACE(TRACE_WATCHER,
			       ("select(%d, ...) == %d, errno %d\n",
				maxfd, cc, errno));
			if (cc < 0) {
				if (!SOFT_ERROR(errno))
					FATAL_ERROR(__FILE__, __LINE__,
						    "select failed: %s",
						    strerror(errno));
			}

			LOCK(&manager->lock);
		} while (cc < 0);


		/*
		 * Process reads on internal, control fd.
		 */
		if (FD_ISSET(ctlfd, &readfds)) {
			while (1) {
				msg = select_readmsg(manager);

				XTRACE(TRACE_WATCHER,
				       ("watcher got message %d\n", msg));

				/*
				 * Nothing to read?
				 */
				if (msg == SELECT_POKE_NOTHING)
					break;

				/*
				 * handle shutdown message.  We really should
				 * jump out of this loop right away, but
				 * it doesn't matter if we have to do a little
				 * more work first.
				 */
				if (msg == SELECT_POKE_SHUTDOWN) {
					XTRACE(TRACE_WATCHER,
					       ("watcher got SHUTDOWN\n"));
					done = ISC_TRUE;

					break;
				}

				/*
				 * This is a wakeup on a socket.  Look
				 * at the event queue for both read and write,
				 * and decide if we need to watch on it now
				 * or not.
				 */
				if (msg >= 0) {
					INSIST(msg < FD_SETSIZE);

					if (manager->fdstate[msg] ==
					    CLOSE_PENDING) {
						manager->fdstate[msg] = CLOSED;
						FD_CLR(msg,
						       &manager->read_fds);
						FD_CLR(msg,
						       &manager->write_fds);

						close(msg);
						XTRACE(TRACE_WATCHER,
						       ("Watcher closed %d\n",
							msg));

						continue;
					}

					if (manager->fdstate[msg] != MANAGED)
						continue;

					sock = manager->fds[msg];

					LOCK(&sock->lock);
					XTRACE(TRACE_WATCHER,
					       ("watcher locked socket %p\n",
						sock));

					/*
					 * If there are no events, or there
					 * is an event but we have already
					 * queued up the internal event on a
					 * task's queue, clear the bit.
					 * Otherwise, set it.
					 */
					iev = HEAD(sock->recv_list);
					nciev = HEAD(sock->accept_list);
					if ((iev == NULL && nciev == NULL)
					    || sock->pending_recv
					    || sock->pending_accept) {
						FD_CLR(sock->fd,
						       &manager->read_fds);
						XTRACE(TRACE_WATCHER,
						       ("watch cleared r\n"));
					} else {
						FD_SET(sock->fd,
						       &manager->read_fds);
						XTRACE(TRACE_WATCHER,
						       ("watch set r\n"));
					}

					iev = HEAD(sock->send_list);
					if ((iev == NULL
					     || sock->pending_send)
					    && !sock->connecting) {
						FD_CLR(sock->fd,
						       &manager->write_fds);
						XTRACE(TRACE_WATCHER,
						       ("watch cleared w\n"));
					} else {
						FD_SET(sock->fd,
						       &manager->write_fds);
						XTRACE(TRACE_WATCHER,
						       ("watch set w\n"));
					}

					UNLOCK(&sock->lock);
				}
			}
		}

		/*
		 * Process read/writes on other fds here.  Avoid locking
		 * and unlocking twice if both reads and writes are possible.
		 */
		for (i = 0 ; i < maxfd ; i++) {
			if (i == manager->pipe_fds[0]
			    || i == manager->pipe_fds[1])
				continue;

			if (manager->fdstate[i] == CLOSE_PENDING) {
				manager->fdstate[i] = CLOSED;
				FD_CLR(i, &manager->read_fds);
				FD_CLR(i, &manager->write_fds);
				
				close(i);
				XTRACE(TRACE_WATCHER,
				       ("Watcher closed %d\n", i));
				
				continue;
			}

			sock = manager->fds[i];
			unlock_sock = ISC_FALSE;
			if (FD_ISSET(i, &readfds)) {
				if (sock == NULL) {
					FD_CLR(i, &manager->read_fds);
					goto check_write;
				}
				XTRACE(TRACE_WATCHER,
				       ("watcher r on %d, sock %p\n",
					i, manager->fds[i]));
				unlock_sock = ISC_TRUE;
				LOCK(&sock->lock);
				if (sock->listener)
					dispatch_listen(sock);
				else
					dispatch_read(sock);
				FD_CLR(i, &manager->read_fds);
			}
		check_write:
			if (FD_ISSET(i, &writefds)) {
				if (sock == NULL) {
					FD_CLR(i, &manager->write_fds);
					continue;
				}
				XTRACE(TRACE_WATCHER,
				       ("watcher w on %d, sock %p\n",
					i, manager->fds[i]));
				if (!unlock_sock) {
					unlock_sock = ISC_TRUE;
					LOCK(&sock->lock);
				}
				if (sock->connecting)
					dispatch_connect(sock);
				else
					dispatch_write(sock);
				FD_CLR(i, &manager->write_fds);
			}
			if (unlock_sock)
				UNLOCK(&sock->lock);
		}
	}

	XTRACE(TRACE_WATCHER, ("Watcher exiting\n"));

	UNLOCK(&manager->lock);
	return ((isc_threadresult_t)0);
}

/*
 * Create a new socket manager.
 */
isc_result_t
isc_socketmgr_create(isc_memctx_t *mctx, isc_socketmgr_t **managerp)
{
	isc_socketmgr_t *manager;

	REQUIRE(managerp != NULL && *managerp == NULL);

	XENTER(TRACE_MANAGER, "isc_socketmgr_create");

	manager = isc_mem_get(mctx, sizeof *manager);
	if (manager == NULL)
		return (ISC_R_NOMEMORY);
	
	manager->magic = SOCKET_MANAGER_MAGIC;
	manager->mctx = mctx;
	memset(manager->fds, 0, sizeof(manager->fds));
	manager->nsockets = 0;
	if (isc_mutex_init(&manager->lock) != ISC_R_SUCCESS) {
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed");
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * Create the special fds that will be used to wake up the
	 * select/poll loop when something internal needs to be done.
	 */
	if (pipe(manager->pipe_fds) != 0) {
		(void)isc_mutex_destroy(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "pipe() failed: %s",
				 strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	INSIST(make_nonblock(manager->pipe_fds[0]) == ISC_R_SUCCESS);
	INSIST(make_nonblock(manager->pipe_fds[1]) == ISC_R_SUCCESS);

	/*
	 * Set up initial state for the select loop
	 */
	FD_ZERO(&manager->read_fds);
	FD_ZERO(&manager->write_fds);
	FD_SET(manager->pipe_fds[0], &manager->read_fds);
	manager->maxfd = manager->pipe_fds[0];
	memset(manager->fdstate, 0, sizeof(manager->fdstate));

	/*
	 * Start up the select/poll thread.
	 */
	if (isc_thread_create(watcher, manager, &manager->watcher) !=
	    ISC_R_SUCCESS) {
		(void)isc_mutex_destroy(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_create() failed");
		close(manager->pipe_fds[0]);
		close(manager->pipe_fds[1]);
		return (ISC_R_UNEXPECTED);
	}

	*managerp = manager;

	XEXIT(TRACE_MANAGER, "isc_socketmgr_create (normal)");
	return (ISC_R_SUCCESS);
}

void
isc_socketmgr_destroy(isc_socketmgr_t **managerp)
{
	isc_socketmgr_t *manager;
	int i;

	/*
	 * Destroy a socket manager.
	 */

	REQUIRE(managerp != NULL);
	manager = *managerp;
	REQUIRE(VALID_MANAGER(manager));

	LOCK(&manager->lock);
	XTRACE(TRACE_MANAGER, ("nsockets == %d\n", manager->nsockets));
	REQUIRE(manager->nsockets == 0);
	UNLOCK(&manager->lock);

	/*
	 * Here, poke our select/poll thread.  Do this by closing the write
	 * half of the pipe, which will send EOF to the read half.
	 */
	select_poke(manager, SELECT_POKE_SHUTDOWN);

	/*
	 * Wait for thread to exit.
	 */
	if (isc_thread_join(manager->watcher, NULL) != ISC_R_SUCCESS)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_join() failed");

	/*
	 * Clean up.
	 */
	close(manager->pipe_fds[0]);
	close(manager->pipe_fds[1]);

	for (i = 0 ; i < FD_SETSIZE ; i++)
		if (manager->fdstate[i] == CLOSE_PENDING)
			close(i);

	(void)isc_mutex_destroy(&manager->lock);
	manager->magic = 0;
	isc_mem_put(manager->mctx, manager, sizeof *manager);

	*managerp = NULL;
}

isc_result_t
isc_socket_recv(isc_socket_t *sock, isc_region_t *region,
		isc_boolean_t partial, isc_task_t *task,
		isc_taskaction_t action, void *arg)
{
	isc_socketevent_t *ev;
	rwintev_t *iev;
	isc_socketmgr_t *manager;
	isc_task_t *ntask = NULL;
	int cc;

	manager = sock->manager;

	ev = (isc_socketevent_t *)isc_event_allocate(manager->mctx, sock,
						     ISC_SOCKEVENT_RECVDONE,
						     action, arg, sizeof(*ev));
	if (ev == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&sock->lock);

	if (sock->riev == NULL) {
		iev = (rwintev_t *)isc_event_allocate(manager->mctx,
						      sock,
						      ISC_SOCKEVENT_INTRECV,
						      internal_recv,
						      sock,
						      sizeof(*iev));
		if (iev == NULL) {
			/* no special free routine yet */
			isc_event_free((isc_event_t **)&ev);
			UNLOCK(&sock->lock);
			return (ISC_R_NOMEMORY);
		}

		INIT_LINK(iev, link);
		iev->posted = ISC_FALSE;


		sock->riev = iev;
		iev = NULL;  /* just in case */
	}

	sock->references++;  /* attach to socket in cheap way */

	/*
	 * Remember that we need to detach on event free
	 */
	ev->common.destroy = done_event_destroy;

	/*
	 * UDP sockets are always partial read
	 */
	if (sock->type == isc_socket_udp)
		partial = ISC_TRUE;

	ev->region = *region;
	ev->n = 0;
	ev->result = ISC_R_SUCCESS;

	/*
	 * If the read queue is empty, try to do the I/O right now.
	 */
	if (EMPTY(sock->recv_list)) {
		if (sock->type == isc_socket_udp) {
			ev->addrlength = sizeof(isc_sockaddr_t);
			cc = recvfrom(sock->fd, ev->region.base,
				      ev->region.length, 0,
				      (struct sockaddr *)&ev->address,
				      &ev->addrlength);
		} else {
			cc = recv(sock->fd, ev->region.base,
				  ev->region.length, 0);
			ev->address = sock->address;
			ev->addrlength = sock->addrlength;
		}

		if (cc < 0) {
			if (SOFT_ERROR(errno))
				goto queue;

			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_socket_recv: %s",
					 strerror(errno));

			sock->recv_result = ISC_R_UNEXPECTED;  /* XXX */

			ev->result = ISC_R_UNEXPECTED; /* XXX */
			ISC_TASK_SEND(task, (isc_event_t **)&ev);

			UNLOCK(&sock->lock);
			return (ISC_R_SUCCESS);
		}

		if (cc == 0) {
			ev->result = ISC_R_EOF;
			ISC_TASK_SEND(task, (isc_event_t **)&ev);

			UNLOCK(&sock->lock);
			return (ISC_R_SUCCESS);
		}

		ev->n = cc;

		/*
		 * Partial reads need to be queued
		 */
		if ((size_t)cc != ev->region.length && !partial)
			goto queue;

		/*
		 * full reads are posted, or partials if partials are ok.
		 */
		ISC_TASK_SEND(task, (isc_event_t **)&ev);

		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}

	/*
	 * We couldn't read all or part of the request right now, so queue
	 * it.
	 */
 queue:
	iev = sock->riev;
	sock->riev = NULL;

	isc_task_attach(task, &ntask);

	iev->done_ev = ev;
	iev->task = ntask;
	iev->partial = partial;

	/*
	 * Enqueue the request.  If the socket was previously not being
	 * watched, poke the watcher to start paying attention to it.
	 */
	if (EMPTY(sock->recv_list)) {
		ENQUEUE(sock->recv_list, iev, link);
		select_poke(sock->manager, sock->fd);
	} else {
		ENQUEUE(sock->recv_list, iev, link);
	}

	XTRACE(TRACE_RECV,
	       ("isc_socket_recv: posted ievent %p, dev %p, task %p\n",
		iev, iev->done_ev, task));

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_send(isc_socket_t *sock, isc_region_t *region,
		isc_task_t *task, isc_taskaction_t action, void *arg)
{
	return isc_socket_sendto(sock, region, task, action, arg, NULL, 0);
}

isc_result_t
isc_socket_sendto(isc_socket_t *sock, isc_region_t *region,
		  isc_task_t *task, isc_taskaction_t action, void *arg,
		  isc_sockaddr_t *address, unsigned int addrlength)
{
	isc_socketevent_t *ev;
	rwintev_t *iev;
	isc_socketmgr_t *manager;
	isc_task_t *ntask = NULL;
	int cc;

	manager = sock->manager;

	ev = (isc_socketevent_t *)isc_event_allocate(manager->mctx, sock,
						     ISC_SOCKEVENT_SENDDONE,
						     action, arg, sizeof(*ev));
	if (ev == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&sock->lock);

	if (sock->wiev == NULL) {
		iev = (rwintev_t *)isc_event_allocate(manager->mctx,
						      sock,
						      ISC_SOCKEVENT_INTSEND,
						      internal_send,
						      sock,
						      sizeof(*iev));
		if (iev == NULL) {
			/* no special free routine yet */
			isc_event_free((isc_event_t **)&ev);
			UNLOCK(&sock->lock);
			return (ISC_R_NOMEMORY);
		}

		INIT_LINK(iev, link);
		iev->posted = ISC_FALSE;

		sock->wiev = iev;
		iev = NULL;  /* just in case */
	}

	sock->references++;  /* attach to socket in cheap way */

	/*
	 * Remember that we need to detach on event free
	 */
	ev->common.destroy = done_event_destroy;

	ev->region = *region;
	ev->n = 0;
	ev->result = ISC_R_SUCCESS;

	/*
	 * If the write queue is empty, try to do the I/O right now.
	 */
	if (sock->type == isc_socket_udp) {
		INSIST(addrlength > 0 || sock->addrlength > 0);
		if (addrlength > 0) {
			ev->address = *address;
			ev->addrlength = addrlength;
		} else if (sock->addrlength > 0) {
			ev->address = sock->address;
			ev->addrlength = sock->addrlength;
		}
	} else if (sock->type == isc_socket_tcp) {
		INSIST(address == NULL);
		INSIST(addrlength == 0);
		ev->address = sock->address;
		ev->addrlength = sock->addrlength;
	}

	if (EMPTY(sock->send_list)) {
		if (sock->type == isc_socket_udp)
			cc = sendto(sock->fd, ev->region.base,
				    ev->region.length, 0,
				    (struct sockaddr *)&ev->address,
				    (int)ev->addrlength);
		else if (sock->type == isc_socket_tcp)
			cc = send(sock->fd, ev->region.base,
				  ev->region.length, 0);
		else {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_socket_send: "
					 "unknown socket type");
			UNLOCK(&sock->lock);
			return (ISC_R_UNEXPECTED);
		}

		if (cc < 0) {
			if (SOFT_ERROR(errno))
				goto queue;

			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_socket_send: %s",
					 strerror(errno));

			sock->send_result = ISC_R_UNEXPECTED;

			UNLOCK(&sock->lock);
			return (ISC_R_UNEXPECTED);
		}

		if (cc == 0) {
			ev->result = ISC_R_EOF;
			ISC_TASK_SEND(task, (isc_event_t **)&ev);

			UNLOCK(&sock->lock);
			return (ISC_R_SUCCESS);
		}

		ev->n = cc;

		/*
		 * Partial writes need to be queued
		 */
		if ((size_t)cc != ev->region.length)
			goto queue;

		/*
		 * full writes are posted.
		 */
		ISC_TASK_SEND(task, (isc_event_t **)&ev);

		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}

	/*
	 * We couldn't send all or part of the request right now, so queue
	 * it.
	 */
 queue:
	iev = sock->wiev;
	sock->wiev = NULL;

	isc_task_attach(task, &ntask);

	iev->done_ev = ev;
	iev->task = ntask;
	iev->partial = ISC_FALSE; /* doesn't matter */

	/*
	 * Enqueue the request.  If the socket was previously not being
	 * watched, poke the watcher to start paying attention to it.
	 */
	if (EMPTY(sock->send_list)) {
		ENQUEUE(sock->send_list, iev, link);
		select_poke(sock->manager, sock->fd);
	} else {
		ENQUEUE(sock->send_list, iev, link);
	}

	XTRACE(TRACE_SEND,
	       ("isc_socket_send: posted ievent %p, dev %p, task %p\n",
		iev, iev->done_ev, task));

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_bind(isc_socket_t *sock, isc_sockaddr_t *sockaddr,
		int addrlen)
{
	int on = 1;

	LOCK(&sock->lock);

	if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
		       &on, sizeof on) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__, "setsockopt(%d) failed",
				 sock->fd);
		/* Press on... */
	}
	if (bind(sock->fd, (struct sockaddr *)sockaddr, addrlen) < 0) {
		UNLOCK(&sock->lock);
		switch (errno) {
		case EACCES:
			return (ISC_R_NOPERM);
			break;
		case EADDRNOTAVAIL:
			return (ISC_R_ADDRNOTAVAIL);
			break;
		case EADDRINUSE:
			return (ISC_R_ADDRINUSE);
			break;
		case EINVAL:
			return (ISC_R_BOUND);
			break;
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "bind: %s", strerror(errno));
			return (ISC_R_UNEXPECTED);
			break;
		}
	}

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

/*
 * set up to listen on a given socket.  We do this by creating an internal
 * event that will be dispatched when the socket has read activity.  The
 * watcher will send the internal event to the task when there is a new
 * connection.
 *
 * Unlike in read, we don't preallocate a done event here.  Every time there
 * is a new connection we'll have to allocate a new one anyway, so we might
 * as well keep things simple rather than having to track them.
 */
isc_result_t
isc_socket_listen(isc_socket_t *sock, unsigned int backlog)
{
	REQUIRE(VALID_SOCKET(sock));

	LOCK(&sock->lock);

	REQUIRE(!sock->listener);
	REQUIRE(sock->type == isc_socket_tcp);

	if (backlog == 0)
		backlog = SOMAXCONN;

	if (listen(sock->fd, (int)backlog) < 0) {
		UNLOCK(&sock->lock);
		UNEXPECTED_ERROR(__FILE__, __LINE__, "listen: %s",
				 strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	sock->listener = ISC_TRUE;
	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

/*
 * This should try to do agressive accept()
 */
isc_result_t
isc_socket_accept(isc_socket_t *sock,
		  isc_task_t *task, isc_taskaction_t action, void *arg)
{
	ncintev_t *iev;
	isc_socket_newconnev_t *dev;
	isc_task_t *ntask = NULL;
	isc_socketmgr_t *manager;
	isc_socket_t *nsock;
	isc_result_t ret;

	XENTER(TRACE_LISTEN, "isc_socket_accept");

	REQUIRE(VALID_SOCKET(sock));
	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	LOCK(&sock->lock);

	REQUIRE(sock->listener);

	iev = (ncintev_t *)isc_event_allocate(manager->mctx, sock,
					      ISC_SOCKEVENT_INTACCEPT,
					      internal_accept, sock,
					      sizeof(*iev));
	if (iev == NULL) {
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}

	iev->posted = ISC_FALSE;

	dev = (isc_socket_newconnev_t *)
		isc_event_allocate(manager->mctx,
				   sock,
				   ISC_SOCKEVENT_NEWCONN,
				   action,
				   arg,
				   sizeof (*dev));
	if (dev == NULL) {
		UNLOCK(&sock->lock);
		isc_event_free((isc_event_t **)&iev);
		return (ISC_R_NOMEMORY);
	}


	ret = allocate_socket(manager, sock->type, &nsock);
	if (ret != ISC_R_SUCCESS) {
		UNLOCK(&sock->lock);
		isc_event_free((isc_event_t **)&iev);
		isc_event_free((isc_event_t **)&dev);
		return (ret);
	}

	INIT_LINK(iev, link);

	/*
	 * Attach to socket and to task
	 */
	isc_task_attach(task, &ntask);
	sock->references++;
	nsock->references++;

	sock->listener = ISC_TRUE;

	iev->task = ntask;
	iev->done_ev = dev;
	iev->canceled = ISC_FALSE;
	dev->common.destroy = done_event_destroy;
	dev->newsocket = nsock;

	/*
	 * poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (EMPTY(sock->accept_list))
		select_poke(manager, sock->fd);

	ENQUEUE(sock->accept_list, iev, link);

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_connect(isc_socket_t *sock, isc_sockaddr_t *addr, int addrlen,
		   isc_task_t *task, isc_taskaction_t action, void *arg)
{
	isc_socket_connev_t *dev;
	isc_task_t *ntask = NULL;
	isc_socketmgr_t *manager;
	int cc;

	XENTER(TRACE_CONNECT, "isc_socket_connect");
	REQUIRE(VALID_SOCKET(sock));
	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));
	REQUIRE(addr != NULL);

	LOCK(&sock->lock);

	REQUIRE(!sock->connecting);

	if (sock->ciev == NULL) {
		sock->ciev = (cnintev_t *)
			isc_event_allocate(manager->mctx,
					   sock,
					   ISC_SOCKEVENT_INTCONN,
					   internal_connect,
					   sock,
					   sizeof(*(sock->ciev)));
		if (sock->ciev == NULL) {
			UNLOCK(&sock->lock);
			return (ISC_R_NOMEMORY);
		}

		sock->ciev->posted = ISC_FALSE;
	}

	dev = (isc_socket_connev_t *)isc_event_allocate(manager->mctx,
							sock,
							ISC_SOCKEVENT_CONNECT,
							action,
							arg,
							sizeof (*dev));
	if (dev == NULL) {
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}

	/*
	 * Try to do the connect right away, as there can be only one
	 * outstanding, and it might happen to complete.
	 */
	sock->address = *addr;
	sock->addrlength = addrlen;
	cc = connect(sock->fd, (struct sockaddr *)addr, addrlen);
	if (cc < 0) {
		if (SOFT_ERROR(errno) || errno == EINPROGRESS)
			goto queue;

		sock->connected = ISC_FALSE;

		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "%s", strerror(errno));

		UNLOCK(&sock->lock);
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * attach to socket
	 */
	sock->references++;
	dev->common.destroy = done_event_destroy;

	/*
	 * If connect completed, fire off the done event
	 */
	if (cc == 0) {
		sock->connected = ISC_TRUE;
		dev->result = ISC_R_SUCCESS;
		ISC_TASK_SEND(task, (isc_event_t **)&dev);
		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}

 queue:

	XTRACE(TRACE_CONNECT, ("queueing connect internal event\n"));
	/*
	 * Attach to to task
	 */
	isc_task_attach(task, &ntask);

	sock->connecting = ISC_TRUE;

	sock->ciev->task = ntask;
	sock->ciev->done_ev = dev;
	sock->ciev->canceled = ISC_FALSE;

	/*
	 * poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (sock->connect_ev == NULL)
		select_poke(manager, sock->fd);

	sock->connect_ev = sock->ciev;
	sock->ciev = NULL;

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

/*
 * Called when a socket with a pending connect() finishes.
 */
static void
internal_connect(isc_task_t *task, isc_event_t *ev)
{
	isc_socket_t *sock;
	isc_socket_connev_t *dev;
	cnintev_t *iev;
	int cc;
	int optlen;

	sock = ev->sender;
	iev = (cnintev_t *)ev;

	REQUIRE(VALID_SOCKET(sock));

	LOCK(&sock->lock);
	XTRACE(TRACE_CONNECT,
	       ("internal_connect called, locked parent sock %p\n", sock));

	REQUIRE(sock->connecting);
	REQUIRE(sock->connect_ev == (cnintev_t *)ev);
	REQUIRE(iev->task == task);

	sock->connecting = ISC_FALSE;

	/*
	 * Has this event been canceled?
	 */
	if (iev->canceled) {
		isc_event_free((isc_event_t **)(sock->connect_ev));

		UNLOCK(&sock->lock);

		return;
	}

	dev = iev->done_ev;

	/*
	 * Get any possible error status here.
	 */
	optlen = sizeof(cc);
	if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR,
		       (char *)&cc, &optlen) < 0)
		cc = errno;
	else
		errno = cc;

	if (errno != 0) {
		/*
		 * If the error is EAGAIN, just re-select on this
		 * fd and pretend nothing strange happened.
		 */
		if (SOFT_ERROR(errno) || errno == EINPROGRESS) {
			sock->connecting = ISC_TRUE;
			select_poke(sock->manager, sock->fd);
			UNLOCK(&sock->lock);

			return;
		}

		/*
		 * Translate other errors into ISC_R_* flavors.
		 */
		switch (errno) {
		case ETIMEDOUT:
			dev->result = ISC_R_TIMEDOUT;
			break;
		case ECONNREFUSED:
			dev->result = ISC_R_CONNREFUSED;
			break;
		case ENETUNREACH:
			dev->result = ISC_R_NETUNREACH;
			break;
		default:
			dev->result = ISC_R_UNEXPECTED;
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_connect: connect() %s",
					 strerror(errno));
			break;
		}
	}


	UNLOCK(&sock->lock);

	ISC_TASK_SEND(iev->task, (isc_event_t **)&dev);
	iev->done_ev = NULL;
	isc_event_free((isc_event_t **)&iev);
}

isc_result_t
isc_socket_getpeername(isc_socket_t *sock, isc_sockaddr_t *addressp,
		       int *lengthp)
{
	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(addressp != NULL);
	REQUIRE(lengthp != NULL);

	LOCK(&sock->lock);

	if (*lengthp < sock->addrlength) {
		UNLOCK(&sock->lock);
		return (ISC_R_TOOSMALL);
	}

	memcpy(addressp, &sock->address, (size_t)sock->addrlength);
	*lengthp = sock->addrlength;

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_getsockname(isc_socket_t *sock, isc_sockaddr_t *addressp,
		       int *lengthp)
{
	isc_sockaddr_t addr;
	int len;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(addressp != NULL);
	REQUIRE(lengthp != NULL);

	LOCK(&sock->lock);

	len = sizeof(addr);
	if (getsockname(sock->fd, (struct sockaddr *)&addr, &len) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "getsockname: %s", strerror(errno));
		UNLOCK(&sock->lock);
		return (ISC_R_UNEXPECTED);
	}

	if (*lengthp < sock->addrlength) {
		UNLOCK(&sock->lock);
		return (ISC_R_TOOSMALL);
	}

	memcpy(addressp, &sock->address, (size_t)sock->addrlength);
	*lengthp = sock->addrlength;

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

/*
 * Run through the list of events on this socket, and cancel the ones
 * queued for task "task" of type "how".  "how" is a bitmask.
 */
void
isc_socket_cancel(isc_socket_t *sock, isc_task_t *task,
		  unsigned int how)
{
	isc_boolean_t poke_needed;

	REQUIRE(VALID_SOCKET(sock));

	/*
	 * Quick exit if there is nothing to do.  Don't even bother locking
	 * in this case.
	 */
	if (how == 0)
		return;

	poke_needed = ISC_FALSE;

	LOCK(&sock->lock);

	/*
	 * All of these do the same thing, more or less.
	 * Each will:
	 *	o If the internal event is marked as "posted" try to
	 *	  remove it from the task's queue.  If this fails, mark it
	 *	  as canceled instead, and let the task clean it up later.
	 *	o For each I/O request for that task of that type, post
	 *	  its done event with status of "ISC_R_CANCELED".
	 *	o Reset any state needed.
	 */
	if ((how & ISC_SOCKCANCEL_RECV) && HEAD(sock->recv_list) != NULL) {
		rwintev_t *		iev;
		rwintev_t *		next;
		isc_socketevent_t *	dev;

		iev = HEAD(sock->recv_list);

		/*
		 * If the internal event was posted, try to remove
		 * it from the task's queue.  If this fails,
		 * set the canceled flag, post the done event, and
		 * point "iev" to the next item on the list, and enter
		 * the while loop.  Otherwise, just enter the while loop
		 * and let it dispatch the done event.
		 */
		if ((task == NULL || task == iev->task)
		    && iev->posted && !iev->canceled) {
			if (isc_task_purge(task, sock,
					   ISC_SOCKEVENT_INTRECV) == 0) {
					iev->canceled = ISC_TRUE;
					/*
					 * pull off the done event and post it.
					 */
					dev = iev->done_ev;
					iev->done_ev = NULL;
					dev->result = ISC_R_CANCELED;
					ISC_TASK_SEND(iev->task,
						      (isc_event_t **)&dev);

					iev = NEXT(iev, link);
			}
		}

		/*
		 * run through the event queue, posting done events with the
		 * canceled result, and freeing the internal event.
		 */
		while (iev != NULL) {
			next = NEXT(iev, link);

			if (task == NULL || task == iev->task)
				send_recvdone_event(sock, &iev,
						    &iev->done_ev,
						    ISC_R_CANCELED);

			iev = next;
		}
	}

	if (how & ISC_SOCKCANCEL_SEND) {
	}

	if ((how & ISC_SOCKCANCEL_ACCEPT) && HEAD(sock->accept_list) != NULL) {
		ncintev_t *		iev;
		ncintev_t *		next;
		isc_socket_newconnev_t *dev;

		iev = HEAD(sock->accept_list);

		if ((task == NULL || task == iev->task)
		    && iev->posted && !iev->canceled) {
			if (isc_task_purge(task, sock,
					   ISC_SOCKEVENT_INTACCEPT) == 0) {
					iev->canceled = ISC_TRUE;
					dev = iev->done_ev;
					iev->done_ev = NULL;
					dev->result = ISC_R_CANCELED;
					dev->newsocket->references--;
					free_socket(&dev->newsocket);
					ISC_TASK_SEND(iev->task,
						      (isc_event_t **)&dev);

					iev = NEXT(iev, link);
			}
		}

		while (iev != NULL) {
			next = NEXT(iev, link);

			if (task == NULL || task == iev->task) {
				dev = iev->done_ev;
				iev->done_ev = NULL;
				dev->newsocket->references--;
				free_socket(&dev->newsocket);
				DEQUEUE(sock->accept_list, iev, link);
				send_ncdone_event(&iev, &dev, ISC_R_CANCELED);
			}

			iev = next;
		}
	}

	if (how & ISC_SOCKCANCEL_CONNECT) {
	}

	/*
	 * Need to guess if we need to poke or not... XXX
	 */
	select_poke(sock->manager, sock->fd);

	UNLOCK(&sock->lock);
}

isc_result_t
isc_socket_recvmark(isc_socket_t *sock,
		    isc_task_t *task, isc_taskaction_t action, void *arg)
{
	isc_socketevent_t *dev;
	rwintev_t *iev;
	isc_socketmgr_t *manager;
	isc_task_t *ntask = NULL;

	manager = sock->manager;

	dev = (isc_socketevent_t *)isc_event_allocate(manager->mctx, sock,
						      ISC_SOCKEVENT_RECVMARK,
						      action, arg,
						      sizeof(*dev));
	if (dev == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&sock->lock);

	/*
	 * If the queue is empty, simply return the last error we got on
	 * this socket as the result code, and send off the done event.
	 */
	if (EMPTY(sock->recv_list)) {
		dev->result = sock->recv_result;

		dev->common.destroy = done_event_destroy;
		sock->references++;

		ISC_TASK_SEND(task, (isc_event_t **)&dev);

		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}

	/*
	 * Bad luck.  The queue wasn't empty.  Insert this in the proper
	 * place.
	 */
	iev = (rwintev_t *)isc_event_allocate(manager->mctx,
					      sock,
					      ISC_SOCKEVENT_INTRECV,
					      internal_recv,
					      sock,
					      sizeof(*iev));

	if (iev == NULL) {
		isc_event_free((isc_event_t **)&dev);
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}

	INIT_LINK(iev, link);
	iev->posted = ISC_FALSE;

	sock->references++;
	dev->common.destroy = done_event_destroy;
	dev->result = ISC_R_SUCCESS;

	isc_task_attach(task, &ntask);

	iev->done_ev = dev;
	iev->task = ntask;
	iev->partial = ISC_FALSE; /* doesn't matter */

	ENQUEUE(sock->send_list, iev, link);

	XTRACE(TRACE_RECV,
	       ("isc_socket_recvmark: posted ievent %p, dev %p, task %p\n",
		iev, dev, task));

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_sendmark(isc_socket_t *sock,
		    isc_task_t *task, isc_taskaction_t action, void *arg)
{
	isc_socketevent_t *dev;
	rwintev_t *iev;
	isc_socketmgr_t *manager;
	isc_task_t *ntask = NULL;

	manager = sock->manager;

	dev = (isc_socketevent_t *)isc_event_allocate(manager->mctx, sock,
						      ISC_SOCKEVENT_SENDMARK,
						      action, arg,
						      sizeof(*dev));
	if (dev == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&sock->lock);

	/*
	 * If the queue is empty, simply return the last error we got on
	 * this socket as the result code, and send off the done event.
	 */
	if (EMPTY(sock->send_list)) {
		dev->result = sock->send_result;

		dev->common.destroy = done_event_destroy;
		sock->references++;

		ISC_TASK_SEND(task, (isc_event_t **)&dev);

		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}

	/*
	 * Bad luck.  The queue wasn't empty.  Insert this in the proper
	 * place.
	 */
	iev = (rwintev_t *)isc_event_allocate(manager->mctx,
					      sock,
					      ISC_SOCKEVENT_INTSEND,
					      internal_send,
					      sock,
					      sizeof(*iev));

	if (iev == NULL) {
		isc_event_free((isc_event_t **)&dev);
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}

	INIT_LINK(iev, link);
	iev->posted = ISC_FALSE;

	sock->references++;
	dev->common.destroy = done_event_destroy;
	dev->result = ISC_R_SUCCESS;

	isc_task_attach(task, &ntask);

	iev->done_ev = dev;
	iev->task = ntask;
	iev->partial = ISC_FALSE; /* doesn't matter */

	ENQUEUE(sock->send_list, iev, link);

	XTRACE(TRACE_SEND,
	       ("isc_socket_sendmark: posted ievent %p, dev %p, task %p\n",
		iev, dev, task));

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

