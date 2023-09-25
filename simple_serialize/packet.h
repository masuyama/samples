#if !defined (__PACKET_H_INCLUDED__)
#define __PACKET_H_INCLUDED__

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t htonll(uint64_t ull)
{
    uint32_t upper, lower;
    uint64_t n_upper, n_lower, n_ull;

    upper = ull >> 32;
    lower = ull & 0xffffffffULL;
    n_upper = htonl(upper);
    n_lower = htonl(lower);
    n_ull = (n_upper << 32ULL) | n_lower;

    return n_ull;
}

static inline uint64_t ntohll(uint64_t n_ull)
{
    uint32_t n_upper, n_lower;
    uint64_t upper, lower, ull;

    n_upper = n_ull >> 32;
    n_lower = n_ull & 0xffffffffULL;
    upper = ntohl(n_upper);
    lower = ntohl(n_lower);
    ull = (upper << 32ULL) | lower;

    return ull;
}

#define _COPY_DATA_1(_data, i)          {i = *_data; _data++;}
#define _COPY_DATA_2(_data, i)          {uint16_t __ts; memcpy(&__ts, _data, sizeof(__ts)); i = ntohs(__ts); _data+=sizeof(__ts);}
#define _COPY_DATA_4(_data, i)          {uint32_t __tl; memcpy(&__tl, _data, sizeof(__tl)); i = ntohl(__tl); _data+=sizeof(__tl);}
#define _COPY_DATA_8(_data, i)          {uint64_t __tll; memcpy(&__tll, _data, sizeof(__tll)); i = ntohll(__tll); _data+=sizeof(__tll);}
#define _COPY_ARRAYF(_data, a, len)     {memcpy(a, _data, len);_data+=len;}
#define _COPY_ARRAYW(_data, a, len)     {_POP_DATA_4(_data, len); _COPY_ARRAYF(_data, a, len);}
#define _COPY_STRING(_data, buf)        {char *__strt=buf; while (*_data!='\0'){*__strt=*_data; _data++; __strt++;} *__strt='\0';_data++;}

#define _POP_DATA_1(_data, i)           {_COPY_DATA_1(_data, i)}
#define _POP_DATA_2(_data, i)           {_COPY_DATA_2(_data, i)}
#define _POP_DATA_4(_data, i)           {_COPY_DATA_4(_data, i)}
#define _POP_DATA_8(_data, i)           {_COPY_DATA_8(_data, i)}
#define _POP_ARRAYF(_data, a, len)      { a=_data; _data+=len;}
#define _POP_ARRAYW(_data, a, len)      {_POP_DATA_4(_data, len); _POP_ARRAYF(_data, a, len);}
#define _POP_STRING(_data, buf)         {buf=_data;uint32_t alen=0;_LENGTH_STRING(_data, alen);_data+=alen;}

#define _PUSH_DATA_1(_data, i)          *_data = i; _data++;
#define _PUSH_DATA_2(_data, i)          {uint16_t __ts; __ts = htons(i); memcpy(_data, &__ts, sizeof(__ts)); _data+=sizeof(__ts);}
#define _PUSH_DATA_4(_data, i)          {uint32_t __tl; __tl = htonl(i); memcpy(_data, &__tl, sizeof(__tl)); _data+=sizeof(__tl);}
#define _PUSH_DATA_8(_data, i)          {uint64_t __tll; __tll = htonll(i); memcpy(_data, &__tll, sizeof(__tll)); _data+=sizeof(__tll);}
#define _PUSH_ARRAYF(_data, a, len)     {memcpy(_data, a, len); _data+=len;}
#define _PUSH_ARRAYW(_data, a, len)     {_PUSH_DATA_4(_data, len); _PUSH_ARRAYF(_data, a, len);}
#define _PUSH_STRING(_data, str)        {int __n=(int)strlen(str); memcpy(_data, str, __n); _data+=__n; *_data='\0'; _data++;}

#define _LENGTH_DATA_1(i, alen)         alen++;
#define _LENGTH_DATA_2(i, alen)         alen+=sizeof(uint16_t);
#define _LENGTH_DATA_4(i, alen)         alen+=sizeof(uint32_t);
#define _LENGTH_DATA_8(i, alen)         alen+=sizeof(uint64_t);
#define _LENGTH_ARRAYF(a, len, alen)    alen+=len;
#define _LENGTH_ARRAYW(a, len, alen)    {alen+=sizeof(uint32_t); alen+=len; }
#define _LENGTH_STRING(a, alen)         {char *_tmp=a; while (*_tmp!='\0'){_tmp++;alen++;} alen++;/*null文字分を足す*/}

#ifdef __cplusplus
}
#endif

#endif  /* if !defined (__PACKET_H_INCLUDED__) */
