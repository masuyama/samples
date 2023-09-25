#if !defined(__MESSAGE_H_INCLUDED__)
#define __MESSAGE_H_INCLUDED__

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <inttypes.h>

    /*
     * intのkeyを用いた単純なhashlist + fifo message list
     *  *** データの実体も持ちます
     */

#include "poolalloc.h"

    typedef struct _element
    {
        struct _element *next;

        struct _element *hashnext;

        uintptr_t key;
        char data[];
    } element_t;

    typedef struct
    {
        int basenum;
        int datasize;

        element_t **element;
        element_t *top;
        element_t *last;

        void *element_a;
    } messageheader_t;

    static inline void message_delete_one(void *m);

    /**
     * message管理構造体の開放.
     *
     * @param void *m
     */
    static inline void message_release(void *m)
    {
        messageheader_t *msg = (messageheader_t *)m;

        if (msg->element != NULL)
        {
            while (msg->top != NULL)
            {
                message_delete_one(m);
            }
            free(msg->element);
            msg->element = NULL;
        }

        if (msg->element_a != NULL)
        {
            release_pool(msg->element_a);
        }

        memset(msg, 0, sizeof(messageheader_t));
        free(msg);
    }

    /**
     * message管理構造体の初期化.
     *
     * @param int basenum
     * @param  int datasize
     * @param  int initial_num
     * @return static
     */
    static inline void *message_create(int basenum, int datasize, int initial_num)
    {
        int i;

        messageheader_t *msg = (messageheader_t *)malloc(sizeof(messageheader_t));
        if (msg == NULL)
        {
            return NULL;
        }
        msg->basenum = basenum;
        msg->datasize = datasize;
        msg->element = (element_t **)malloc(sizeof(element_t *) * msg->basenum);
        if (msg->element == NULL)
        {
            free(msg);
            return NULL;
        }
        for (i = 0; i < msg->basenum; i++)
        {
            msg->element[i] = NULL;
        }
        msg->top = NULL;
        msg->last = NULL;

        msg->element_a = init_pool(sizeof(element_t) + msg->datasize, initial_num);
        if (msg->element_a == NULL)
        {
            message_release(msg);
            return NULL;
        }

        return (void *)msg;
    }

    /**
     * keyによる検索.
     * （element_tを返す）
     *
     * @param messageheader_t *msg
     * @param  uintptr_t key
     * @return void *
     */
    static inline element_t *__message_find(messageheader_t *msg, uintptr_t key)
    {
        element_t *elem = NULL;
        for (elem = msg->element[key % msg->basenum]; elem; elem = elem->hashnext)
        {
            if (key == elem->key)
            {
                return elem;
            }
        }
        return NULL;
    }

    /**
     * keyによる検索.
     * （valueを返す）
     *
     * @param void *m
     * @param  uintptr_t key
     * @return void *
     */
    static inline void *message_find(void *m, uintptr_t key)
    {
        messageheader_t *msg = (messageheader_t *)m;
        element_t *elem = __message_find(msg, key);

        if (elem != NULL)
        {
            return elem->data;
        }

        return NULL;
    }

    /**
     * hashからの削除.
     *
     * @param messageheader_t *msg
     * @param  int key
     */
    static inline void __message_del_hash(messageheader_t *msg, uintptr_t key)
    {
        element_t *prev = NULL;
        element_t *elem = NULL;
        element_t *delelem = NULL;
        uintptr_t idx = key % msg->basenum;

        for (elem = msg->element[idx]; elem; elem = elem->hashnext)
        {
            if (delelem != NULL)
            {
                pool_free(msg->element_a, delelem);
                delelem = NULL;
            }
            if (key == elem->key)
            {
                if (prev == NULL)
                {
                    msg->element[idx] = elem->hashnext;
                }
                else
                {
                    prev->hashnext = elem->hashnext;
                }
                delelem = elem;
                continue; // keyは重複がありうるので続行
            }
            prev = elem;
        }

        if (delelem != NULL)
        {
            pool_free(msg->element_a, delelem);
            delelem = NULL;
        }
    }

    /**
     * messageからの削除.
     * (同一keyのものをすべて削除します)
     *
     * @param void *m
     * @param  uintptr_t key
     */
    static inline void message_del(void *m, uintptr_t key)
    {
        messageheader_t *msg = (messageheader_t *)m;
        element_t *prev = NULL;
        element_t *elem = NULL;

        for (elem = msg->top; elem; elem = elem->next)
        {
            if (key == elem->key)
            {
                if (prev == NULL)
                {
                    msg->top = elem->next;
                }
                else
                {
                    prev->next = elem->next;
                }
                continue; // keyは重複がありうるので続行
            }
            prev = elem;
        }
        msg->last = prev;

        __message_del_hash(msg, key); // elementのfreeはこの中で行う
        return;
    }

    /**
     *  messageへの要素の追加.
     *
     * @param void *m
     * @param  uintptr_t key
     * @return void *
     */
    static inline void *message_add(void *m, uintptr_t key)
    {
        messageheader_t *msg = (messageheader_t *)m;
        element_t *elem;

        // hashkeyとの一番の違いは重複キーを許すこと

        elem = (element_t *)pool_alloc(msg->element_a);
        if (elem == NULL)
        {
            return NULL;
        }

        // hashlistの先頭に追加
        int idx = key % msg->basenum;
        elem->key = key;
        elem->next = NULL;
        elem->hashnext = msg->element[idx];
        msg->element[idx] = elem;

        // msglistの最後に追加
        if (msg->last != NULL)
        {
            msg->last->next = elem;
        }
        else
        {
            msg->top = elem;
        }
        msg->last = elem;

        return elem->data;
    }

    /**
     * 先頭のmessageを一つ取得する.
     *
     * @param void *m
     * @return void *
     */
    static inline void *message_get_one(void *m)
    {
        messageheader_t *msg = (messageheader_t *)m;

        if (msg->top)
        {
            return msg->top->data;
        }
        return NULL;
    }

    /**
     * 先頭のmessageを一つ削除する.
     *
     * @param void *m
     */
    static inline void message_delete_one(void *m)
    {
        messageheader_t *msg = (messageheader_t *)m;
        element_t *e = msg->top;

        if (msg->last == e)
        {
            msg->last = NULL;
        }
        msg->top = e->next;

        int idx = e->key % msg->basenum;
        element_t *prev = NULL;
        element_t *elem = NULL;
        for (elem = msg->element[idx]; elem; elem = elem->hashnext)
        {
            if (e == elem)
            {
                if (prev == NULL)
                {
                    msg->element[idx] = elem->hashnext;
                }
                else
                {
                    prev->hashnext = elem->hashnext;
                }
                break;
            }
            prev = elem;
        }

        pool_free(msg->element_a, e);

        return;
    }

    /**
     * message内容のdump.
     * （debug用）
     *
     * @param void *m
     */
    static inline void message_dump(void *m)
    {
        messageheader_t *msg = (messageheader_t *)m;
        element_t *elem = NULL;
        int i;

        for (i = 0; i < msg->basenum; i++)
        {
            printf("ELEMENT[%d] : ", i);
            for (elem = msg->element[i]; elem; elem = elem->hashnext)
            {
                printf("->[%" PRIuPTR "] %p %p", elem->key, elem, elem->data);
            }
            printf("\n");
        }

        for (elem = msg->top, i = 0; elem; elem = elem->next, i++)
        {
            printf("LIST[%d] : [%" PRIuPTR "] %p %p\n", i, elem->key, elem, elem->data);
        }
        printf("LAST : [%" PRIuPTR "] %p %p\n", msg->last->key, msg->last, msg->last->data);
    }

#ifdef __cplusplus
}
#endif

#endif /* !defined (__MESSAGE_H_INCLUDED__) */
