#ifndef TC_SELECT_MODULE_INCLUDED
#define TC_SELECT_MODULE_INCLUDED

#include <xcopy.h>

typedef struct tc_select_multiplex_io_s   tc_select_multiplex_io_t;

struct tc_select_multiplex_io_s {
    int             max_fd;           /*此IO对象关注的最大fd*/
    int             last;             /*此IO对象关注的event个数*/
    tc_event_t    **evs;              /*此IO对象关注的event列表*/         
    fd_set          r_set;
    fd_set          w_set;
};


int tc_select_create(tc_event_loop_t *loop);
int tc_select_destroy(tc_event_loop_t *loop);
int tc_select_add_event(tc_event_loop_t *loop, tc_event_t *ev, int events);
int tc_select_del_event(tc_event_loop_t *loop, tc_event_t *ev, int events);
int tc_select_polling(tc_event_loop_t *loop, long timeout);

#endif
