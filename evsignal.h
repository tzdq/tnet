#ifndef TNET_EVSIGNAL_INTERNAL_H
#define TNET_EVSIGNAL_INTERNAL_H

#include <signal.h>

//信号结构体
struct evsig_info {
    struct event ev_signal;//ev_signal_pair[1]关注的事件
    int ev_signal_pair[2];//用于从事件处理程序发送通知，现在都是统一事件源，作用类似pipe
    int ev_signal_added;//是否已经添加信号事件的标志（true/false）
    int ev_n_signals_added;//当前我们关注的信号事件的数量
    //用于恢复旧的信号处理程序，这儿默认使用sigaction，
    struct sigaction **sh_old;
    int sh_old_max;//sh_old的大小
};

int evsig_init(struct event_base *);
void evsig_dealloc(struct event_base *);

void evsig_set_base(struct event_base *base);

#endif //TNET_EVSIGNAL_INTERNAL_H
