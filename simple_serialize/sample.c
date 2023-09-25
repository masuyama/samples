#include <string.h>

#include "packet.h"

char fixed_array[64] = "1234";

char array[64] = "5678";
int array_len = 6;

int serialize(char *buffer)
{
    char *pbuffer = buffer;

    _PUSH_DATA_1(pbuffer, 1);       // 1byte(int8)
    _PUSH_DATA_2(pbuffer, 20);      // 2byte(int16)
    _PUSH_DATA_4(pbuffer, 400);     // 4byte(int32)
    _PUSH_DATA_8(pbuffer, 8000);    // 8byte(int64)
    _PUSH_ARRAYF(pbuffer, fixed_array, sizeof(fixed_array));    // 固定長（受け取り側でも長さがわかっている）
    _PUSH_ARRAYW(pbuffer, array, array_len);    // 可変長（受け取り側で長さがわからない）
    _PUSH_STRING(pbuffer, "stringstring");  // 文字列

    int total_len = pbuffer - buffer;

    return total_len;
}

int deserialize(char *buffer)
{
    char *pbuffer = buffer;

    char d1;
    _POP_DATA_1(pbuffer, d1);       // 1byte(int8)
    printf("DATA_1 : %d\n", d1);

    uint16_t d2;
    _POP_DATA_2(pbuffer, d2);      // 2byte(int16)
    printf("DATA_2 : %d\n", d2);

    uint32_t d4;
    _POP_DATA_4(pbuffer, d4);     // 4byte(int32)
    printf("DATA_4 : %u\n", d4);

    uint64_t d8;
    _POP_DATA_8(pbuffer, d8);    // 8byte(int64)
    printf("DATA_8 : %lu\n", d8);

    // 固定長（受け取り側でも長さがわかっている）
    char *fixed_array_ptr;
    _POP_ARRAYF(pbuffer, fixed_array_ptr, sizeof(fixed_array));
    printf("ARRAYF : %d\n", memcmp(fixed_array_ptr, fixed_array, sizeof(fixed_array)));
    // 可変長（受け取り側で長さがわからない）
    char *recv_array;
    int recv_array_len;
    _POP_ARRAYW(pbuffer, recv_array, recv_array_len);
    printf("ARRAYW : %d(%d)\n", memcmp(recv_array, array, recv_array_len), recv_array_len);

    char *bufstr;
    _POP_STRING(pbuffer, bufstr);  // 文字列
    printf("STRING : %s\n", bufstr);
}

int main(int argc, char *argv[])
{
    // Serialize
    char buffer[65536];

    int len = serialize(buffer);
    printf("%d bytes packed\n", len);

    // Deserialize
    deserialize(buffer);

    exit(0);
}
