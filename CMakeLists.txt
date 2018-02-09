cmake_minimum_required(VERSION 3.7)
project(tnet)

set(CMAKE_CXX_STANDARD 11)
include_directories(event2)
include_directories(./sys)

set(SOURCE_FILES sys/queue.h event2/event.h event2/event_struct.h event2/util.h event2/thread.h evlog.c event.c event_internal.h evlog.h evutil.c evthread.c evutil.h evmemory.h evthread.h evthread_pthread.c minheap.h evsignal.c evsignal.h evmap.h evmap.c event2/listener.h listener.c defer.h event2/buffer.h bufferevent_internal.h bufferevent.c buffer.c event2/bufferevent.h evbuffer.h bufferevent_sock.c evmemory.c eviomultiplexing.h eviomultiplexing.c)

add_executable(tnet ${SOURCE_FILES})