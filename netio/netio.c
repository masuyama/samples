/**
 * netio.c
 *
 * ネットワークIO
 *
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <fcntl.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>

#include <event.h>

#include <errno.h>
#include <assert.h>

#include "netio.h"

#include "poolalloc.h"
#include "message.h"

// #include "addrsearch.h"

/***************************/
/* define */

#if !defined MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#if !defined MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static int NIO_DEBUG = 0; // debug flag
#define _PRINTF(...)         \
	if (NIO_DEBUG)           \
	{                        \
		printf(__VA_ARGS__); \
		fflush(stdout);      \
	} // debug print

#if defined(_DEBUG)
#define _DEFAULT_CONNECTION_NUM 2
#define _PUSH_BUFFER_NUM_PAR_LOOP 1
#else
#define _DEFAULT_CONNECTION_NUM 200
#define _PUSH_BUFFER_NUM_PAR_LOOP 128
#endif
#define _MAX_CONNECTION_NUM 65536 * 4 // これ以上は絶対にコネクションしないという数

#define BUFFER_SIZE NIO_BUFFER_SIZE		  // max recv buffer size
#define RW_BUFFER_SIZE BUFFER_SIZE		  // read/write buffer size (per connection)
#define SOCKET_RECV_BUFFER_SIZE 65536 * 4 // TCP recv buffer size
#define SOCKET_SEND_BUFFER_SIZE 65536 * 4 // TCP send buffer size
#define MESSAGE_HASH_SIZE 103			  // message hashsize

// convert macro
#define NETIO_TO_CONNECTION(conn, co, retval) \
	if (co == NIO_INVALID_HANDLE)             \
		return retval;                        \
	conn = (connection_t *)co;
#define NETIO_TO_TCP(tcp, in, retval) \
	if (in == NIO_INVALID_HANDLE)     \
		return retval;                \
	tcp = (tcp_t *)in;

#define CONN_CLEAR(conn)                             \
	close(conn->soc);                                \
	event_del(&(conn->event));                       \
	tcp_t *__parent = (tcp_t *)conn->parent;         \
	netio_tcp_delete_write_buffer(__parent, conn);   \
	__purge_recv_buffer(__parent, &(conn->rbuffer)); \
	conn->rbuffer.len = 0;                           \
	pool_free(__parent->connection_a, conn);

#define FREE(p)    \
	if (p != NULL) \
	{              \
		free(p);   \
		p = NULL;  \
	}

/***************************
 * receive buffer */
typedef struct _recv_buffer
{
	char data[BUFFER_SIZE];
	int len;

	struct _recv_buffer *next;
} recv_buffer_t;

/***************************
 * connection */
typedef struct _connection
{
	int soc;			// socket
	struct event event; // event

	close_callback close_func; // close callback function
	recv_callback recv_func;   // receive callback function
	parse_callback parse_func; // parse callback function

	recv_check_func rcheck_func; // receive check function

	void *parent; // server or client

	recv_buffer_t rbuffer; // receive buffer

	struct _connection *pair; // pair connection

	char conbuf[]; // connection buffer
} connection_t;

/***************************
 * server */
typedef struct _server
{
	connection_t listen_conn;	   // listen connection
	accept_callback accept_func;   // accept callback function
	accept_check_func acheck_func; // accept check function
} server_t;

/***************************
 * client */
typedef struct _client
{
	char address[NIO_MAX_ADDRESS_LEN]; // connect address
	unsigned short port;			   // connect port

	close_callback close_func; // close callbck function
	recv_callback recv_func;   // receive callback function
	parse_callback parse_func; // parse callback function

	recv_check_func rcheck_func; // receive check function
} client_t;

/***************************
 * tcp_t */
typedef struct _tcp
{
	void *connection_a; // server connection
	int n_connection;	// server connection num

	struct sockaddr_in addr;	   // server address info
	struct event_base *event_base; // event base

	struct event event; // timer event

	void *wbuffer_m; // message list
	void *rbuffer_a; // optional receive buffer

	union
	{
		server_t server;
		client_t client;
	};

	char svbuf[]; // server buffer

} tcp_t;

/***************************
 * write_buffer_t */
typedef struct _write_buffer
{
	nio_conn conn;
	char buffer[RW_BUFFER_SIZE];
	int buffer_len;
} write_buffer_t;

static struct event_base *event_base = NULL; // global event base

static struct timeval timer_inteval = {.tv_sec = 0, .tv_usec = 1000};

// static int netio_tcp_append_write_buffer(tcp_t *tcp, connection_t *c, const char *data, int len);
static void netio_tcp_delete_write_buffer(tcp_t *tcp, connection_t *c);
static int netio_tcp_push_write_buffer(nio_tcp tcp, int count);

/**
 * 受信バッファの使用サイズを得る
 *
 * @param connection_t *conn : コネクション
 * @return int : データ長さ（なければ0)
 */
static int __get_receive_buffer_len(connection_t *conn)
{
	int len = 0;
	recv_buffer_t *rb;
	for (rb = &(conn->rbuffer); rb; rb = rb->next)
	{
		len += rb->len;
	}
	return len;
}

/**
 * 未使用拡張受信バッファの削除
 *
 * @param tcp_t *tcp : tcp
 * @param  recv_buffer_t *rb : 受信バッファ（これ以降のバッファを消す）
 */
static void __purge_recv_buffer(tcp_t *tcp, recv_buffer_t *rb)
{
	if (rb == NULL)
	{
		return;
	}
	// 残りを消す
	recv_buffer_t *delete_rb = rb->next;
	while (delete_rb)
	{
		recv_buffer_t *trb = delete_rb->next;
		_PRINTF("%s : purge : (%p) %d\n", __func__, delete_rb, delete_rb->len);
		delete_rb->len = 0;
		delete_rb->next = NULL;
		pool_free(tcp->rbuffer_a, delete_rb);
		delete_rb = trb;
	}
	rb->next = NULL;
}

/**
 * 受信バッファへの積み込み
 *
 * @param connection_t *conn : コネクション
 * @param  const char *data : 積み込むデータ
 * @param  int len : 積み込むデータ長さ
 *
 * @return int : 成功：1 / 失敗：0
 */
static int __stack_recv_buffer(connection_t *conn, const char *data, int len)
{
	tcp_t *tcp = conn->parent;
	const char *pdata = data;
	int last_len = len;

	if (last_len == 0)
	{
		// 入れるものはない
		conn->rbuffer.len = 0;
		__purge_recv_buffer(tcp, &(conn->rbuffer));
		return 1;
	}

	recv_buffer_t *rb = &(conn->rbuffer);
	int cplen = MIN(sizeof(rb->data), last_len);
	// printf("%s : cplen = %d\n", __func__, cplen);
	//  data copy
	memcpy(rb->data, pdata, cplen);
	rb->len = cplen;
	// pointerをするめる
	pdata += cplen;
	last_len -= cplen;
	// printf("%s : last_len = %d\n", __func__, last_len);
	_PRINTF("%s : stuck buffer : %d %d\n", __func__, cplen, last_len);

	while (last_len > 0)
	{
		// printf("%s : last_len(loop) = %d\n", __func__, last_len);
		if (rb->next == NULL)
		{
			rb->next = (recv_buffer_t *)pool_alloc(tcp->rbuffer_a);
			if (rb == NULL)
			{
				_PRINTF("%s : rbuffer_a pool_alloc failed\n", __func__);
				return 0;
			}
			rb->next->len = 0;
			rb->next->next = NULL;
			rb = rb->next;
			_PRINTF("%s : alloc : (%p) %d\n", __func__, rb, last_len);
		}

		int cplen = MIN(sizeof(rb->data), last_len);
		// data copy
		memcpy(rb->data, pdata, cplen);
		rb->len = cplen;
		// pointerをするめる
		pdata += cplen;
		last_len -= cplen;
		// printf("%s : last_len(loop end) = %d\n", __func__, last_len);
	}
	__purge_recv_buffer(tcp, rb); // 現在よりも後のデータを消す

	return 1;
}

/******************************************************************************/
/**
 * パーサ設定時の受信バッファ処理.
 *
 * @param connection_t *conn [in] : コネクション
 * @param  char *data [in] :受信データ
 * @param  int len [in] :受信データ長さ
 * @return int : 成功:受信データ長さ 失敗:< 0
 */
static inline int __parse_receive(connection_t *conn, char *data, int len)
{
	char *pdata = NULL;
	char *databuffer = NULL;
	int n_data = 0;

	if (conn->parse_func == NULL)
	{
		return -1; // safety
	}

	int erb_len = __get_receive_buffer_len(conn);
	int databuffer_len = len + erb_len + 1; // +1はtextの時の'\0'の分を確保
	if (erb_len > 0)
	{
		// 保存してあるデータを読み込む

		char *databuffer = (char *)malloc(databuffer_len);
		if (databuffer == NULL)
		{
			_PRINTF("%s : databuffer alloc failed : %d\n", __func__, databuffer_len);
			assert(0);
			return -1; // safety
		}

		// printf("%s : erb_len = %d\n", __func__, erb_len);
		pdata = databuffer;
		n_data = 0;

		// rbufferのデータを積む
		int tmplen = 0;
		recv_buffer_t *rb;
		for (rb = &(conn->rbuffer); rb; rb = rb->next)
		{
			// printf("%s : rb->len = %d\n", __func__, rb->len);
			memcpy(pdata + tmplen, rb->data, rb->len);
			tmplen += rb->len;
		}
		// 今回のデータを改めて積む
		memcpy(pdata + tmplen, data, len);

		n_data = len + erb_len; // 合計長
		pdata = databuffer;

		__purge_recv_buffer(conn->parent, &(conn->rbuffer));
		conn->rbuffer.len = 0;
	}
	else
	{
		pdata = data;
		n_data = len;
	}
	// printf("%s : len = %d\n", __func__, n_data);

	char *parsed_data = (char *)malloc(databuffer_len);
	if (parsed_data == NULL)
	{
		FREE(databuffer);
		_PRINTF("%s : parsed_data alloc failed : %d\n", __func__, databuffer_len);
		assert(0);
		return -1;
	}
	while (n_data > 0)
	{
		int parsed_len = databuffer_len;
		int read_len = conn->parse_func(pdata, n_data, parsed_data, &parsed_len);
		if (read_len < 0)
		{
			// 何らかのエラー
			_PRINTF("%s : parse_func failed : %d : %d %d\n", __func__, parsed_len, n_data, (int)databuffer_len);
			FREE(databuffer);
			FREE(parsed_data);
			return read_len;
		}
		else if (read_len == 0)
		{
			// データが足りない
			break;
		}

		if (conn->recv_func != NULL)
		{
			conn->recv_func(conn, parsed_data, parsed_len);
		}
		n_data -= read_len;
		pdata += read_len;
	}
	// printf("%s : n_data = %d\n", __func__, n_data);

	if (n_data > 0)
	{
		// 残りを受信バッファに戻す
		if (!__stack_recv_buffer(conn, pdata, n_data))
		{
			_PRINTF("%s : recv buffer too short : %d\n", __func__, n_data);
			assert(0);
			FREE(databuffer);
			FREE(parsed_data);
			return -2;
		}
	}

	FREE(databuffer);
	FREE(parsed_data);
	return n_data;
}

/**
 * タイマーイベント処理.
 *
 * @param int soc [in] : イベント発生descriptor（未使用）
 * @param short events [in] : 発生イベント種類（未使用）
 * @param void *user_data [in] : ユーザ設定データ：tcp_t構造体へのポインタ
 */
static void __timer_event_callback(int soc, short events, void *user_data)
{
	tcp_t *t = (tcp_t *)user_data;

	int r = 0;
	if (t->wbuffer_m != NULL)
	{
		r = netio_tcp_push_write_buffer(t, _PUSH_BUFFER_NUM_PAR_LOOP);
	}

	static struct timeval ti = {.tv_sec = 0, .tv_usec = 500000};
	if (r > 0)
	{
		ti = timer_inteval;
	}
	evtimer_add(&(t->event), &ti);
}

/**
 * 読み込みイベント処理
 *
 * @param int soc [in] : イベント発生ソケット
 * @param short events [in] : 発生イベント種類
 * @param void *user_data [in] : ユーザ設定データ：connection_t構造体へのポインタ
 */
static void __read_event_callback(int soc, short events, void *user_data)
{
	int ret = -1;
	char buff[BUFFER_SIZE];

	connection_t *conn = (connection_t *)user_data;
	tcp_t *sv = NULL;

	if (conn->rcheck_func != NULL)
	{
		// 受信可否チェック関数が指定されている
		if (conn->rcheck_func(conn) < 0)
		{
			// 受信可能ではないので何もしない
			return; // recvを呼び出していないので、kernelの受信バッファにたまる
		}
	}

	if (!(events & EV_READ))
	{
		// READ eventではない
		_PRINTF("%s : event = 0x%X\n", __func__, events);
		return;
	}

	sv = (tcp_t *)conn->parent;

	ret = recv(soc, buff, sizeof(buff), MSG_NOSIGNAL);
	if (ret == 0)
	{
		// 切断
		if (conn->close_func != NULL)
		{
			// close callback が指定されていたらcallbackを呼び出す
			conn->close_func(conn, 0);
		}
		CONN_CLEAR(conn);
		_PRINTF("connlist : %d / %d\n", get_element_use_num(sv->connection_a), get_element_max_num(sv->connection_a));
		return;
	}
	else if (ret < 0)
	{
		// エラー
		if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR))
		{
			// 後でもう一度呼ぶ
			return;
		}
		// 上記以外のエラー
		_PRINTF("%s : read failed : %d\n", __func__, errno);
		if (conn->close_func != NULL)
		{
			// close callback が指定されていたらcallbackを呼び出す
			conn->close_func(conn, errno);
		}
		CONN_CLEAR(conn);
		_PRINTF("connlist : %d / %d\n", get_element_use_num(sv->connection_a), get_element_max_num(sv->connection_a));
		return;
	}
	else
	{
		// Pairへの送信
		if (conn->pair != NULL)
		{
			int r = netio_sender(conn->pair, buff, ret);
			_PRINTF("relay[%d](%d) : %d\n", soc, ret, r);
			return;
		}
		// 通常処理
		if (conn->parse_func != NULL)
		{
			// parce functionが指定されている
			if (__parse_receive(conn, buff, ret) < 0)
			{
				_PRINTF("%s : parse_func failed : %d %d\n", __func__, soc, ret);
			}
		}
		else if (conn->recv_func != NULL)
		{
			// recv callbackが指定されていたらcallbackを呼び出す
			conn->recv_func(conn, buff, ret);
		}
		else
		{
			_PRINTF("read[%d](%d)=%s\n", soc, ret, buff);
		}
	}
}

/**
 * acceptイベント処理
 *
 * @param int soc [in] : イベント発生ソケット
 * @param short events [in] : 発生イベント種類
 * @param void *user_data [in] : ユーザ設定データ：tcp_t構造体へのポインタ
 */
static void __accept_event_callback(int event_soc, short events, void *user_data)
{
	socklen_t addr_len;
	struct sockaddr_in caddr;
	int soc = 1;
	connection_t *conn = NULL;
	tcp_t *sv = (tcp_t *)user_data;

	if (sv->server.acheck_func != NULL)
	{
		// accept可否チェック関数が指定されている
		if (sv->server.acheck_func(sv) < 0)
		{
			// acceptできる状態ではない
			return;
		}
	}

	_PRINTF("accept event callback\n");

	if (!(events & EV_READ))
	{
		// READ eventではない
		_PRINTF("%s : event = 0x%X\n", __func__, events);
		return;
	}

	addr_len = sizeof(struct sockaddr_in);
	if ((soc = accept(event_soc, (struct sockaddr *)&caddr, &addr_len)) == -1)
	{
		_PRINTF("%s : accept failed : %s(%d) \n", __func__, strerror(errno), errno);
		return;
	}

	conn = (connection_t *)pool_alloc(sv->connection_a);
	if (conn == NULL)
	{
		_PRINTF("%s : no more connection!!\n", __func__);
		close(soc);
		return;
	}
	_PRINTF("connlist : %d / %d\n", get_element_use_num(sv->connection_a), get_element_max_num(sv->connection_a));
	conn->soc = soc;

	fcntl(conn->soc, F_SETFL, O_NONBLOCK | O_RDWR); // non blocking
	int val = SOCKET_RECV_BUFFER_SIZE;
	setsockopt(conn->soc, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)); // recv buffer size
	val = SOCKET_SEND_BUFFER_SIZE;
	setsockopt(conn->soc, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)); // send buffer size

	event_set(&(conn->event), conn->soc, EV_READ | EV_PERSIST, __read_event_callback, conn);
	event_base_set(sv->event_base, &(conn->event));
	event_add(&(conn->event), NULL);
	conn->recv_func = sv->server.listen_conn.recv_func;
	conn->close_func = sv->server.listen_conn.close_func;
	conn->parse_func = sv->server.listen_conn.parse_func;
	conn->rcheck_func = NULL;
	conn->parent = (void *)sv;
	conn->rbuffer.len = 0;
	conn->rbuffer.next = NULL;
	conn->pair = NULL;

	if (sv->server.accept_func != NULL)
	{
		// accept callbaskが指定されていたらcallbackを呼び出す
		if (sv->server.accept_func(conn) < 0)
		{
			// 負の値を返してきたらコネクションを受け付けない
			CONN_CLEAR(conn);
			_PRINTF("accept_func failed : connlist : %d / %d\n", get_element_use_num(sv->connection_a), get_element_max_num(sv->connection_a));
		}
	}
}

/*******************************************************/
/**
 * netio 初期化
 */
void netio_init(void)
{
	event_base = event_init(); // global event
}

/********************************************************/
/**
 * netio polling
 *
 * @return int
 */
int netio_poll(void)
{
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 10000; // default timeout

	event_loopexit(&tv);
	event_loop(0);

	return 1;
}

/********************************************************/
/**
 * netio polling
 */
static inline void __nio_tcp_poll(tcp_t *tcp, int timeout)
{
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = timeout;

	event_base_loopexit(tcp->event_base, &tv);
	event_base_loop(tcp->event_base, 0);
}

/********************************************************/
/**
 * tcp polling
 *
 * @param nio_tcp ntcp
 * @param  int timeout
 */
void netio_tcp_poll(nio_tcp ntcp, int timeout)
{
	tcp_t *tcp = NULL;
	NETIO_TO_TCP(tcp, ntcp, );

	__nio_tcp_poll(tcp, timeout);
	return;
}

/*******************************************************
 * netio server polling
 */
void netio_server_poll(nio_server nsv, int timeout)
{
	netio_tcp_poll(nsv, timeout);
	return;
}

/*******************************************************
 * netio client polling
 */
void netio_client_poll(nio_client ncl, int timeout)
{
	netio_tcp_poll(ncl, timeout);
	return;
}

/*******************************************************/
/**
 * nio_tcp 初期化(server/client共通部分)
 *
 */
static inline tcp_t *netio_init_tcp(unsigned int tcpbuffsize, unsigned int conbuffsize)
{
	// サーバ構造体準備
	tcp_t *tcp = (tcp_t *)calloc(1, sizeof(tcp_t) + tcpbuffsize);
	if (tcp == NULL)
	{
		_PRINTF("%s : no more alloc\n", __func__);
		return NIO_INVALID_HANDLE;
	}
	// connection list準備
	tcp->connection_a = init_pool_with_max(sizeof(connection_t) + conbuffsize, _DEFAULT_CONNECTION_NUM, _MAX_CONNECTION_NUM);
	if (tcp->connection_a == NULL)
	{
		_PRINTF("%s : init_pool_with_max (connection_a) failed : %u %u %d\n", __func__, (int)sizeof(connection_t), conbuffsize, _DEFAULT_CONNECTION_NUM);
		return NIO_INVALID_HANDLE;
	}
	// message buffer list
	tcp->wbuffer_m = message_create(MESSAGE_HASH_SIZE, sizeof(write_buffer_t), MAX(_DEFAULT_CONNECTION_NUM / 8, 16));
	if (tcp->wbuffer_m == NULL)
	{
		_PRINTF("%s : message_create failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}
	// recv buffer
	tcp->rbuffer_a = init_pool_with_max(sizeof(recv_buffer_t), 4, _MAX_CONNECTION_NUM);
	if (tcp->rbuffer_a == NULL)
	{
		_PRINTF("%s : init_pool_with_max (rbuffer_a) failed : %u\n", __func__, (int)sizeof(recv_buffer_t));
		return NIO_INVALID_HANDLE;
	}
	return tcp;
}

/*******************************************************/
/**
 * nio_server 初期化
 *
 * @param unsigned short listen_port [in] : listen port番号
 * @param unsigned int tcpbuffsize [in] : netio_tcp_get_buffer で取得できるバッファサイズ
 * @param unsigned int conbuffsize [in] : netio_connection_get_buffer で取得できるコネクションバッファサイズ
 * @param void *group  [in] : 同一グループ 既に確保されたnio_server, nio_client, netio_init_groupで初期化されたグループを渡す
 * @return nio_server
 */
nio_server netio_init_server(unsigned short listen_port, unsigned int tcpbuffsize, unsigned int conbuffsize, void *group)
{
	tcp_t *sv = netio_init_tcp(tcpbuffsize, conbuffsize);
	if (sv == NIO_INVALID_HANDLE)
	{
		return sv;
	}

	connection_t *c = &(sv->server.listen_conn);
	sv->addr.sin_port = htons(listen_port);
	sv->addr.sin_family = AF_INET;
	sv->addr.sin_addr.s_addr = htonl(INADDR_ANY);

	// socket作成
	if (-1 == (c->soc = socket(AF_INET, SOCK_STREAM, 0)))
	{
		_PRINTF("%s : socket failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	int val = 1;
	//	  setsockopt(c->soc, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));	  // no delay
	setsockopt(c->soc, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); // reuse address
																	 //	  setsockopt(c->soc, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));	  // keepalive
																	 //	  fcntl(c->soc, F_SETFL, O_NONBLOCK | O_RDWR);						  // non block
	fcntl(c->soc, F_SETFD, FD_CLOEXEC);								 // close on exec
																	 //	  val = SOCKET_RECV_BUFFER_SIZE;
																	 //	  setsockopt(c->soc, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)); 	  // recv buffer size
																	 //	  val = SOCKET_SEND_BUFFER_SIZE;
																	 //	  setsockopt(c->soc, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)); 	  // send buffer size

	// portへのbind
	if (bind(c->soc, (struct sockaddr *)&(sv->addr), sizeof(sv->addr)) == -1)
	{
		_PRINTF("%s : bind failed\n", __func__);
		netio_release_server(sv);
		return NIO_INVALID_HANDLE;
	}

	// listen開始
	if (listen(c->soc, SOMAXCONN) == -1)
	{
		_PRINTF("%s : listen failed\n", __func__);
		netio_release_server(sv);
		return NIO_INVALID_HANDLE;
	}

	// acceptイベントの設定
	sv->event_base = group ? ((tcp_t *)group)->event_base : event_base_new();
	memset(&(c->event), 0, sizeof(struct event));
	event_set(&(c->event), c->soc, EV_READ | EV_PERSIST, __accept_event_callback, sv);
	event_base_set(sv->event_base, &(c->event));
	event_add(&(c->event), NULL);

	// timer イベントの設定
	memset(&(sv->event), 0, sizeof(struct event));
	evtimer_set(&(sv->event), __timer_event_callback, sv);
	event_base_set(sv->event_base, &(sv->event));
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 10000;
	evtimer_add(&(sv->event), &tv);

	sv->server.accept_func = NULL;
	sv->server.acheck_func = NULL;
	c->close_func = NULL;
	c->recv_func = NULL;
	c->parse_func = NULL;
	c->rbuffer.len = 0;

	return (nio_server)sv;
}

/**
 * nio_serverの開放
 *
 * @param nio_server s
 */
void netio_release_server(nio_server s)
{
	tcp_t *sv = (tcp_t *)s;

	if (sv->connection_a != NULL)
	{
		// 有効なコネクションをclose
		connection_t *c;
		for (c = get_element_first(sv->connection_a); c; c = get_element_next(sv->connection_a, c))
		{
			if (c->close_func)
			{
				// close callbackを呼ぶ
				c->close_func(c, -1);
			}
			CONN_CLEAR(c);
		}
		release_pool(sv->connection_a);
	}

	// listen port close
	event_del(&(sv->server.listen_conn.event));
	close(sv->server.listen_conn.soc);

	// イベントの終了
	event_del(&(sv->event));

	// メモリの開放
	if (sv->wbuffer_m != NULL)
	{
		message_release(sv->wbuffer_m);
		sv->wbuffer_m = NULL;
	}
	// recv bufferの解放
	if (sv->rbuffer_a != NULL)
	{
		release_pool(sv->rbuffer_a);
		sv->rbuffer_a = NULL;
	}

	free(sv);
}

/*******************************************************/
/**
 * client 初期化
 *
 * @param const char *address [in] : 接続先IPアドレス
 * @param unsigned short port [in] : 接続先port番号
 * @param unsigned int tcpbuffsize [in] : netio_tcp_get_buffer で取得できるバッファサイズ
 * @param unsigned int conbuffsize [in] : netio_connection_get_buffer で取得できるコネクションバッファサイズ
 * @param void *group  [in] : 同一グループ 既に確保されたnio_server, nio_client, netio_init_groupで初期化されたグループを渡す
 * @return nio_client : クライアントハンドル
 */
nio_client netio_init_client(const char *address, unsigned short port, unsigned int tcpbuffsize, unsigned int conbuffsize, void *group)
{
	struct in_addr addr;
	if (inet_aton(address, &addr) == 0)
	{
		_PRINTF("%s : invalid address [%s]\n", __func__, address);
		return NIO_INVALID_HANDLE;
	}

	tcp_t *cli = netio_init_tcp(tcpbuffsize, conbuffsize);
	if (cli == NIO_INVALID_HANDLE)
	{
		return cli;
	}
	cli->addr.sin_family = AF_INET;
	cli->addr.sin_addr.s_addr = addr.s_addr;
	cli->addr.sin_port = htons(port);

	cli->event_base = group ? ((tcp_t *)group)->event_base : event_base_new();

	// timer イベントの設定
	memset(&(cli->event), 0, sizeof(struct event));
	evtimer_set(&(cli->event), __timer_event_callback, cli);
	event_base_set(cli->event_base, &(cli->event));
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(&(cli->event), &tv);

	strncpy(cli->client.address, address, sizeof(cli->client.address) - 1);
	cli->client.address[sizeof(cli->client.address) - 1] = '\0';
	cli->client.port = port;

	cli->client.close_func = NULL;
	cli->client.recv_func = NULL;
	cli->client.parse_func = NULL;

	return (nio_client)cli;
}

/**
 * client 解放
 *
 * @param nio_client tcp [in] : 解放するクライアント
 */
void netio_release_client(nio_client tcp)
{
	tcp_t *cli = (tcp_t *)tcp;

	if (cli->connection_a != NULL)
	{
		// 有効なコネクションをclose
		connection_t *c;
		for (c = get_element_first(cli->connection_a); c; c = get_element_next(cli->connection_a, c))
		{
			if (c->close_func)
			{
				// close callbackを呼ぶ
				c->close_func(c, -1);
			}
			CONN_CLEAR(c);
		}
		release_pool(cli->connection_a);
		cli->connection_a = NULL;
	}

	// イベントの終了
	event_del(&(cli->event));

	// 書き込みバッファメモリの開放
	if (cli->wbuffer_m != NULL)
	{
		message_release(cli->wbuffer_m);
		cli->wbuffer_m = NULL;
	}
	// recv bufferの解放
	if (cli->rbuffer_a != NULL)
	{
		release_pool(cli->rbuffer_a);
		cli->rbuffer_a = NULL;
	}

	memset(cli, 0, sizeof(tcp_t));
	free(cli);
}

/**
 * client 接続
 *
 * @param nio_client nclient
 * @return nio_conn
 */
nio_conn netio_client_connect(nio_client nclient)
{
	tcp_t *cli = NULL;
	NETIO_TO_TCP(cli, nclient, NULL);

	// コネクション確保
	connection_t *conn = (connection_t *)pool_alloc(cli->connection_a);
	if (conn == NULL)
	{
		_PRINTF("%s : no more connection!!\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	// socket作成
	if ((conn->soc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		_PRINTF("%s : socket failed\n", __func__);
		pool_free(cli->connection_a, conn);
		return NIO_INVALID_HANDLE;
	}

	int val = 1;
	setsockopt(conn->soc, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)); // no delay
	setsockopt(conn->soc, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)); // keepalive
	fcntl(conn->soc, F_SETFL, O_NONBLOCK | O_RDWR);						// non blocking
	fcntl(conn->soc, F_SETFD, FD_CLOEXEC);								// close on exec

	// connect
	if (connect(conn->soc, (struct sockaddr *)&(cli->addr), sizeof(cli->addr)) == -1)
	{
		if (errno != EWOULDBLOCK && errno != EINPROGRESS)
		{
			_PRINTF("%s : connect failed : %s(%d) : %X : %d\n",
					__func__, cli->client.address, cli->client.port, cli->addr.sin_addr.s_addr, errno);
			close(conn->soc);
			pool_free(cli->connection_a, conn);
			return NIO_INVALID_HANDLE;
		}
	}

	memset(&(conn->event), 0, sizeof(struct event));
	event_set(&(conn->event), conn->soc, EV_READ | EV_PERSIST, __read_event_callback, conn);
	event_base_set(cli->event_base, &(conn->event));
	event_add(&(conn->event), NULL);

	conn->close_func = cli->client.close_func;
	conn->recv_func = cli->client.recv_func;
	conn->parse_func = cli->client.parse_func;
	conn->rbuffer.len = 0;
	conn->rbuffer.next = NULL;
	conn->parent = cli;

	return (nio_conn)conn;
}

/**
 * client 接続
 *
 * init_clientで指定したaddress, portを用いず、引数のaddress, portを用いて接続する
 *
 * @param nio_client nclient [in]
 * @param const char *address [in]
 * @param unsigned short port [in]
 * @return nio_conn
 */
nio_conn netio_client_connect_by_address(nio_client nclient, const char *address, unsigned short port)
{
	tcp_t *cli = NULL;
	NETIO_TO_TCP(cli, nclient, NULL);

	struct in_addr iaddr;
	struct sockaddr_in addr;

	if (inet_aton(address, &iaddr) == 0)
	{
		_PRINTF("%s : invalid address [%s]\n", __func__, address);
		return NIO_INVALID_HANDLE;
	}
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = iaddr.s_addr;
	addr.sin_port = htons(port);

	// コネクション確保
	connection_t *conn = (connection_t *)pool_alloc(cli->connection_a);
	if (conn == NULL)
	{
		_PRINTF("%s : no more connection!!\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	// socket作成
	if ((conn->soc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		_PRINTF("%s : socket failed\n", __func__);
		pool_free(cli->connection_a, conn);
		return NIO_INVALID_HANDLE;
	}

	int val = 1;
	//	  setsockopt(conn->soc, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)); // no delay
	setsockopt(conn->soc, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)); // keepalive
	fcntl(conn->soc, F_SETFL, O_NONBLOCK | O_RDWR);						// non blocking

	// connect
	if (connect(conn->soc, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
		if (errno != EWOULDBLOCK && errno != EINPROGRESS)
		{
			_PRINTF("%s : connect failed : %s(%d) : %X : %d\n",
					__func__, address, port, addr.sin_addr.s_addr, errno);
			close(conn->soc);
			pool_free(cli->connection_a, conn);
			return NIO_INVALID_HANDLE;
		}
	}

	memset(&(conn->event), 0, sizeof(struct event));
	event_set(&(conn->event), conn->soc, EV_READ | EV_PERSIST, __read_event_callback, conn);
	event_base_set(cli->event_base, &(conn->event));
	event_add(&(conn->event), NULL);

	conn->close_func = cli->client.close_func;
	conn->recv_func = cli->client.recv_func;
	conn->parse_func = cli->client.parse_func;
	conn->rbuffer.len = 0;
	conn->rbuffer.next = NULL;
	conn->parent = cli;
	conn->pair = NULL;

	return (nio_conn)conn;
}

/**
 * コネクション使用数取得
 *
 * @param nio_tcp tcp
 * @return int
 */
int netio_tcp_get_conn_use_num(nio_tcp tcp)
{
	tcp_t *cli = NULL;
	NETIO_TO_TCP(cli, tcp, -1);

	return get_element_use_num(cli->connection_a);
}

/**
 * 設定アドレス取得
 *	server: 自分のlisten IP, port情報
 *	client: 接続先IP, port情報
 *
 * @param nio_tcp client [in]
 * @param char *buff [out]
 * @param int len [in]
 * @return char *
 */
char *netio_tcp_get_address(nio_tcp client, char *buff, int len)
{
	tcp_t *c = NULL;
	*buff = '\0';
	NETIO_TO_TCP(c, client, buff);

	unsigned char ip[4];
	memcpy(ip, &(c->addr.sin_addr.s_addr), sizeof(ip));
	snprintf(buff, len, "%d.%d.%d.%d:%u", ip[0], ip[1], ip[2], ip[3], ntohs(c->addr.sin_port));
	return buff;
}

/**
 * 設定IPの取得
 *
 * @param nio_tcp tcp [in]
 * @param char *buff [out]
 * @param int len [in]
 */
void netio_tcp_get_ip(nio_tcp tcp, char *buff, int len)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, );

	*buff = '\0';
	unsigned char ip[4];
	memcpy(ip, &(t->addr.sin_addr.s_addr), sizeof(ip));
	snprintf(buff, len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

	return;
}

/**
 * 設定port番号の取得
 *
 * @param nio_tcp tcp
 * @return unsigned short : ポート番号（network byte order）
 */
unsigned short netio_tcp_get_port(nio_tcp tcp)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, 0);

	return t->addr.sin_port;
}

/**
 * tcp bufferの取得
 *
 * @param nio_tcp tcp
 * @return unsigned char : buffer address
 */
char *netio_tcp_get_buffer(nio_tcp tcp)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, NULL);

	return t->svbuf;
}

nio_conn netio_tcp_get_conn_first(nio_tcp tcp)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, NIO_INVALID_HANDLE);

	return (nio_conn)get_element_first(t->connection_a);
}

nio_conn netio_tcp_get_conn_next(nio_tcp tcp, nio_conn c)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, NIO_INVALID_HANDLE);

	return (nio_conn)get_element_next(t->connection_a, c);
}

/**
 * connection切断
 *
 */
int netio_tcp_connection_close_all(nio_tcp tcp)
{
	tcp_t *t = NULL;
	NETIO_TO_TCP(t, tcp, 0);

	int count = 0;
	if (t->connection_a != NULL)
	{
		// 有効なコネクションをclose
		connection_t *c;
		for (c = get_element_first(t->connection_a); c; c = get_element_next(t->connection_a, c))
		{
			if (c->close_func)
			{
				// close callbackを呼ぶ
				c->close_func(c, -2);
			}
			CONN_CLEAR(c);
			count++;
		}
	}

	return count;
}

/******************************************************************************
 * weite bufferへのデータ登録
 *
 * @param tcp_t *tcp [in]
 * @param connection_t *c [in]
 * @param const char *data [in]
 * @param int len [in]
 * @return static int
 */
static inline int netio_tcp_append_write_buffer(tcp_t *tcp, connection_t *c, const char *data, int len)
{
	write_buffer_t *wb = NULL;

	int storedlen = 0;
	while (len > 0)
	{
		int datalen = MIN(len, RW_BUFFER_SIZE);

		wb = message_add(tcp->wbuffer_m, (uintptr_t)c);
		if (wb == NULL)
		{
			_PRINTF("%s : write_buffer message_add failed\n", __func__);
			return 0;
		}

		wb->conn = (nio_conn)c;
		memcpy(wb->buffer, data + storedlen, datalen);
		wb->buffer_len = datalen;

		storedlen += datalen;
		len -= datalen;
		_PRINTF("WBUFF : (%p) %d %d %d\n", c, datalen, storedlen, len);
	}
	return 1;
}

/**
 * weite bufferからのデータ消去
 *
 * @param tcp_t *tcp [ini]
 * @param connection_t *c [in]
 */
static void netio_tcp_delete_write_buffer(tcp_t *tcp, connection_t *c)
{
	message_del(tcp->wbuffer_m, (uintptr_t)c);
}

/**
 * weite bufferからのデータ送信
 *
 * @param nio_tcp t [in]
 * @param int count [in]
 */
static int netio_tcp_push_write_buffer(nio_tcp t, int count)
{
	tcp_t *tcp = NULL;
	int result = 0;
	NETIO_TO_TCP(tcp, t, 0);

	int i;
	for (i = 0; i < count; i++)
	{
		write_buffer_t *wb = message_get_one(tcp->wbuffer_m);
		if (wb == NULL)
		{
			// たまっているものはない
			return result;
		}

		connection_t *c = (connection_t *)wb->conn;
		int n = send(c->soc, wb->buffer, wb->buffer_len, MSG_NOSIGNAL);
		if (n < 0)
		{
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))
			{
				// continue
				return result;
			}
			else
			{
				// error
#if defined(_DEBUG)
				char tmp[256];
				strerror_r(errno, tmp, sizeof(tmp));
				_PRINTF("%s : send failed :%d, %s(%d)\n", __func__, n, tmp, errno);
#endif
				return result;
			}
		}
		_PRINTF("PUSH : %p %d %d\n", c, n, wb->buffer_len);
		result++;

		if (n < wb->buffer_len)
		{
			// 送りきれていない
			// データを縮小する
			wb->buffer_len = wb->buffer_len - n; // 残りbyte数
			memmove(wb->buffer, wb->buffer + n, wb->buffer_len);
			return result;
		}

		// 成功：bufferから一つ消す
		message_delete_one(tcp->wbuffer_m);
	}
	return result;
}

/******************************************************/
/**
 * event_baseを共有するための構造体初期化
 *
 * @return nio_server
 */
nio_server netio_init_group(void)
{
	tcp_t *sv = NULL;

	// サーバ構造体準備
	sv = (tcp_t *)calloc(1, sizeof(tcp_t));
	if (sv == NULL)
	{
		_PRINTF("%s : no more alloc\n", __func__);
		return NIO_INVALID_HANDLE;
	}
	memset(sv, 0, sizeof(tcp_t));

	// event_baseの設定
	sv->event_base = event_base_new();
	if (sv->event_base == NULL)
	{
		_PRINTF("%s : event_base_new failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	return (nio_server)sv;
}

/********************************************************/
/**
 * netio データ送信
 *
 * @param nio_conn ncon
 * @param char *data
 * @param int datalen
 * @return int
 */
int netio_sender(nio_conn ncon, char *data, int datalen)
{
	connection_t *c = NULL;

	NETIO_TO_CONNECTION(c, ncon, -2);

	tcp_t *t = c->parent;
	if (message_find(t->wbuffer_m, (uintptr_t)ncon))
	{
		// バッファにためているものがある
		_PRINTF("%s : append_write_buffer 1 : %p %d\n", __func__, c, datalen);
		// このデータもバッファに入れる
		if (netio_tcp_append_write_buffer(t, c, data, datalen) == 0)
		{
			_PRINTF("%s : netio_tcp_append_write_buffer failed (%p) %d\n", __func__, c, datalen);
			return -1;
		}
		return datalen;
	}

	int n = send(c->soc, data, datalen, MSG_NOSIGNAL);
	_PRINTF("%s : send : %p %d %d\n", __func__, c, datalen, n);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))
		{
			// continue
			n = 0;
		}
		else
		{
			// error
			char tmp[256];
			strerror_r(errno, tmp, sizeof(tmp));
			_PRINTF("%s : send failed :%d, %s(%d)\n", __func__, n, tmp, errno);
			return n;
		}
	}

	if (n < datalen)
	{
		// 送りきれていない
		_PRINTF("%s : append_write_buffer 2 : %p %d\n", __func__, c, datalen - n);
		if (netio_tcp_append_write_buffer(t, c, data + n, datalen - n) == 0)
		{
			_PRINTF("%s : netio_tcp_append_write_buffer failed (%p) %d\n", __func__, c, datalen - n);
			return -1;
		}
	}

	return n;
}

/**
 * netio データ送信
 *
 *	古いsender関数
 *
 * @param nio_conn ncon
 * @param char *data
 * @param int datalen
 * @return int
 */
int netio_sender_old(nio_conn ncon, char *data, int datalen)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, -2);

	int n = 0;
	int len = datalen;
	char *pdata = data;
	while (len > 0)
	{
		n = send(c->soc, pdata, len, MSG_NOSIGNAL);
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				continue;
			}
			// error
			_PRINTF("%s : send failed :%d, %d\n", __func__, n, errno);
			return n;
		}
		len -= n;
		pdata += n;
	}
	return datalen;
}

/**
 * connection切断
 *
 * @param nio_conn ncon [in]
 * @return int
 */
int netio_connection_close(nio_conn ncon)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, 0);

	if (c->close_func != NULL)
	{
		// close callback が指定されていたらcallbackを呼び出す
		c->close_func(ncon, -3);
	}
	CONN_CLEAR(c); // いきなり切断される。問題が起こるようなら後ほど修正します

	return 1;
}

/**
 * connectionのテスト
 *
 * @param nio_conn ncon [in]
 * @return int
 */
int netio_connection_is_valid(nio_conn ncon)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, 0);

	if (c->soc < 0)
	{
		return 0;
	}
	return 1;
}

/**
 * connection bufferの取得
 *
 * @param nio_conn ncon [in]
 * @return char *
 */
char *netio_connection_get_buffer(nio_conn ncon)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	return c->conbuf;
}

/**
 * 接続先アドレス：ポートの取得
 *
 * @param nio_conn ncon [in]
 * @param char *buff [out]
 * @param int len [in]
 *
 * @return char * : 引数のbuffをそのまま返します
 */
char *netio_connection_get_remote_address(nio_conn ncon, char *buff, int len)
{
	connection_t *c = NULL;
	*buff = '\0';
	NETIO_TO_CONNECTION(c, ncon, buff);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);

	if (getpeername(c->soc, (struct sockaddr *)&addr, &addrlen) != 0)
	{
		// 失敗
		return buff;
	}
	unsigned char ip[4];
	memcpy(ip, &(addr.sin_addr.s_addr), sizeof(ip));
	snprintf(buff, len, "%d.%d.%d.%d:%u", ip[0], ip[1], ip[2], ip[3], ntohs(addr.sin_port));
	return buff;
}

/**
 * 接続元アドレス：ポートの取得
 *
 * @param nio_conn ncon [in]
 * @param char *buff [out]
 * @param int len [in]
 *
 * @return char * : 引数のbuffをそのまま返します
 */
char *netio_connection_get_host_address(nio_conn ncon, char *buff, int len)
{
	connection_t *c = NULL;
	*buff = '\0';
	NETIO_TO_CONNECTION(c, ncon, buff);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);

	if (getsockname(c->soc, (struct sockaddr *)&addr, &addrlen) != 0)
	{
		// 失敗
		return buff;
	}
	unsigned char ip[4];
	memcpy(ip, &(addr.sin_addr.s_addr), sizeof(ip));
	snprintf(buff, len, "%d.%d.%d.%d:%u", ip[0], ip[1], ip[2], ip[3], ntohs(addr.sin_port));
	return buff;
}

/**
 * 使用中書き込みバッファの長さを取得
 *
 * sizeof(wb->buffer)以上のサイズはだたしく得ることができません
 *
 * @param nio_conn ncon [in]
 * @return int
 */
int netio_connection_get_wbuff_len(nio_conn ncon)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, 0);
	tcp_t *t = c->parent;
	if (t == NULL)
	{
		assert(0);
		return 0;
	}
	write_buffer_t *wb = message_find(t->wbuffer_m, (uintptr_t)ncon);
	if (wb != NULL)
	{
		return wb->buffer_len;
	}
	return 0;
}

/**
 * tcp_tの取得
 *
 * @param nio_conn ncon [in]
 * @return nio_tcp
 */
nio_tcp netio_get_tcp_by_conn(nio_conn ncon)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	return c->parent;
}

/********************************************************/
/**
 * netio server accept可否チェック関数設定
 *
 * @param nio_server nsv [in] :
 * @param accept_check_func checkfunc [in] :
 */
void netio_server_set_accept_check_func(nio_server nsv, accept_check_func checkfunc)
{
	tcp_t *server = NULL;
	NETIO_TO_TCP(server, nsv, );

	server->server.acheck_func = checkfunc;
}

/*
 * netio server acceptコールバック設定
 *
 * @param nio_server nsv
 * @param accept_callback callback
 */
void netio_server_set_accept_callback(nio_server nsv, accept_callback callback)
{
	tcp_t *server = NULL;
	NETIO_TO_TCP(server, nsv, );

	server->server.accept_func = callback;
}

/**
 * netio server受信コールバック設定
 *
 * @param nio_server nsv [in] :
 * @param recv_callback callback [in] :
 */
void netio_server_set_recv_callback(nio_server nsv, recv_callback callback)
{
	tcp_t *server = NULL;
	NETIO_TO_TCP(server, nsv, );

	server->server.listen_conn.recv_func = callback;
}

/**
 * netio server切断時コールバック設定
 *
 * @param nio_server nsv [in] :
 * @param close_callback callback [in] :
 */
void netio_server_set_close_callback(nio_server nsv, close_callback callback)
{
	tcp_t *server = NULL;
	NETIO_TO_TCP(server, nsv, );

	server->server.listen_conn.close_func = callback;
}

/**
 * netio server parse コールバック設定
 *
 * @param nio_server nsv [in] :
 * @param  parse_callback callback [in] :
 */
void netio_server_set_parse_callback(nio_server nsv, parse_callback callback)
{
	tcp_t *server = NULL;
	NETIO_TO_TCP(server, nsv, );

	server->server.listen_conn.parse_func = callback;
}

/**
 * netio client受信コールバック設定
 *
 * @param nio_client ncl [in] :
 * @param  recv_callback callback [in] :
 */
void netio_client_set_recv_callback(nio_client ncl, recv_callback callback)
{
	tcp_t *client = NULL;
	NETIO_TO_TCP(client, ncl, );

	client->client.recv_func = callback;
}

/**
 * netio client切断時コールバック設定
 *
 * @param nio_client ncl [in] :
 * @param close_callback callback [in] :
 */
void netio_client_set_close_callback(nio_client ncl, close_callback callback)
{
	tcp_t *client = NULL;
	NETIO_TO_TCP(client, ncl, );

	client->client.close_func = callback;
}

/**
 * netio client切断時コールバック設定
 *
 * @param nio_client ncl [in] :
 * @param parse_callback callback [in] :
 */
void netio_client_set_parse_callback(nio_client ncl, parse_callback callback)
{
	tcp_t *client = NULL;
	NETIO_TO_TCP(client, ncl, );

	client->client.parse_func = callback;
}

/**
 * netio connction受信コールバック設定
 *
 * @param nio_conn ncon [in] :
 * @param recv_callback callback [in] :
 * @return recv_callback
 */
recv_callback netio_conn_set_recv_callback(nio_conn ncon, recv_callback callback)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	recv_callback old_callback = c->recv_func;
	c->recv_func = callback;

	return old_callback;
}

/**
 * netio connction切断時コールバック設定
 *
 * @param nio_conn ncon [in] :
 * @param close_callback callback [in] :
 * @return close_callback
 */
close_callback netio_conn_set_close_callback(nio_conn ncon, close_callback callback)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	close_callback old_callback = c->close_func;
	c->close_func = callback;

	return old_callback;
}

/**
 * netio parseコールバック設定
 *
 * @param nio_conn ncon [in] :
 * @param parse_callback callback [in] :
 * @return parse_callback
 */
parse_callback netio_conn_set_parse_callback(nio_conn ncon, parse_callback callback)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	parse_callback old_callback = c->parse_func;
	c->parse_func = callback;

	return old_callback;
}

/**
 * netio connction受信可否チェック関数設定
 *
 * @param nio_conn ncon [in] :
 * @param recv_check_func checkfunc [in] :
 * @return recv_check_func
 */
recv_check_func netio_conn_set_recv_check_func(nio_conn ncon, recv_check_func checkfunc)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, NULL);

	recv_check_func old_checkfunc = c->rcheck_func;
	c->rcheck_func = checkfunc;

	return old_checkfunc;
}

/**
 * netio pair connction設定
 *
 * @param nio_conn ncon [in] :
 * @param nio_conn pair_conn [in] :
 * @return void
 */
void netio_conn_set_pair_connection(nio_conn ncon, nio_conn pair_conn)
{
	connection_t *c = NULL;
	NETIO_TO_CONNECTION(c, ncon, );

	connection_t *pairc = NULL;
	NETIO_TO_CONNECTION(pairc, pair_conn, );

	// 片方向
	c->pair = pairc;

	return;
}

/***********************************************************************/
/****** multicast *****/

typedef struct _multicast
{
	int soc;			// socket
	struct event event; // event

	struct event_base *event_base; // event base
	struct sockaddr_in addr;	   // server address info

	multicastcallback recv_func;
} multicast_t;

/**
 * multicast event callback
 *
 * @param int soc
 * @param short events
 * @param void *user_data
 */
static void __multicast_recv_event_callback(int soc, short events, void *user_data)
{
	int ret = -1;
	char buff[BUFFER_SIZE];

	multicast_t *m = (multicast_t *)user_data;

	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	if (events & EV_READ)
	{
		memset(&client_addr, 0, sizeof(client_addr));
		client_addr_len = sizeof(client_addr);
		ret = recvfrom(soc, buff, sizeof(buff), MSG_NOSIGNAL, (struct sockaddr *)&client_addr, &client_addr_len);
		if (ret == 0)
		{
			return;
		}
		else if (ret < 0)
		{
			if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR))
			{
				// 後でもう一度呼ぶ
				return;
			}
			// 上記以外のエラー
			_PRINTF("%s : read failed : %d\n", __func__, errno);
			return;
		}
		else
		{
			if (m->recv_func != NULL)
			{
				// recv callbackが指定されていたらcallbackを呼び出す
				m->recv_func(m, buff, ret);
			}
			else
			{
				_PRINTF("read[%d](%d):(%X/%d):(%X/%d)=%s\n", soc, ret,
						client_addr.sin_addr.s_addr, ntohs(client_addr.sin_port),
						m->addr.sin_addr.s_addr, ntohs(m->addr.sin_port), buff);
			}
		}
	}
	else
	{
		_PRINTF("%s : event = 0x%X\n", __func__, events);
	}
}

/**
 * multicast 通信初期化
 *
 * multicast通信に必要な初期化を行う
 * 一つのportで受信・送信の両方を行う
 *
 * @param char *address [in] : multicast IPアドレス(規約に則ったアドレスを指定すること)
 * @param unsigned short port [in] : multicast 通信使用port
 * @return nio_multicast : multicast 通信ハンドル
 */
nio_multicast netio_multicast_init(char *address, unsigned short port)
{
	multicast_t *m = (multicast_t *)calloc(1, sizeof(multicast_t));
	if (m == NULL)
	{
		_PRINTF("%s : multicast_t calloc failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}
	memset(m, 0, sizeof(multicast_t));

	// socket作成
	if ((m->soc = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
	{
		_PRINTF("%s : socket failed\n", __func__);
		free(m);
		return NIO_INVALID_HANDLE;
	}

	// portへのbind
	m->addr.sin_port = htons(port);
	m->addr.sin_family = AF_INET;
	m->addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(m->soc, (struct sockaddr *)&(m->addr), sizeof(m->addr)) == -1)
	{
		_PRINTF("%s : bind failed\n", __func__);
		free(m);
		return NIO_INVALID_HANDLE;
	}

	// multicast 設定
	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr(address);
	mreq.imr_interface.s_addr = INADDR_ANY;
	unsigned long ttl = 8;
	int val = 0;

	setsockopt(m->soc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)); // member ship
	setsockopt(m->soc, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));	// TTL set
	setsockopt(m->soc, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));	// loopback off

	fcntl(m->soc, F_SETFL, O_NONBLOCK | O_RDWR); // non block

	// acceptイベントの設定
	m->event_base = event_base_new();
	memset(&(m->event), 0, sizeof(struct event));
	event_set(&(m->event), m->soc, EV_READ | EV_PERSIST, __multicast_recv_event_callback, m);
	event_base_set(m->event_base, &(m->event));
	event_add(&(m->event), NULL);

	m->recv_func = NULL;

	// addressを入れなおす（送信用）
	m->addr.sin_port = htons(port);
	m->addr.sin_family = AF_INET;
	m->addr.sin_addr.s_addr = inet_addr(address);

	return (nio_multicast)m;
}

/**
 * multicast データ送信
 *
 * multicast通信でデータを送信する
 *
 * @param nio_multicast mc
 * @param char *data
 * @param int len
 * @return int
 */
int netio_multicast_send(nio_multicast mc, char *data, int len)
{
	multicast_t *m = (multicast_t *)mc;

	return sendto(m->soc, data, len, MSG_NOSIGNAL, (struct sockaddr *)&(m->addr), sizeof(m->addr));
}

/**
 * multicast polling処理
 *
 * @param nio_multicast mc
 * @param int timeout
 */
void netio_multicast_poll(nio_multicast mc, int timeout)
{
	multicast_t *m = (multicast_t *)mc;

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = timeout;

	event_base_loopexit(m->event_base, &tv);
	event_base_loop(m->event_base, 0);
}

/**
 * netio multicast受信時コールバック関数設定
 *
 * @param nio_multicast mc [in] :
 * @param multicastcallback callback [in] :
 * @return multicastcallback
 */
multicastcallback netio_multicast_recv_callback(nio_multicast mc, multicastcallback callback)
{
	multicast_t *m = (multicast_t *)mc;

	multicastcallback old_callback = m->recv_func;
	m->recv_func = callback;

	return old_callback;
}

/***********************************************************************/
/****** udp *****/

typedef struct _udp_t
{
	int soc;			// socket
	struct event event; // event

	struct event_base *event_base; // event base
	struct sockaddr_in addr;	   // server address info

	udpcallback recv_func;
} udp_t;

/**
 * udp event callback
 *
 * @param int soc
 * @param short events
 * @param void *user_data
 */
static void __udp_recv_event_callback(int soc, short events, void *user_data)
{
	int ret = -1;
	char buff[BUFFER_SIZE];

	udp_t *udp = (udp_t *)user_data;

	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	if (events & EV_READ)
	{
		memset(&client_addr, 0, sizeof(client_addr));
		client_addr_len = sizeof(client_addr);
		ret = recvfrom(soc, buff, sizeof(buff), MSG_NOSIGNAL, (struct sockaddr *)&client_addr, &client_addr_len);
		if (ret == 0)
		{
			return;
		}
		else if (ret < 0)
		{
			if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR))
			{
				// 後でもう一度呼ぶ
				return;
			}
			// 上記以外のエラー
			_PRINTF("%s : read failed : %d\n", __func__, errno);
			return;
		}
		else
		{
			if (udp->recv_func != NULL)
			{
				// recv callbackが指定されていたらcallbackを呼び出す
				udp->recv_func(udp, client_addr, buff, ret);
			}
			else
			{
				_PRINTF("read[%d](%d):(%X/%d):(%X/%d)=%s\n", soc, ret,
						client_addr.sin_addr.s_addr, ntohs(client_addr.sin_port),
						udp->addr.sin_addr.s_addr, ntohs(udp->addr.sin_port), buff);
			}
		}
	}
	else
	{
		_PRINTF("%s : event = 0x%X\n", __func__, events);
	}
}

/**
 * udp 通信初期化
 *
 * udp通信に必要な初期化を行う
 * 一つのportで受信・送信の両方を行う
 *
 * @param char *address [in] : bind IPアドレス(特に指定がなければ 0.0.0.0)
 * @param unsigned short port [in] : udp port
 * @return nio_udp : udp 通信ハンドル
 */
nio_udp netio_udp_init(char *address, unsigned short port)
{
	udp_t *udp = (udp_t *)calloc(1, sizeof(udp_t));
	if (udp == NULL)
	{
		_PRINTF("%s : udp_t calloc failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	// socket作成
	if ((udp->soc = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
	{
		_PRINTF("%s : socket failed\n", __func__);
		free(udp);
		return NIO_INVALID_HANDLE;
	}

	// portへのbind
	udp->addr.sin_port = htons(port);
	udp->addr.sin_family = AF_INET;
	udp->addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(udp->soc, (struct sockaddr *)&(udp->addr), sizeof(udp->addr)) == -1)
	{
		_PRINTF("%s : bind failed\n", __func__);
		free(udp);
		return NIO_INVALID_HANDLE;
	}

	// acceptイベントの設定
	udp->event_base = event_base_new();
	memset(&(udp->event), 0, sizeof(struct event));
	event_set(&(udp->event), udp->soc, EV_READ | EV_PERSIST, __udp_recv_event_callback, udp);
	event_base_set(udp->event_base, &(udp->event));
	event_add(&(udp->event), NULL);

	udp->recv_func = NULL;

	return (nio_udp)udp;
}

/**
 * udp データ送信
 *
 * udp通信でデータを送信する
 *
 * @param nio_udp u
 * @param char *data
 * @param int len
 * @return int
 */
int netio_udp_send(nio_udp u, struct sockaddr_in dst_addr, char *data, int len)
{
	udp_t *udp = (udp_t *)u;

	return sendto(udp->soc, data, len, 0, (struct sockaddr *)&(dst_addr), sizeof(dst_addr));
}

/**
 * udp データ送信
 *
 * udp通信でデータを送信する
 *
 * @param nio_udp u
 * @param char *data
 * @param int len
 * @return int
 */
int netio_udp_send_by_address(nio_udp u, const char *address, unsigned short port, char *data, int len)
{
	struct sockaddr_in dst_addr;

	memset(&(dst_addr), 0, sizeof(dst_addr));
	dst_addr.sin_port = htons(port);
	dst_addr.sin_family = AF_INET;
	dst_addr.sin_addr.s_addr = inet_addr(address);

	return netio_udp_send(u, dst_addr, data, len);
}

/**
 * udp polling処理
 *
 * @param nio_udp mc
 * @param int timeout
 */
void netio_udp_poll(nio_udp u, int timeout)
{
	udp_t *udp = (udp_t *)u;

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = timeout;

	event_base_loopexit(udp->event_base, &tv);
	event_base_loop(udp->event_base, 0);
}

/**
 *	udp受信時コールバック関数設定
 *
 * @param nio_udp mc [in] :
 * @param udpcallback callback [in] :
 * @return udpcallback
 */
udpcallback netio_udp_recv_callback(nio_udp u, udpcallback callback)
{
	udp_t *udp = (udp_t *)u;

	udpcallback old_callback = udp->recv_func;
	udp->recv_func = callback;

	return old_callback;
}

/**
 * debug flag の設定
 *
 * DEBUG BUILD時のみ有効
 *
 * @param int debug [in] :
 */
void netio_set_debug(int debug)
{
	NIO_DEBUG = debug;
}

/***********************************************************************/
/****** raw *****/

typedef struct _raw_t
{
	int soc;				 // socket
	struct sockaddr_in addr; // server address info

	struct event event;			   // event
	struct event_base *event_base; // event base

	rawcallback recv_func; // receivce callback
} raw_t;

#if 0
/**
 * dsr event callback
 *
 * @param int soc
 * @param short events
 * @param void *user_data
 */
static void __raw_recv_event_callback(int soc, short events, void *user_data)
{
	int ret = -1;
	char buff[BUFFER_SIZE];

	raw_t *raw = (raw_t *)user_data;

	struct sockaddr_in src_addr;
	socklen_t src_addr_len;
	if (events & EV_READ){
		memset(&src_addr, 0, sizeof(src_addr));
		src_addr_len = sizeof(src_addr);
		ret = recvfrom(soc, buff, sizeof(buff), MSG_NOSIGNAL, (struct sockaddr *)&src_addr, &src_addr_len);
		if (ret == 0) {
			_PRINTF("%s : recvfrom ret=0\n", __func__);
			return;
		}
		else if (ret < 0) {
			if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR)) {
				// 後でもう一度呼ぶ
				return;
			}
			// 上記以外のエラー
			_PRINTF("%s : recvfrom failed : %d\n", __func__, errno);
			return;
		}

		// recv_funcを呼び出す
		if (raw->recv_func != NULL) {
			// recv callbackが指定されていたらcallbackを呼び出す
			int cresult = raw->recv_func(raw, &src_addr, buff, ret);
			if (cresult < 0) {
				// end?
			}
		}
		else {
			_PRINTF("read[%d](%d):(%X/%d):(%X/%d)=%s\n", soc, ret,
					src_addr.sin_addr.s_addr, ntohs(src_addr.sin_port),
					raw->addr.sin_addr.s_addr, ntohs(raw->addr.sin_port), buff);
		}
	}
	else {
		_PRINTF("%s : event = 0x%X\n", __func__, events);
	}
}
#endif

nio_raw netio_raw_init(unsigned short port, rawcallback callback)
{
	raw_t *raw = (raw_t *)calloc(1, sizeof(raw_t));
	if (raw == NULL)
	{
		_PRINTF("%s : udp_t calloc failed\n", __func__);
		return NIO_INVALID_HANDLE;
	}

	// socket作成
	//	  if ((raw->soc = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1) {
	if ((raw->soc = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP))) == -1)
	{
		_PRINTF("%s : socket failed : %d : %s\n", __func__, errno, strerror(errno));
		free(raw);
		return NIO_INVALID_HANDLE;
	}

	// setsockopt
	//	  int flag = 1;
	//	  if ((setsockopt(raw->soc, IPPROTO_IP, IP_HDRINCL, &flag, sizeof(flag))) <0) {
	//		  _PRINTF("%s : setsockopt failed : %d : %s\n", __func__, errno, strerror(errno));
	//		  close(raw->soc);
	//		  free(raw);
	//		  return NIO_INVALID_HANDLE;
	//	  }
	//
	// interfaceへのbind
	struct ifreq ifr;
	memset(&ifr, 0xFF, sizeof(ifr));
	strncpy(ifr.ifr_name, "eth0:0", IFNAMSIZ);
	ioctl(raw->soc, SIOCGIFINDEX, &ifr);
	int interface_index = ifr.ifr_ifindex;
	_PRINTF("%s : interface=%s, index=%d\n", __func__, ifr.ifr_name, interface_index);

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET; /* allways AF_PACKET */
								//	  sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_protocol = htons(ETH_P_IP);
	sll.sll_ifindex = interface_index;
	if (bind(raw->soc, (struct sockaddr *)&(sll), sizeof(sll)) == -1)
	{
		_PRINTF("%s : bind failed : %d : %s\n", __func__, errno, strerror(errno));
		netio_raw_release(raw);
		return NIO_INVALID_HANDLE;
	}

	// promiscas mode
	strncpy(ifr.ifr_name, "eth0:0", IFNAMSIZ);
	if (ioctl(raw->soc, SIOCGIFFLAGS, &ifr) < 0)
	{
		_PRINTF("%s : ioctl failed(SIOCGIFFLAGS) : %d : %s\n", __func__, errno, strerror(errno));
		netio_raw_release(raw);
		return NIO_INVALID_HANDLE;
	}
	ifr.ifr_flags = ifr.ifr_flags | IFF_PROMISC;
	if (ioctl(raw->soc, SIOCSIFFLAGS, &ifr) < 0)
	{
		_PRINTF("%s : ioctl failed(SIOCSIFFLAGS) : %d : %s\n", __func__, errno, strerror(errno));
		netio_raw_release(raw);
		return NIO_INVALID_HANDLE;
	}

	// portへのbind
	//	  raw->addr.sin_port = htons(port);
	//	  raw->addr.sin_port = 0;
	//	  raw->addr.sin_family = AF_INET;
	//	  raw->addr.sin_addr.s_addr = INADDR_ANY;
	//	  if (bind(raw->soc, (struct sockaddr *)&(raw->addr), sizeof(raw->addr)) == -1) {
	//		  _PRINTF("%s : bind failed : %d : %s\n", __func__, errno, strerror(errno));
	//		  netio_raw_release(raw);
	//		  return NIO_INVALID_HANDLE;
	//	  }

	//	  // receiveイベントの設定
	//	  raw->event_base = event_base_new();
	//	  event_set(&(raw->event), raw->soc, EV_READ|EV_PERSIST, __raw_recv_event_callback, raw);
	//	  event_base_set(raw->event_base, &(raw->event));
	//	  event_add(&(raw->event), NULL);

	// receive call back 関数
	raw->recv_func = callback;

	return (nio_raw)raw;
}

void netio_raw_release(nio_raw raw)
{
	raw_t *r = (raw_t *)raw;
	if (r->event_base != NULL)
	{
		// イベントの終了
		event_del(&(r->event));
	}
	if (r->soc >= 0)
	{
		close(r->soc);
	}
	free(r);
}

int netio_raw_sendto(nio_raw raw, struct sockaddr_in *send_to_addr, char *data, int len)
{
	raw_t *r = (raw_t *)raw;
	return sendto(r->soc, data, len, 0, (struct sockaddr *)send_to_addr, sizeof(struct sockaddr_in));
}

void netio_raw_poll(nio_raw r, int timeout)
{
	//	  struct timeval tv;
	//	  tv.tv_sec = 0;
	//	  tv.tv_usec = timeout;

	raw_t *raw = (raw_t *)r;

	char buff[BUFFER_SIZE];

	//_PRINTF("%s : recv\n", __func__);
	//	  int ret = recvfrom(raw->soc, buff, sizeof(buff), MSG_NOSIGNAL, (struct sockaddr *)&src_addr, &src_addr_len);
	int ret = recv(raw->soc, buff, sizeof(buff), 0);
	if (ret == 0)
	{
		_PRINTF("%s : recv ret=0\n", __func__);
		return;
	}
	else if (ret < 0)
	{
		if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR))
		{
			// 後でもう一度呼ぶ
			return;
		}
		// 上記以外のエラー
		_PRINTF("%s : recv failed : %d\n", __func__, errno);
		return;
	}

	// recv_funcを呼び出す
	if (raw->recv_func != NULL)
	{
		// recv callbackが指定されていたらcallbackを呼び出す
		//_PRINTF("%s : callback\n", __func__);
		int cresult = raw->recv_func(raw, buff, ret);
		if (cresult < 0)
		{
			// end?
		}
	}
	else
	{
		_PRINTF("read[%d](%d):(%X/%d)=%s\n", raw->soc, ret,
				raw->addr.sin_addr.s_addr, ntohs(raw->addr.sin_port), buff);
	}
	//	  event_base_loopexit(r->event_base, &tv);
	//	  event_base_loop(r->event_base, 0);
}

/***********************************************************************/
/****** parser *****/

int netio_parse16(const char *data, int datalen, char *parsed_data, int *parsed_data_len)
{
	int max_parsed_len = *parsed_data_len;
	*parsed_data_len = 0;
	// 16bit length parser
	if ((unsigned int)datalen <= sizeof(uint16_t))
	{
		// データが届ききっていない
		return 0;
	}

	uint16_t tmplen = 0;
	uint16_t len = 0;
	memcpy(&tmplen, data, sizeof(uint16_t));
	len = ntohs(tmplen);
	data += sizeof(uint16_t);

	if ((len + sizeof(uint16_t)) > (unsigned int)datalen)
	{
		// データが届ききっていない
		return 0;
	}
	if (len > max_parsed_len)
	{
		// データ入りきらない
		*parsed_data_len = -1;
		return -1;
	}

	// データのコピー
	memcpy(parsed_data, data, len);
	*parsed_data_len = len;
	return (len + sizeof(uint16_t));
}

int netio_pack16_length(char *pack_data, int datalen)
{
	uint16_t len = (uint16_t)datalen;
	uint16_t tmplen = htons(len);

	memcpy(pack_data, &tmplen, sizeof(uint16_t));

	return sizeof(uint16_t);
}

int netio_pack16(const char *data, int datalen, char *pack_data, int max_pack_data)
{
	// 16bit length pack
	if ((uint16_t)max_pack_data < (uint16_t)datalen + sizeof(uint16_t))
	{
		// データが入りきらない
		return -1;
	}

	int hdlen = netio_pack16_length(pack_data, datalen);
	memcpy(pack_data + hdlen, data, datalen);

	return (datalen + hdlen);
}

int netio_parse32(const char *data, int datalen, char *parsed_data, int *parsed_data_len)
{
	int max_parsed_len = *parsed_data_len;
	*parsed_data_len = 0;

	// 32bit length parser(int=32bitの環境では実質使えるのは31bit)
	if ((unsigned int)datalen <= sizeof(uint32_t))
	{
		// データが届ききっていない
		return 0;
	}

	uint32_t tmplen = 0;
	uint32_t len = 0;
	memcpy(&tmplen, data, sizeof(uint32_t));
	len = ntohl(tmplen);
	data += sizeof(uint32_t);

	if (((unsigned int)len + sizeof(uint32_t)) > (unsigned int)datalen)
	{
		// データが届ききっていない
		return 0;
	}
	if (len > (unsigned int)max_parsed_len)
	{
		// データ入りきらない
		*parsed_data_len = -1;
		return -1;
	}

	// データのコピー
	memcpy(parsed_data, data, len);
	*parsed_data_len = len;
	return (int)(len + sizeof(uint32_t));
}

int netio_pack32_length(char *pack_data, int datalen)
{
	uint32_t len = (uint32_t)datalen;
	uint32_t tmplen = htonl(len);

	memcpy(pack_data, &tmplen, sizeof(uint32_t));

	return sizeof(uint32_t);
}

int netio_pack32(const char *data, int datalen, char *pack_data, int max_pack_data)
{
	// 32bit length pack
	if ((uint32_t)max_pack_data < (uint32_t)datalen + sizeof(uint32_t))
	{
		// データが入りきらない
		return -1;
	}

	int hdlen = netio_pack32_length(pack_data, datalen);
	memcpy(pack_data + hdlen, data, datalen);

	return (datalen + hdlen);
}

int netio_parse_text(const char *data, int datalen, char *parsed_data, int *parsed_data_len)
{
	int max_parsed_len = *parsed_data_len;
	*parsed_data_len = 0;

	char *pdata = (char *)data;
	char *poutput = parsed_data;
	int i;

	// 改行parser
	for (i = 0; i < datalen; i++, pdata++, poutput++)
	{
		*poutput = *pdata;
		if ((*poutput == '\n') || (*poutput == '\0'))
		{
			// 改行もしくはtermで終了
			*poutput = '\0';
			*parsed_data_len = i + 1;
			return *parsed_data_len;
		}
		else if (*poutput == '\r')
		{
			*poutput = '\0';
		}
		if (i > max_parsed_len)
		{
			// 入りきらない
			*parsed_data_len = -1;
			return -1;
		}
	}

	// データが届ききっていない
	return 0;
}

int netio_pack_text(const char *data, int datalen, char *pack_data, int max_pack_data)
{
	if (max_pack_data > datalen + 1)
	{
		// データが入りきらない
		return -1;
	}

	memcpy(pack_data, data, datalen);
	if ((pack_data[datalen] != '\n') && (pack_data[datalen] != '\0'))
	{
		pack_data[datalen] = '\n';
		datalen++;
	}

	return datalen;
}
