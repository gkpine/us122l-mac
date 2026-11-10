/* Connecting to the us122ld daemon and mapping its shared memory (used by
 * the Core Audio plug-in and the test tool). */
#ifndef US122L_CLIENT_H
#define US122L_CLIENT_H

#include "shared.h"
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <errno.h>
#include <signal.h>

/* Returns the mapped block, or NULL if the daemon isn't reachable. */
static inline us122l_shm_t *us122l_connect(void)
{
    mach_port_t svc = MACH_PORT_NULL, reply = MACH_PORT_NULL;
    us122l_shm_t *shm = NULL;
    if (bootstrap_look_up(bootstrap_port, US122L_SERVICE, &svc) != KERN_SUCCESS)
        return NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS)
        goto out;

    us122l_request_t req;
    memset(&req, 0, sizeof req);
    req.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.header.msgh_size = sizeof req;
    req.header.msgh_remote_port = svc;
    req.header.msgh_local_port = reply;
    req.header.msgh_id = 0x5531;
    req.op = US122L_MSG_GET_SHM;
    if (mach_msg(&req.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof req, 0, MACH_PORT_NULL,
                 1000, MACH_PORT_NULL) != KERN_SUCCESS)
        goto out;

    struct {
        us122l_reply_t r;
        mach_msg_trailer_t trailer;
    } rep;
    memset(&rep, 0, sizeof rep);
    if (mach_msg(&rep.r.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof rep, reply, 2000,
                 MACH_PORT_NULL) != KERN_SUCCESS)
        goto out;
    if (!(rep.r.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || rep.r.body.msgh_descriptor_count != 1 ||
        rep.r.size < sizeof(us122l_shm_t)) {
        mach_msg_destroy(&rep.r.header);
        goto out;
    }
    mach_vm_address_t addr = 0;
    kern_return_t kr = mach_vm_map(mach_task_self(), &addr, rep.r.size, 0, VM_FLAGS_ANYWHERE,
                                   rep.r.memory.name, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                                   VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    mach_port_deallocate(mach_task_self(), rep.r.memory.name);
    if (kr != KERN_SUCCESS)
        goto out;
    shm = (us122l_shm_t *)(uintptr_t)addr;
    if (shm->magic != US122L_SHM_MAGIC || shm->version != US122L_SHM_VERSION) {
        mach_vm_deallocate(mach_task_self(), addr, rep.r.size);
        shm = NULL;
    }
out:
    if (reply != MACH_PORT_NULL)
        mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), svc);
    return shm;
}

/* Is the daemon that gave us this block still alive? */
static inline int us122l_daemon_alive(us122l_shm_t *shm)
{
    pid_t pid = (pid_t)atomic_load(&shm->daemon_pid);
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

#endif
