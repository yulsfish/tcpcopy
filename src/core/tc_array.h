
#ifndef TC_ARRAY_H_INCLUDED
#define TC_ARRAY_H_INCLUDED

#include <xcopy.h>


struct tc_array_s{
    void        *elts;             /*数组*/
    unsigned int nelts;            /*已经分配出去的数组个数*/
    size_t       size;             /*数组元素大小*/
    unsigned int nalloc;           /*数组容量*/
    tc_pool_t   *pool;
};


tc_array_t *tc_array_create(tc_pool_t *p, unsigned int n, size_t size);
void tc_array_destroy(tc_array_t *a);
void *tc_array_push(tc_array_t *a);
void *tc_array_push_n(tc_array_t *a, unsigned int n);


static inline int
tc_array_init(tc_array_t *array, tc_pool_t *pool, unsigned int n, size_t size)
{
    array->nelts = 0;
    array->size = size;
    array->nalloc = n;
    array->pool = pool;

    array->elts = tc_palloc(pool, n * size);
    if (array->elts == NULL) {
        return TC_ERR;
    }

    return TC_OK;
}


#endif /* TC_ARRAY_H_INCLUDED */
