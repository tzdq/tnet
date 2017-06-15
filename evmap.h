/*
 * event_io_map其实就是event_signal_map，是用的宏定义，但是event_signal_map中关于IO事件和signal信号量使用的数据结构不同
*/

#ifndef TNET_EVMAP_INTERNAL_H
#define TNET_EVMAP_INTERNAL_H

struct event_base;
struct event;

//evmap_io和evmap_signal初始化
void evmap_io_initmap(struct event_io_map* ctx);
void evmap_signal_initmap(struct event_signal_map* ctx);

//清空event_*_map的所有数据
void evmap_io_clear(struct event_io_map* ctx);
void evmap_signal_clear(struct event_signal_map* ctx);

// 添加一个IO事件（EV_READ或者EV_WRITE的组合）到event_base.io,如果它的状态发送改变，通知eventops上对应的fd，要求ev未被添加
int evmap_io_add(struct event_base *base, int fd, struct event *ev);

//从event_base.io中删除一个IO事件（EV_READ或者EV_WRITE的组合）,如果它的状态发送改变，通知eventops上对应的fd
int evmap_io_del(struct event_base *base, int fd, struct event *ev);

/* 激活event_base上给定fd的一组事件（EV_READ|EV_WRITE|EV_ET.）*/
void evmap_io_active(struct event_base *base, int fd, short events);

/* 返回与给定fd关联的fdinfo对象。 如果fd没有与之相关联的事件，则结果可能为NULL.*/
void *evmap_io_get_fdinfo(struct event_io_map *ctx, int fd);

/* 这些函数的作用与evmap_io_ *的方式相同，除了它们处理信号而不是fds。*/
int evmap_signal_add(struct event_base *base, int signum, struct event *ev);
int evmap_signal_del(struct event_base *base, int signum, struct event *ev);
void evmap_signal_active(struct event_base *base, int signum, int ncalls);

#endif //TNET_EVMAP_INTERNAL_H
