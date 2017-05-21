#ifndef _TC_PALLOC_H_INCLUDED_
#define _TC_PALLOC_H_INCLUDED_


#include <xcopy.h>

typedef struct tc_pool_large_s  tc_pool_large_t;
typedef struct tc_pool_loop_s  tc_pool_loop_t;

struct tc_pool_large_s {
    tc_pool_large_t     *next;
    void                *alloc;
};

/*
 * 内存附加信息
*/
typedef struct {
    uint32_t len:24;                     /*分配的块长度*/
    uint32_t try_rel_cnt:6;              /*被回尝试收次数*/
    uint32_t large:1;                    /*是否是大内存*/
    uint32_t released:1;                 /*是否已经释放*/
    uint32_t padding;
} tc_mem_hid_info_t;

/*内存池数据结构*/
typedef struct {
    u_char              *last;            /*上次内存分配结束的位置，即下次开始的地方*/
    u_char              *end;             /*整个内存池结束的位置*/
    tc_pool_t           *next;            /*下一个内存池*/
    uint32_t             objs:16;         /*内存池分出去的块数*/
    uint32_t             failed:8;        /*内存分配失败的次数*/
    uint32_t             need_check:1;    /*是不是需要检查内存池可回收，第一个内存池不用检查 !!*/
    uint32_t             cand_recycle:1;  /*failed若干次之后，标记为可尝试回收*/
    uint32_t             is_traced:1;     /*暂时没用*/
} tc_pool_data_t;

/*
 * 内存池结构
 * 小内存挂在 tc_pool_data_t 下
 * 大内存挂在 tc_pool_large_t 下
*/
struct tc_pool_s {
    tc_pool_data_t         d;             /*内存池数据块*/
    union {
        int max;                          /*内存池对外提供的最大块大小（第一个内存池使用）*/
        int fn;                           /*第一块未释放内存块的序号*/
    } sh_num;
    int                    main_size;     /*内存池初始大小*/
    int                    sub_size;      /*内存池增大步长*/
    tc_pool_t             *current;       /*当前使用的内存池*/
    union {
        tc_mem_hid_info_t *fp;            /*指向第一块未释放内存的附加信息(第二个及以后的内存池使用)*/
        tc_pool_large_t   *large;         /*大内存链表(第一个内存池使用)*/
    } sh_pt;
};


tc_pool_t *tc_create_pool(int size, int sub_size, int pool_max);
void tc_destroy_pool(tc_pool_t *pool);

void *tc_palloc(tc_pool_t *pool, size_t size);
void *tc_pcalloc(tc_pool_t *pool, size_t size);
tc_int_t tc_pfree(tc_pool_t *pool, void *p);



#endif /* _TC_PALLOC_H_INCLUDED_ */
