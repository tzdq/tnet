#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "sys/queue.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "event2/event.h"
#include "event2/event_struct.h"
#include "event_internal.h"
#include "event2/util.h"
#include "evsignal.h"
#include "evlog.h"
#include "evmap.h"
#include "evthread.h"

/*
 * 信号处理实现，使用sigaction或者signal来设置事件处理函数，目前linux上普遍支持sigaction，我是采用这种方式
 * 信号事件的处理采用统一事件源的方式
 * 一次只能一个event_base被设置信号
 */

static int evsig_add(struct event_base *, int , short , short , void *);
static int evsig_del(struct event_base *, int , short , short , void *);

static const struct eventop evsigops = {
        "signal",
        NULL,
        evsig_add,
        evsig_del,
        NULL,
        NULL,
        0,0,0
};

static void *evsig_base_lock = NULL;

static struct event_base *evsig_base = NULL;
static int evsig_base_n_signals_added = 0;
static int evsig_base_fd = -1;

//信号事件处理函数
static void evsig_handler(int sig);

//针对信号的加解锁
#define EVSIGBASE_LOCK() EVLOCK_LOCK(evsig_base_lock,0)
#define EVSIGBASE_UNLOCK() EVLOCK_UNLOCK(evsig_base_lock, 0)

//设置全局参数
void evsig_set_base(struct event_base *base){
    EVSIGBASE_LOCK();
    evsig_base = base;
    evsig_base_n_signals_added = base->sig.ev_n_signals_added;
    evsig_base_fd = base->sig.ev_signal_pair[1];
    EVSIGBASE_UNLOCK();
}

//信号处理函数写入一个字节到信号管道时的回调函数
static void evsig_cb(int fd, short what, void *arg){
    static char signals[1024] = {0};

    ssize_t n;
    int ncaught[NSIG];
    struct event_base *base;

    base = (struct event_base*)arg;

    memset(&ncaught,0, sizeof(ncaught));

    while(1){
        n = read(fd,signals, sizeof(signals));
        if(n == -1){
            //出錯
            int err = errno;
            if(!EVUTIL_ERR_RW_RETRIABLE(err))
                event_sock_err(1,fd,"%s: recv",__func__);
            break;
        }
        else if(n == 0)break;
        for(int i = 0 ;i < n;++i){
            uint8_t sig = signals[i];
            if(sig < NSIG)
                ncaught[sig]++;
        }
    }

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    for (int i = 0;  i < NSIG; ++i) {
        if(ncaught[i])
            evmap_signal_active(base, i, ncaught[i]);
    }
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

int evsig_init(struct event_base *base){
    /* 我们的信号处理程序将写入双向管道一端以唤醒我们的事件循环。 事件循环然后扫描递送的信号 */
    if(evutil_make_internal_pipe(base->sig.ev_signal_pair) == -1){
        event_sock_err(1,-1, "%s: socketpair", __func__);
        return -1;
    }

    if(base->sig.sh_old){
        mm_free(base->sig.sh_old);
    }
    base->sig.sh_old = NULL;
    base->sig.sh_old_max = 0;

    event_assign(&base->sig.ev_signal, base, base->sig.ev_signal_pair[0],
                 EV_READ | EV_PERSIST, evsig_cb, base);

    base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;
    event_priority_set(&base->sig.ev_signal, 0);

    base->evsigsel = &evsigops;

    return 0;
}

//将evsignal的信号处理程序设置为base中的处理程序，以便我们在清除当前处理程序时恢复原始处理程序。
int evsig_set_handler(struct event_base *base, int evsignal, void (*handler)(int))
{
    struct sigaction sa;
    struct evsig_info *sig = &base->sig;
    void *p;

    /* 调整sh_old的内存空间大小 */
    if (evsignal >= sig->sh_old_max) {
        int new_max = evsignal + 1;
        event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing", __func__, evsignal, sig->sh_old_max));
        p = mm_realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));
        if (p == NULL) {
            event_warn("realloc");
            return (-1);
        }

        memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old), 0,
               (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

        sig->sh_old_max = new_max;
        sig->sh_old = (struct sigaction **)p;
    }

    /* 在动态数组中为evsignal的事件处理函数分配空间 */
    sig->sh_old[evsignal] = (struct sigaction *)mm_malloc(sizeof * sig->sh_old[evsignal]);
    if (sig->sh_old[evsignal] == NULL) {
        event_warn("malloc");
        return (-1);
    }

    /* 设置新的事件处理函数 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);

    if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
        event_warn("sigaction");
        mm_free(sig->sh_old[evsignal]);
        sig->sh_old[evsignal] = NULL;
        return (-1);
    }

    return (0);
}

int evsig_restore_handler(struct event_base *base, int evsignal)
{
    int ret = 0;
    struct evsig_info *sig = &base->sig;
    struct sigaction *sh;

    if (evsignal >= sig->sh_old_max) {
        return 0;
    }

    /* 销毁evsignal的事件处理程序 */
    sh = sig->sh_old[evsignal];
    sig->sh_old[evsignal] = NULL;
    if (sigaction(evsignal, sh, NULL) == -1) {
        event_warn("sigaction");
        ret = -1;
    }

    mm_free(sh);

    return ret;
}

static int evsig_add(struct event_base *base,int evsignal,short old,short events,void *p){
    struct evsig_info *sig = &base->sig;
    (void)p;

    EVUTIL_ASSERT(evsignal >= 0 && evsignal < NSIG);

    //捕捉信号，如果它们发生很快
    EVSIGBASE_LOCK();
    if(evsig_base != base && evsig_base_n_signals_added){
        event_warnx("Added a signal to event base %p with signals already added to event_base %p.  "
                    "Only one can have signals at a time with the %s backend.  "
                    "The base with the most recently added signal or the most recent "
                     "event_base_loop() call gets preference; "
                     "do not rely on this behavior in future Libevent versions.",
                    base, evsig_base, base->evsel->name);
    }
    evsig_base = base;
    evsig_base_n_signals_added = ++sig->ev_n_signals_added;
    evsig_base_fd = base->sig.ev_signal_pair[1];
    EVSIGBASE_UNLOCK();

    event_debug(("%s: %d: changing signal handler", __func__, (int)evsignal));
    if (evsig_set_handler(base, (int)evsignal, evsig_handler) == -1) {
        goto err;
    }

    if (!sig->ev_signal_added) {
        if (event_add_nolock(&sig->ev_signal, NULL, 0))
            goto err;
        sig->ev_signal_added = 1;
    }

    return (0);

err:
    EVSIGBASE_LOCK();
    --evsig_base_n_signals_added;
    --sig->ev_n_signals_added;
    EVSIGBASE_UNLOCK();
    return (-1);
}

static int evsig_del(struct event_base *base, int evsignal, short old, short events, void *p)
{
    EVUTIL_ASSERT(evsignal >= 0 && evsignal < NSIG);

    event_debug(("%s: %d: restoring signal handler", __func__, evsignal));

    EVSIGBASE_LOCK();
    --evsig_base_n_signals_added;
    --base->sig.ev_n_signals_added;
    EVSIGBASE_UNLOCK();

    return (evsig_restore_handler(base, (int)evsignal));
}

static void evsig_handler(int sig)
{
    int save_errno = errno;
    uint8_t msg;

    if (evsig_base == NULL) {
        event_warnx("%s: received signal %d, but have no base configured", __func__, sig);
        return;
    }

    /* 唤醒我们的通知机制 */
    msg = sig;

    write(evsig_base_fd, (char*)&msg, 1);

    errno = save_errno;
}

void evsig_dealloc(struct event_base *base)
{
    if (base->sig.ev_signal_added) {
        event_del(&base->sig.ev_signal);
        base->sig.ev_signal_added = 0;
    }

    /* ev_signal_added == 0时，调试事件在evsig_init/event_assign中创建，因此需要重新分配 */
    event_debug_unassign(&base->sig.ev_signal);

    for (int i = 0; i < NSIG; ++i) {
        if (i < base->sig.sh_old_max && base->sig.sh_old[i] != NULL)
            evsig_restore_handler(base, i);
    }
    EVSIGBASE_LOCK();
    if (base == evsig_base) {
        evsig_base = NULL;
        evsig_base_n_signals_added = 0;
        evsig_base_fd = -1;
    }
    EVSIGBASE_UNLOCK();

    if (base->sig.ev_signal_pair[0] != -1) {
        close(base->sig.ev_signal_pair[0]);
        base->sig.ev_signal_pair[0] = -1;
    }
    if (base->sig.ev_signal_pair[1] != -1) {
        close(base->sig.ev_signal_pair[1]);
        base->sig.ev_signal_pair[1] = -1;
    }
    base->sig.sh_old_max = 0;

    /* 每个索引释放在evsig_del() */
    if (base->sig.sh_old) {
        mm_free(base->sig.sh_old);
        base->sig.sh_old = NULL;
    }
}

int evsig_global_setup_locks(const int enable_locks)
{
    EVTHREAD_SETUP_GLOBAL_LOCK(evsig_base_lock, 0);
    return 0;
}

static void evsig_free_globals_locks(void)
{
    if (evsig_base_lock != NULL) {
        EVTHREAD_FREE_LOCK(evsig_base_lock, 0);
        evsig_base_lock = NULL;
    }
    return;
}

void evsig_free_globals(void)
{
    evsig_free_globals_locks();
}
