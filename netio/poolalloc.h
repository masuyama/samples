#if !defined(__POOLALLOC_H_INCLUDED__)
#define __POOLALLOC_H_INCLUDED__

#ifdef __cplusplus
extern "C"
{
#endif

    // メモリプール
    // ・解放したものを再利用します
    // ・足りなくなったら自動で拡張します(reallocを使います)
    // ・pool内は固定サイズのelementのリストになっています。異なるサイズの要素は、別のpoolを確保してください。
    // ・可変長データは扱えません

    typedef struct _memelement_t
    {
        int in_use;                 // 使用済みflag
        struct _memelement_t *next; // 次要素(未使用list用)
        char data[0];               // ユーザデータ
    } memelement_t;

    typedef struct _memblock_t
    {
        int num;               // block内の個数
        memelement_t *element; // elementのリスト
    } memblock_t;

    typedef struct
    {
        int num;               // elementの個数
        int use_num;           // elementの使用数
        int max_num;           // 確保可能なelementの最大数
        int size;              // dataのサイズ
        int n_block;           // blockの個数
        memblock_t *block;     // block list
        memelement_t *not_use; // 未使用element list
    } mempool_t;

    static inline void extend_pool(mempool_t *mpool);
    static inline void pool_dump(void *mp);

#if !defined MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define INCREMENT_ELEMENT(cur, size) cur = (memelement_t *)((char *)(cur) + (sizeof(memelement_t) + size))

    static int __poolalloc_verbose = 0;
#define VERBOSE(...)              \
    if (__poolalloc_verbose != 0) \
        fprintf(stdout, __VA_ARGS__);

    /**
     * 解放.
     *
     * @param void *mp
     */
    static inline void release_pool(void *mp)
    {
        mempool_t *mpool = (mempool_t *)mp;

        if (mpool->block != NULL)
        {
            int i;
            memblock_t *pblock;
            for (i = 0, pblock = mpool->block; i < mpool->n_block; i++, pblock++)
            {
                // block内のelementの解放
                if (pblock->element != NULL)
                {
                    free(pblock->element);
                    pblock->element = NULL;
                }
            }
            VERBOSE("release_pool : free block : %p\n", mpool->block);
            // blockの解放
            free(mpool->block);
            mpool->block = NULL;
        }
        VERBOSE("release_pool : %p\n", mpool);
        free(mpool);
    }

    /**
     * blockの初期化.
     * （共通処理。外部から呼ばれることは考えていません）
     *
     * @param mempool_t *mpool
     * @param memblock_t *block
     * @param int num
     * @return memblock_t *
     */
    static inline memblock_t *alloc_block(mempool_t *mpool, memblock_t *block, int num)
    {
        block->num = num;
        block->element = (memelement_t *)malloc((sizeof(memelement_t) + mpool->size) * num);
        if (block->element == NULL)
        {
            VERBOSE("alloc_block : malloc failed : %p : block=%p, num=%d, size=%d\n", mpool, block, num, (int)((sizeof(memelement_t) + mpool->size) * num));
            return NULL;
        }

        VERBOSE("alloc_block : %p : block=%p, element=%p, num=%d, size=%d\n", mpool, block, block->element, num, (int)((sizeof(memelement_t) + mpool->size) * num));
        int i;
        memelement_t *pelement, *prev;
        prev = NULL;
        for (i = 0, pelement = block->element; i < block->num; i++, INCREMENT_ELEMENT(pelement, mpool->size))
        {
            pelement->in_use = 0;
            if (prev != NULL)
            {
                prev->next = pelement;
            }
            prev = pelement;
        }
        prev->next = NULL; // 最後の要素なのでNULLを入れておく

        mpool->not_use = block->element; // 先頭

        return block;
    }

    /**
     * 初期化.
     * （最大値制限付き）
     *
     * @param int size
     * @param int num
     * @param int max_num
     * @return void *
     */
    static inline void *init_pool_with_max(int size, int num, int max_num)
    {
        if ((size <= 0) || (num <= 0))
        {
            // Invalid argument
            return NULL;
        }

        mempool_t *mpool = (mempool_t *)malloc(sizeof(mempool_t));
        if (mpool == NULL)
        {
            VERBOSE("init_pool_with_max : malloc failed\n");
            return NULL;
        }
        VERBOSE("init_pool_with_max : %p\n", mpool);

        mpool->num = num;
        mpool->use_num = 0;
        mpool->max_num = max_num;
        mpool->size = size;
        mpool->n_block = 1; // 最初は一つのみ
        mpool->block = (memblock_t *)malloc(sizeof(memblock_t) * 1);
        VERBOSE("init_pool_with_max : %p : block=%p, size=%d, num=%d, max_num=%d\n", mpool, mpool->block, size, num, max_num);
        if (mpool->block == NULL)
        {
            free(mpool);
            return NULL;
        }

        memblock_t *b = alloc_block(mpool, mpool->block, mpool->num);
        if (b == NULL)
        {
            free(mpool->block);
            free(mpool);
            return NULL;
        }

        return (void *)mpool;
    }

    /**
     * 初期化.
     *
     * @param int size
     * @param int num
     * @return void *
     */
    static inline void *init_pool(int size, int num)
    {
        return init_pool_with_max(size, num, 0);
    }

    /**
     * メモリ領域の拡張（足りなくなったら倍にする）.
     *
     * @param mempool_t *mpool
     */
    static inline void extend_pool(mempool_t *mpool)
    {
        if (mpool->not_use != NULL)
        {
            // まだ空きがある
            VERBOSE("extend_pool : %p : mpool->not_use != NULL\n", mpool);
            return;
        }
        if (mpool->max_num == mpool->num)
        {
            // もうこれ以上は拡張しない
            VERBOSE("extend_pool : %p : mpool->max_num == mpool->num : %d\n", mpool, mpool->num);
            return;
        }

        mpool->n_block++;
        mpool->block = (memblock_t *)realloc(mpool->block, sizeof(memblock_t) * mpool->n_block);
        if (mpool->block == NULL)
        {
            // 失敗
            VERBOSE("extend_pool : %p : block realloc failed : %d\n", mpool, (int)sizeof(memblock_t) * mpool->n_block);
            mpool->n_block--;
            return;
        }
        VERBOSE("extend_pool : %p : block=%p, bsize=%d, bnum=%d\n", mpool, mpool->block, (int)(sizeof(memblock_t) * mpool->n_block), mpool->n_block);

        int new_num = (mpool->max_num == 0) ? mpool->num : MIN(mpool->num, (mpool->max_num - mpool->num));

        memblock_t *b = alloc_block(mpool, &(mpool->block[mpool->n_block - 1]), new_num);
        if (b == NULL)
        {
            VERBOSE("extend_pool : %p : alloc_block failed : %d\n", mpool, new_num);
            mpool->n_block--;
            return;
        }

        // printf("extend : %d -> %d\n", mpool->num, mpool->num + new_num);
        VERBOSE("extend_pool : %p : num=%d, new_num=%d\n", mpool, mpool->num, mpool->num + new_num);
        // 総個数を増やす
        mpool->num += new_num;

        return;
    }

    /**
     * 要素の取得.
     *
     * 足りない場合は拡張します
     *
     * @param void *mp
     * @return void *
     */
    static inline void *pool_alloc(void *mp)
    {
        mempool_t *mpool = (mempool_t *)mp;

        if (mpool->not_use == NULL)
        {
            // メモリの拡張
            extend_pool(mpool);
            if (mpool->not_use == NULL)
            {
                // 拡張に失敗した
                VERBOSE("pool_alloc : %p : extend failed\n", mpool);
                return NULL;
            }
        }

        memelement_t *e = mpool->not_use;

        mpool->not_use = e->next;
        // 情報をクリア
        e->in_use = 1;
        e->next = NULL;

        mpool->use_num++;
        VERBOSE("pool_alloc : %p : data=%p, use_num=%d\n", mpool, e->data, mpool->use_num);
        return (void *)(e->data);
    }

    /**
     * 要素の開放.
     *
     * @param void *mp
     * @param void *element
     */
    static inline void pool_free(void *mp, void *element)
    {
        mempool_t *mpool = (mempool_t *)mp;
        memelement_t *pelement = (memelement_t *)((char *)element - sizeof(memelement_t));

        if (pelement->in_use == 0)
        {
            // 解放済み要素
            VERBOSE("pool_free : %p : not used data=%p\n", mpool, element);
            return;
        }
        pelement->in_use = 0;
        // memset(pelement->data, 0, mpool->size);

        // 未使用リストの先頭に入れる
        pelement->next = mpool->not_use;
        mpool->not_use = pelement;

        mpool->use_num--;

        VERBOSE("pool_free : %p : data=%p, use_num=%d\n", mpool, element, mpool->use_num);

        return;
    }

    /**
     * 使用している要素のうち最初の要素を得る.
     *
     * @param void *mp
     * @return void *
     */
    static inline void *get_element_first(void *mp)
    {
        mempool_t *mpool = (mempool_t *)mp;

        int i, j;
        memblock_t *b = mpool->block;
        for (i = 0; i < mpool->n_block; i++, b++)
        {
            memelement_t *pelement = b->element;
            for (j = 0; j < b->num; j++, INCREMENT_ELEMENT(pelement, mpool->size))
            {
                if (pelement->in_use == 1)
                {
                    VERBOSE("get_element_first : %p : data=%p\n", mpool, pelement->data);
                    return (void *)(pelement->data);
                }
            }
        }
        VERBOSE("get_element_first : %p : data not found\n", mpool);
        return NULL;
    }

    /**
     * 使用している要素の次の要素を得る.
     *
     * @param void *mp
     * @param void *element
     * @return void *
     */
    static inline void *get_element_next(void *mp, void *element)
    {
        mempool_t *mpool = (mempool_t *)mp;
        memelement_t *pelement = (memelement_t *)((char *)element - sizeof(memelement_t));

        int hit = 0;
        memblock_t *b = mpool->block;
        int i, j;
        for (i = 0; i < mpool->n_block; i++, b++)
        {
            // そのblockの中に含まれている要素かどうか調べる
            int esize = b->num * (sizeof(memelement_t) + mpool->size);

            if (((unsigned long int)pelement >= (unsigned long int)(b->element)) &&
                ((unsigned long int)pelement < ((unsigned long int)(b->element) + esize)))
            { // このblockの要素
                memelement_t *e = pelement;
                INCREMENT_ELEMENT(e, mpool->size); // 次の要素から検索スタート
                int n = ((unsigned long int)e - (unsigned long int)(b->element)) / (sizeof(memelement_t) + mpool->size);
                for (j = n; j < b->num; j++, INCREMENT_ELEMENT(e, mpool->size))
                {
                    if (e->in_use)
                    {
                        VERBOSE("get_element_next : %p : current block :  data=%p\n", mpool, e->data);
                        return (e->data);
                    }
                }
                // このblockにはない
                hit = 1;
                continue; // 次のblockへ
            }
            // hitしているので要素を調べる
            if (hit == 1)
            {
                memelement_t *e = b->element;
                for (j = 0; j < b->num; j++, INCREMENT_ELEMENT(e, mpool->size))
                {
                    if (e->in_use)
                    {
                        VERBOSE("get_element_next : %p : next block : data=%p\n", mpool, e->data);
                        return (e->data);
                    }
                }
            }
        }
        VERBOSE("get_element_next : %p : data not found\n", mpool);
        return NULL;
    }

    static inline void foreach_element(void *mp, void *val, int (*func)(void *ele, void *val))
    {
        mempool_t *mpool = (mempool_t *)mp;
        memblock_t *pblock = mpool->block;
        int i, j;
        for (i = 0; i < mpool->n_block; i++, pblock++)
        {
            memelement_t *pelement = pblock->element;
            for (j = 0; j < pblock->num; j++, INCREMENT_ELEMENT(pelement, mpool->size))
            {
                if (pelement->in_use)
                {
                    VERBOSE("foreach_element : %p : data=%p\n", mpool, pelement->data);
                    if (func((void *)(pelement->data), val) < 0)
                    {
                        return;
                    }
                }
            }
        }

        return;
    }

    /**
     * 確保している数を返す.
     *
     * @param void *mp
     * @return int
     */
    static inline int get_element_max_num(void *mp)
    {
        return ((mempool_t *)mp)->num;
    }

    /**
     * 使用している数を返す.
     *
     * loopで調べているので遅いです
     *
     * @param void *mp
     * @return int
     */
    static inline int get_element_use_num(void *mp)
    {
        return ((mempool_t *)mp)->use_num;
    }

    /**
     * 要素が使用中かどうか調べる
     *
     * @param void *mp
     * @param void *element
     * @return int
     */
    static inline int pool_element_is_valid(void *mp, void *element)
    {
        memelement_t *pelement = (memelement_t *)((char *)element - sizeof(memelement_t));
        if (pelement->in_use == 0)
        {
            // 解放済み要素
            return 0;
        }
        // 使用中要素
        return 1;
    }

    /**
     * 情報のdump.
     *
     * @param void *mp
     */
    static inline void pool_dump(void *mp)
    {
        mempool_t *mpool = (mempool_t *)mp;
        memelement_t *pelement;

        printf("POOL DUMP : %d %d %d %d\n", mpool->num, mpool->use_num, mpool->size, mpool->n_block);
        memblock_t *pblock = mpool->block;
        int i, j;
        for (i = 0; i < mpool->n_block; i++, pblock++)
        {
            pelement = pblock->element;
            printf("POOL BLOCK [%03d] : %p : %d\n", i, pblock, pblock->num);
            for (j = 0; j < pblock->num; j++, INCREMENT_ELEMENT(pelement, mpool->size))
            {
                printf("POOL ELEMENT [%03d][%03d] : %p : %d %ld %p\n", i, j, pelement, pelement->in_use, sizeof(memelement_t), pelement->data);
            }
        }
        for (pelement = mpool->not_use; pelement; pelement = pelement->next)
        {
            printf("POOL NOT USE : %p : %d %p %p\n", pelement, pelement->in_use, pelement->data, pelement->next);
        }
    }

    static inline void set_pool_alloc_verbose(int v)
    {
        __poolalloc_verbose = v;
    }

#ifdef __cplusplus
}
#endif

#endif /* !defined (__POOLALLOC_H_INCLUDED__) */
