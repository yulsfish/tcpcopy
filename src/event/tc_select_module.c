#include <xcopy.h>

int tc_select_create(tc_event_loop_t *loop)
{
    tc_event_t               **evs;
    tc_select_multiplex_io_t  *io;

    evs = tc_palloc(loop->pool, loop->size * sizeof(tc_event_t *));
    if (evs == NULL) {
        return TC_EVENT_ERROR;
    }

    /*
     * 创建 IO 对象
    */
    io = tc_palloc(loop->pool, sizeof(tc_select_multiplex_io_t));
    if (io == NULL) {
        return TC_EVENT_ERROR;
    }

    FD_ZERO(&io->r_set);
    FD_ZERO(&io->w_set);

    io->max_fd = -1;
    io->last = 0;
    io->evs = evs;

    loop->io = io;

    return TC_EVENT_OK;
}

int tc_select_destroy(tc_event_loop_t *loop)
{
    int                       i;
    tc_event_t               *event;
    tc_select_multiplex_io_t *io;

    io = loop->io;

    for (i = 0; i < io->last; i++) {
        event = io->evs[i];
        if (event->fd > 0) {
            tc_log_info(LOG_NOTICE, 0, "tc_select_destroy, close fd:%d",
                    event->fd);
            tc_socket_close(event->fd);
        }
        event->fd = -1;
        tc_pfree(loop->pool, event);
    }

    tc_pfree(loop->pool, io->evs);
    tc_pfree(loop->pool, loop->io);

    return TC_EVENT_OK;
}

int tc_select_add_event(tc_event_loop_t *loop, tc_event_t *ev, int events)
{
    tc_select_multiplex_io_t *io;

    io = loop->io;

    /*
     * io event个数超上限
    */
    if (io->last >= loop->size) {
        /* too many */
        return TC_EVENT_ERROR;
    }

    /*
     * fd_set
    */
    if (events == TC_EVENT_READ && ev->read_handler
            && ev->write_handler == NULL)
    {
        FD_SET(ev->fd, &io->r_set);
    } else if (events == TC_EVENT_WRITE && ev->write_handler
            && ev->read_handler == NULL)
    {
        FD_SET(ev->fd, &io->w_set);
    } else {
        return TC_EVENT_ERROR;
    }

    /*
     * 更新最大的fd
    */
    if (io->max_fd != -1 && ev->fd > io->max_fd) {
        io->max_fd = ev->fd;
    }

    ev->index = io->last;
    /*
     * 复制event到event 数组，
     * 递增event数目
    */
    io->evs[io->last++] = ev;

    return TC_EVENT_OK;
}

int tc_select_del_event(tc_event_loop_t *loop, tc_event_t *ev, int events)
{
    tc_event_t               *last_ev;
    tc_select_multiplex_io_t *io;

    io = loop->io;

    if (ev->index < 0 || ev->index >= io->last) {
        return TC_EVENT_ERROR;
    }

    if (events == TC_EVENT_READ) {
        FD_CLR(ev->fd, &io->r_set);
    } else if (events == TC_EVENT_WRITE) {
        FD_CLR(ev->fd, &io->w_set);
    } else {
        return TC_EVENT_ERROR;
    }

    /*
     * 这样就能删除 ?
    */
    if (ev->index < --(io->last)) {
        last_ev = io->evs[io->last];
        io->evs[ev->index] = last_ev;
        last_ev->index = ev->index;
    }

    ev->index = -1;

    /*
     * 如果删除的就是最大fd对应的event
     * 则删除后max_fd是不知道的
    */
    if (io->max_fd == ev->fd) {
        io->max_fd = -1;
    }

    return TC_EVENT_OK;
}

int tc_select_polling(tc_event_loop_t *loop, long to)
{
    int                         i, ret;
    fd_set                      cur_read_set, cur_write_set;
    tc_event_t                **evs;
    struct timeval              timeout;
    tc_select_multiplex_io_t   *io;

    io = loop->io;
    evs = io->evs;

    if (io->max_fd == -1) {
        for (i = 0; i < io->last; i++) {
            if (io->max_fd < evs[i]->fd) {
                io->max_fd = evs[i]->fd;
            }
        }
    }

    timeout.tv_sec = (long) (to / 1000);
    timeout.tv_usec = (long) ((to % 1000) * 1000);

    cur_read_set = io->r_set;
    cur_write_set = io->w_set;

    ret = select(io->max_fd + 1, &cur_read_set, &cur_write_set, NULL,
                 &timeout);

    if (ret == -1) {
        if (errno == EINTR) {
           return TC_EVENT_AGAIN;
        }
        return TC_EVENT_ERROR;
    }

    if (ret == 0) {
        return TC_EVENT_AGAIN;
    }

    for (i = 0; i < io->last; i++) {
        /*
         * 这里有个清除操作
        */
        /* clear the active events, and reset */
        evs[i]->events = TC_EVENT_NONE;

        if (evs[i]->read_handler) {
            if (FD_ISSET(evs[i]->fd, &cur_read_set)) {
                evs[i]->events |= TC_EVENT_READ;
                /*
                 * 事件加入激活事件链表
                */
                tc_event_push_active_event(loop->active_events, evs[i]);
            }
        } else {
            if (FD_ISSET(evs[i]->fd, &cur_write_set)) {
                evs[i]->events |= TC_EVENT_WRITE;
                tc_event_push_active_event(loop->active_events, evs[i]);
            }
        }
    }

    return TC_EVENT_OK;
}

