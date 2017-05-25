
#include <xcopy.h>

tc_atomic_t  tc_over = 0;

static tc_event_t *ev_mark[MAX_FD_NUM];

static tc_event_actions_t tc_event_actions = {
#ifdef TC_HAVE_EPOLL
    tc_epoll_create,
    tc_epoll_destroy,
    tc_epoll_add_event,
    tc_epoll_del_event,
    tc_epoll_polling
#else
    tc_select_create,
    tc_select_destroy,
    tc_select_add_event,
    tc_select_del_event,
    tc_select_polling
#endif
};


int tc_event_loop_init(tc_event_loop_t *loop, int size)
{
    tc_pool_t          *pool;
    tc_event_actions_t *actions;

    /*
     * 创建event_loop 的内存池
    */
    pool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0, 0);

    if (pool != NULL) {
        /*
         * event_loop 操作集
        */
        actions = &tc_event_actions;

        loop->actions = actions;
        loop->active_events = NULL;
        loop->pool = pool;
        loop->size = size;

        /*
         * 创建action对象
        */
        if (actions->create(loop) == TC_EVENT_ERROR) {
            return TC_EVENT_ERROR;
        }

        return TC_EVENT_OK;
    } else {
        return TC_EVENT_ERROR;
    }
}


int tc_event_loop_finish(tc_event_loop_t *loop)
{
    tc_event_actions_t *actions;

    actions = loop->actions;

    if (actions != NULL) {
        /* destroy io module */
        actions->destroy(loop);

        loop->actions = NULL;
    }

    if (loop->pool) {
        tc_destroy_pool(loop->pool);
        loop->pool = NULL;
    }

    return TC_EVENT_OK;
}

int tc_event_add(tc_event_loop_t *loop, tc_event_t *ev, int events)
{
    tc_event_actions_t *actions;

    /*
     * 记录event 所属的event_loop
    */
    ev->loop = loop;
    actions = loop->actions;

    if (events == TC_EVENT_NONE) {
        return TC_EVENT_OK;
    }

    /*
     * 添加event到loop
    */
    if (actions->add(loop, ev, events) == TC_EVENT_ERROR) {
        return TC_EVENT_ERROR;
    }

    if (events & TC_EVENT_READ) {
        ev->reg_evs |= TC_EVENT_READ;
    }

    if (events & TC_EVENT_WRITE) {
        ev->reg_evs |= TC_EVENT_WRITE;
    }

    return TC_EVENT_OK;
}

int tc_event_del(tc_event_loop_t *loop, tc_event_t *ev, int events)
{
    tc_event_actions_t *actions;

    actions = loop->actions;

    if (events == TC_EVENT_NONE) {
        return TC_EVENT_OK;
    }

    if (actions->del(loop, ev, events) == TC_EVENT_ERROR) {
        return TC_EVENT_ERROR;
    }

    if (events & TC_EVENT_READ) {
        ev->reg_evs &= ~TC_EVENT_READ;
    }

    if (events & TC_EVENT_WRITE) {
        ev->reg_evs &= ~TC_EVENT_WRITE;
    }

    return TC_EVENT_OK;
}


int tc_event_proc_cycle(tc_event_loop_t *loop)
{
    int                  ret;
    long                 timeout;
    tc_msec_t            delta;
    tc_event_t          *act_event, *act_next;
    tc_event_actions_t  *actions;

    actions = loop->actions;

    for ( ;; ) {
        /*
         * 得到距离当前时间最小的超时时间
         * 把这个时间作为poll的超时时间
         * 就不会影响超时事件的处理
        */
        timeout = tc_event_find_timer();
        if (timeout == 0 || timeout > 1000) {
            timeout = 500;
        }

        /*
         * 清空active事件链表
        */
        loop->active_events = NULL;

        /*
         * 记录当前时间
        */
        delta = tc_current_time_msec;

        /*
         * poll
         * timeout时间内等待网络事件
        */
        ret = actions->poll(loop, timeout);
        if (tc_over) {
            goto FINISH;
        }

        /*
         * 更新当前时间
        */
        tc_time_update();

        /*
         * 计算时间差值，
        */
        delta = tc_current_time_msec - delta;

        /*
         * 如果poll立即返回, delta 就有可能在1ms内, 本次loop就不用再查找超时事件
         * 属于优化
        */
        if (delta) {
            /*
             * 处理超时事件
            */
            tc_event_expire_timers();
        }

        /*
         * poll返回error或again
         * 则没有网络io事件需要处理, 直接进入下一轮loop
        */
        if (ret == TC_EVENT_ERROR || ret == TC_EVENT_AGAIN) {
            continue;
        }

        /*
         * 处理所有激活的网络IO事件
        */
        for (act_event = loop->active_events; act_event; act_event = act_next) {
            act_next = act_event->next;

            if (act_event->events & TC_EVENT_READ) {
                if (act_event->read_handler &&
                        act_event->read_handler(act_event) == TC_ERR_EXIT)
                {
                    goto FINISH;
                }
            }

            if (act_event->events & TC_EVENT_WRITE) {
                if (act_event->write_handler &&
                        act_event->write_handler(act_event) == TC_ERR_EXIT)
                {
                    goto FINISH;
                }
            }

            if (act_event->reg_evs == TC_EVENT_NONE) {

                tc_event_destroy(act_event, 0);
            }
        }
    }

FINISH:
    return TC_EVENT_OK;
}


tc_event_t *tc_event_create(tc_pool_t *pool, int fd, tc_event_handler_pt reader,
        tc_event_handler_pt writer)
{
    tc_event_t *ev;

    ev = tc_palloc(pool, sizeof(tc_event_t));

    if (ev != NULL) {
        ev->events = 0;
        ev->reg_evs = 0;
        ev->index = -1;
        ev->fd = fd;
        ev->next = NULL;
        ev->read_handler = reader;
        ev->write_handler = writer;
    }

    return ev;
}

static void tc_event_destroy_with_no_delay(tc_pool_t *pool, tc_event_t *ev)
{
    tc_log_info(LOG_NOTICE, 0, "destroy event:%d", ev->fd);
    ev_mark[ev->fd] = NULL;
    ev->loop = NULL;
    ev->read_handler = NULL;
    ev->write_handler = NULL;
    tc_pfree(pool, ev);
}

void tc_event_destroy(tc_event_t *ev, int delayed)
{
    tc_log_info(LOG_NOTICE, 0, "enter tc_event_destroy:%d", ev->fd);
    if (ev->fd <= 0 || ev->fd >= MAX_FD_NUM) {
        tc_log_info(LOG_ERR, 0, "fd is not valid");
        return;
    }

    if (ev_mark[ev->fd] != NULL && ev != ev_mark[ev->fd]) {
        tc_log_info(LOG_NOTICE, 0, "destroy prev ev:%d", ev->fd);
        tc_event_destroy_with_no_delay(ev->loop->pool, ev_mark[ev->fd]);
    }

    /*
     * 延时销毁的事件，本次只做标记，下次销毁时才真正销毁
     * 否则，立即销毁
    */
    if (delayed) {
        tc_log_info(LOG_NOTICE, 0, "delayed destroy ev:%d", ev->fd);
        ev_mark[ev->fd] = ev;
    } else {
        tc_event_destroy_with_no_delay(ev->loop->pool, ev);
    }
}

