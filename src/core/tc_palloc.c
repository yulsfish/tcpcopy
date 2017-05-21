
#include <xcopy.h>

static void *tc_palloc_block(tc_pool_t *pool, size_t size);
static void *tc_palloc_large(tc_pool_t *pool, size_t size);

/*
 * 创建内存池
 * size:     内存池初始大小
 * sub_size: 内存池扩大步长
 * pool_max: 单次可malloc的内存最大大小
*/
tc_pool_t *
tc_create_pool(int size, int sub_size, int pool_max)
{
    tc_pool_t  *p;

    if (size < (int) TC_MIN_POOL_SIZE) {
        tc_log_info(LOG_ERR, 0, "pool size must be no less than:%d", 
                TC_MIN_POOL_SIZE);
        size = TC_MIN_POOL_SIZE;
    }

    p = tc_memalign(TC_POOL_ALIGNMENT, size);
    if (p != NULL) {
        /*
         * 初始化内存池结构
        */
        p->d.last = (u_char *) p + sizeof(tc_pool_t);
        p->d.end  = (u_char *) p + size;
        p->d.next = NULL;
        p->d.failed = 0;
        p->d.objs   = 0;
        p->d.need_check = 0;
        p->d.cand_recycle = 0;
        p->d.is_traced = 0;
        p->main_size = size;
        if (sub_size > (int) TC_MIN_POOL_SIZE) {
            p->sub_size = sub_size;
        } else {
            p->sub_size = p->main_size;
        }

        /*
         * 池子可用内存实际大小
        */
        size = size - sizeof(tc_pool_t);
        
        /*
         * 确定单次可分配内存大小
         * 大于 max 将属于large内存
        */
        if (pool_max && size >= pool_max) {
            p->sh_num.max = pool_max;
        } else {
            p->sh_num.max = (size < (int) TC_MAX_ALLOC_FROM_POOL) ? 
                size : (int) TC_MAX_ALLOC_FROM_POOL;
        }

        p->current = p;
        p->sh_pt.large = NULL;
    }
    
    return p;
}

/*
 * 销毁内存池
*/
void
tc_destroy_pool(tc_pool_t *pool)
{
#if (TC_DEBUG)
    int                 tot_size, sub_size;
#endif
    tc_pool_t          *p, *n;
    tc_pool_large_t    *l;

    for (l = pool->sh_pt.large; l; l = l->next) {

        if (l->alloc) {
            tc_free(l->alloc);
        }
    }

#if (TC_DEBUG)
    tot_size = pool->main_size - pool->sub_size;
    sub_size = pool->sub_size;
#endif
    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
#if (TC_DEBUG)
        tot_size += sub_size;
#endif
        tc_free(p);

        if (n == NULL) {
            break;
        }
    }

#if (TC_DEBUG)
    tc_log_info(LOG_NOTICE, 0, "pool occupy:%d", tot_size);
#endif

}

/*
 * 申请内存
*/
void *
tc_palloc(tc_pool_t *pool, size_t size)
{
    u_char            *m;
    tc_pool_t         *p;
    tc_mem_hid_info_t *hid;

    /*
     * 实际占用内存大小为 size + 附加信息大小
    */
    size = size + MEM_HID_INFO_SZ;

    /*
     * 小于max，属于小内存，走小内存申请流程
    */
    if ((int) size <= pool->sh_num.max) {

        /*
         * 从current 开始查
        */
        p = pool->current;

        do {
            m = tc_align_ptr(p->d.last, TC_ALIGNMENT);

            if ((size_t) (p->d.end - m) >= size) {            
#if (TC_DETECT_MEMORY)
                if (p->d.last >= p->d.end) {
                    tc_log_info(LOG_WARN, 0, "pool full");
                }
#endif
                /*
                 * 找到合适的块， 填充附加信息
                */
                p->d.objs++;
                p->d.last = m + size;
                hid = (tc_mem_hid_info_t *) m;
                hid->large = 0;
                hid->len = size;
                hid->try_rel_cnt = 0;
                hid->released = 0;

                return m + MEM_HID_INFO_SZ;
            }

            /*
             * 当前池子里没找到合适的块，则到下一个池子继续找
            */
            p = p->d.next;

        } while (p);

        /*
         * 所有的池子里都没找到，则扩大内存池
        */
        m = tc_palloc_block(pool, size);
        if (m != NULL) {
            hid = (tc_mem_hid_info_t *) m;
            hid->large = 0;
            hid->len = size;
            hid->try_rel_cnt = 0;
            hid->released = 0;
            return m + MEM_HID_INFO_SZ;
        } else {
            return NULL;
        }
    }

    /*
     * 大于max，属于大内存，到走大内存申请流程
    */
    m = tc_palloc_large(pool, size);
    if (m != NULL) {
        hid = (tc_mem_hid_info_t *) m;
        hid->large = 1;
        hid->len = size;
        hid->try_rel_cnt = 0;
        hid->released = 0;
        return m + MEM_HID_INFO_SZ;
    } else {
        return NULL;
    }
}


static bool 
tc_check_block_free(tc_pool_t *root, tc_pool_t *p)
{
    int                i;
    u_char            *m;
    tc_mem_hid_info_t *hid;

    if (p->sh_pt.fp) {
        m = (u_char *) p->sh_pt.fp;
        i = p->sh_num.fn;
#if (TC_DETECT_MEMORY)
        if (root->d.is_traced) {
            tc_log_info(LOG_INFO, 0, "pool:%p,block:%p,m from last:%p, i:%d", 
                    root, p, m, i);
        }
#endif
 
    } else {
        m = ((u_char *) p) + sizeof(tc_pool_t);
        m = tc_align_ptr(m, TC_ALIGNMENT);
        i = 0;
    }

    /*
     * 检查是否有没释放的块
    */
    while (m < p->d.end) {
        hid = (tc_mem_hid_info_t *) m;
        if (!hid->released) {
            p->sh_pt.fp = hid;
            p->sh_num.fn = i;
            hid->try_rel_cnt++;
#if (TC_DETECT_MEMORY)
            if (hid->try_rel_cnt == REL_CNT_MAX_VALUE) {
                tc_log_info(LOG_INFO, 0, "pool:%p,block:%p,len:%u occupy", 
                        root, p, hid->len);
            }
#endif
            return false;
        }
        m += hid->len;
        m = tc_align_ptr(m, TC_ALIGNMENT);
        i++;

        /*
         * 检测的个数与已分配个数相等则无需再查
        */
        if (i == p->d.objs) {
            break;
        }
    }

    return true;
}


static void *
tc_palloc_block(tc_pool_t *pool, size_t size)
{
    bool        reused;
    u_char     *m;
    size_t      psize;
    tc_pool_t  *p, *new, *current;

    reused = false;

    /*
     * 看next内存池是否可以回收
    */
    p  = pool->d.next;
    if (p && p->d.cand_recycle) {
        /*
         * 标记为可回收不是说直接就可以用， 还是要验证一下的
        */
        if (tc_check_block_free(pool, p)) {
            reused = true;
            m = (u_char *) p;
            new = p;

            /*
             *回收的池子先从链表上摘下来
            */
            pool->d.next = p->d.next;
#if (TC_DETECT_MEMORY)
            if (pool->d.is_traced) {
                tc_log_info(LOG_INFO, 0, "pool:%p recycle:%p", pool, p);
            }
#endif
        }
    }

    /*
     * 没有可回收的块，则向系统申请一块内存
    */
    if (!reused) {
        if (pool->sub_size) {
            psize = pool->sub_size;
        } else {
            psize = (size_t) (pool->d.end - (u_char *) pool);
        }
        m = tc_memalign(TC_POOL_ALIGNMENT, psize);

        if (m == NULL) {
            return NULL;
        }

#if (TC_DETECT_MEMORY)
        if (pool->d.is_traced) {
            tc_log_info(LOG_INFO, 0, "pare pool:%p, create pool:%p, size:%d", 
                    pool, m, (int) psize);
        }
#endif

        new = (tc_pool_t *) m;
        new->d.end  = m + psize;
    }

    new->d.next = NULL;
    new->d.failed = 0;
    new->d.objs = 1;

    /*
     * 非第一个池子都可检查可回收性
    */
    new->d.need_check = 1;
    new->d.cand_recycle = 0;
    new->d.is_traced = 0;
    new->sh_pt.fp = NULL;
    new->sh_num.fn = 0;

    m += sizeof(tc_pool_t);
    m = tc_align_ptr(m, TC_ALIGNMENT);
    new->d.last = m + size;

#if (TC_DETECT_MEMORY)
    if (new->d.last > new->d.end) {
        tc_log_info(LOG_WARN, 0, "pool overflow");
    }
#endif

    current = pool->current;

    for (p = current; p->d.next; p = p->d.next) {
        /*
         * 能进到这个函数，说明向现有的池子申请内存都失败了， 都要failed++
        */
        if (p->d.failed++ > 4) {
            /*
             * 对于可失败超4次的池子，标记为可回收
            */
            if (p->d.need_check) {
                p->d.cand_recycle = 1;
            }
            /*
             * failed超4次，则移动current, 无需每次都存头查
            */
            current = p->d.next;
        }
    }

    /*
     * 将新的池子挂在表尾
    */
    p->d.next = new;

    pool->current = current ? current : new;

    return m;
}


static void *
tc_palloc_large(tc_pool_t *pool, size_t size)
{
    void              *p;
    tc_uint_t          n;
    tc_pool_large_t   *large;

    /*
     * 直接向系统申请
    */
    p = tc_alloc(size);
    if (p != NULL) {

        n = 0;

        /*
         *有可用的large结点则复用
         *注意，large是挂在第一个池子结构上的
        */
        for (large = pool->sh_pt.large; large; large = large->next) {
            if (large->alloc == NULL) {
                large->alloc = p;
                return p;
            }

            if (n++ > 3) {
                break;
            }
        }

        /*
         * 没有可用的large结点，则向小内存池申请一个large结点
        */
        large = tc_palloc(pool, sizeof(tc_pool_large_t));
        if (large == NULL) {
            tc_free(p);
            return NULL;
        }

        /*
         * 将新申请的large内存结点挂到链表上
        */
        large->alloc = p;
        large->next = pool->sh_pt.large;
        pool->sh_pt.large = large;
    }

    return p;
}


tc_int_t
tc_pfree(tc_pool_t *pool, void *p)
{
    tc_pool_large_t   *l, *prev;
    tc_mem_hid_info_t *act_p;
    
    if (p == NULL)
        return TC_OK;

    act_p = (tc_mem_hid_info_t *) ((u_char *) p - MEM_HID_INFO_SZ);

    
    if (act_p->large) {
        /*
         *大内存，直接还给系统, 结点不释放还是要复用滴
        */
        prev = NULL;
        for (l = pool->sh_pt.large; l; l = l->next) {
            if (act_p == l->alloc) {
                tc_free(l->alloc);
                l->alloc = NULL;

                /*
                 * TODO: bug ?
                */
                if (prev) {
                    prev->next = l->next;
                } else {
                    pool->sh_pt.large = l->next;
                }

                act_p = (tc_mem_hid_info_t *) ((u_char *) l - MEM_HID_INFO_SZ);

                /*
                 * large结点标记为released
                */
                act_p->released = 1;
#if (TC_DETECT_MEMORY)
                if (act_p->len != TC_LARGE_OBJ_INFO_SIZE) {
                    tc_log_info(LOG_WARN, 0, "pool item wrong:%d != %d", 
                            act_p->len, TC_LARGE_OBJ_INFO_SIZE);
                }
#endif

                return TC_OK;
            }
            prev = l;
        }

#if (TC_DETECT_MEMORY)
        if (l == NULL) {
            tc_log_info(LOG_WARN, 0, "pool item not freed");
        }
#endif
    } else {
        /*
         * 小内存，直接标记即可
        */
        act_p->released = 1;
    }

    return TC_DELAYED;
}



void *
tc_pcalloc(tc_pool_t *pool, size_t size)
{
    void *p;

    p = tc_palloc(pool, size);
    if (p) {
        tc_memzero(p, size);
    }

    return p;
}


