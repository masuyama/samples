/**
 * netio.h
 *
 * ネットワークIO
 *
 *
 *
 */
#if !defined(__NETIO_H_INCLUDED__)
#define __NETIO_H_INCLUDED__

#ifdef __cplusplus
extern "C"
{
#endif

#include <netinet/in.h>

  //=======================================================================/

#define NIO_MAX_ADDRESS_LEN 32  // 最大アドレス長さ
#define NIO_INVALID_HANDLE NULL // 無効な nio_*

#define NIO_BUFFER_SIZE 65536 // max recv buffer size

#define _ENABLE_MESSAGE_BUFFER

  typedef void *nio_tcp;
  typedef void *nio_server;
  typedef void *nio_client;
  typedef void *nio_conn;
  typedef void *nio_multicast;
  typedef void *nio_udp;
  typedef void *nio_raw;

  //=======================================================================/

  // 初期化
  void netio_init(void);

  // polling処理
  int netio_poll(void);
  void netio_tcp_poll(nio_tcp ntcp, int timeout);
  void netio_server_poll(nio_server sv, int timeout);
  void netio_client_poll(nio_client cl, int timeout);

  // サーバ
  nio_server netio_init_server(unsigned short listen_port, unsigned int tcpbuffsize, unsigned int conbuffsize, void *group); // 初期化
  void netio_release_server(nio_server s);                                                                                   // 解放

  // クライアント
  nio_client netio_init_client(const char *address, unsigned short port, unsigned int tcpbuffsize, unsigned int conbuffsize, void *group); // 初期化
  void netio_release_client(nio_client tcp);                                                                                               // 解放
  nio_conn netio_client_connect(nio_client client);                                                                                        // 接続
  nio_conn netio_client_connect_by_address(nio_client nclient, const char *address, unsigned short port);                                  // 接続(address,port指定)

  // サーバ・クライアント共通
  char *netio_tcp_get_address(nio_tcp tcp, char *buff, int len); // 設定されているアドレス:ポートを取得
  void netio_tcp_get_ip(nio_tcp tcp, char *buff, int len);       // 設定されているアドレスを取得
  unsigned short netio_tcp_get_port(nio_tcp tcp);                // 設定されているport番号を取得
  char *netio_tcp_get_buffer(nio_tcp tcp);                       // tcp bufferの取得
  int netio_tcp_get_conn_use_num(nio_tcp tcp);                   // コネクション数を取得

  int netio_tcp_connection_close_all(nio_tcp tcp); // 全コネクション切断

  // コネクション取得
  nio_conn netio_tcp_get_conn_first(nio_tcp tcp);
  nio_conn netio_tcp_get_conn_next(nio_tcp tcp, nio_conn c);
// 要素SCAN
#define NIO_CONN_SCAN(tcp, c) for (c = netio_tcp_get_conn_first(tcp); c != NIO_INVALID_HANDLE; c = netio_tcp_get_conn_next(tcp, c))

  // groupの初期化
  nio_server netio_init_group(void);

  // コネクション
  int netio_sender(nio_conn conn, char *data, int datalen); // 送信

  int netio_connection_close(nio_conn conn);         // 切断（close callbackは呼ばれません）
  int netio_connection_is_valid(nio_conn ncon);      // 有効性のテスト
  char *netio_connection_get_buffer(nio_conn ncon);  // コネクションバッファの取得
  nio_tcp netio_get_tcp_by_conn(nio_conn ncon);      // コネクションの生成元nio_tcp(= nio_server or nio_client)を取得
  int netio_connection_get_wbuff_len(nio_conn ncon); // コネクションの書き込み保存バッファ使用量を取得

  // アドレス情報を取得
  char *netio_connection_get_remote_address(nio_conn ncon, char *buff, int len);
  char *netio_connection_get_host_address(nio_conn ncon, char *buff, int len);

  // コールバック関数type定義
  typedef int (*accept_callback)(nio_conn conn);
  typedef int (*close_callback)(nio_conn conn, int result);
  typedef int (*recv_callback)(nio_conn conn, char *data, int datalen);
  typedef int (*parse_callback)(const char *data, int datalen, char *parsed_data, int *max_parsed_data);
  typedef int (*recv_check_func)(nio_conn conn);
  typedef int (*accept_check_func)(nio_server sv);

  // 各種コールバック設定
  void netio_server_set_accept_callback(nio_server sv, accept_callback callback);       // サーバaccept
  void netio_server_set_recv_callback(nio_server sv, recv_callback callback);           // サーバデータ受信
  void netio_server_set_close_callback(nio_server sv, close_callback callback);         // サーバconnection close
  void netio_server_set_parse_callback(nio_server nsv, parse_callback callback);        // サーバデータparse
  void netio_server_set_accept_check_func(nio_server nsv, accept_check_func checkfunc); // サーバaccept可否チェック
  void netio_client_set_recv_callback(nio_client cl, recv_callback callback);           // クライアントデータ受信
  void netio_client_set_close_callback(nio_client cl, close_callback callback);         // クライアントconnection close
  void netio_client_set_parse_callback(nio_client ncl, parse_callback callback);        // クライアントデータparse

  // コネクションへのコールバック設定
  recv_callback netio_conn_set_recv_callback(nio_conn conn, recv_callback callback);        // コネクションデータ受信
  close_callback netio_conn_set_close_callback(nio_conn conn, close_callback callback);     // コネクションclose
  parse_callback netio_conn_set_parse_callback(nio_conn ncon, parse_callback callback);     // データparse
  recv_check_func netio_conn_set_recv_check_func(nio_conn ncon, recv_check_func checkfunc); // 受信可否チェック

  // Pair connection設定
  void netio_conn_set_pair_connection(nio_conn ncon, nio_conn pair_conn);

  //=======================================================================/
  /* Multicast ***/

  // callback関数type定義
  typedef int (*multicastcallback)(nio_multicast mc, char *data, int len);

  // 初期化
  nio_multicast netio_multicast_init(char *address, unsigned short port);

  // 送信
  int netio_multicast_send(nio_multicast mc, char *data, int len);

  // polling
  void netio_multicast_poll(nio_multicast mc, int timeout);

  // callback設定
  multicastcallback netio_multicast_recv_callback(nio_multicast mc, multicastcallback callback);

  //=======================================================================/
  /* UDP ***/

  // callback関数type定義
  typedef int (*udpcallback)(nio_udp u, struct sockaddr_in dst_addr, char *data, int len);

  nio_udp netio_udp_init(char *address, unsigned short port);

  int netio_udp_send(nio_udp u, struct sockaddr_in dst_addr, char *data, int len);
  int netio_udp_send_by_address(nio_udp u, const char *address, unsigned short port, char *data, int len);

  void netio_udp_poll(nio_udp u, int timeout);

  udpcallback netio_udp_recv_callback(nio_udp u, udpcallback callback);

  //=======================================================================/
  /* RAW ***/

  // callback関数type定義
  typedef int (*rawcallback)(nio_raw raw, char *data, int len);

  // 初期化・解放
  nio_raw netio_raw_init(unsigned short port, rawcallback callback);
  void netio_raw_release(nio_raw raw);

  // 送信
  int netio_raw_sendto(nio_raw raw, struct sockaddr_in *sendto, char *data, int len);

  // poll
  void netio_raw_poll(nio_raw raw, int timeout);

  //=======================================================================/
  /* PARSER ***/

  int netio_parse16(const char *data, int datalen, char *parsed_data, int *parsed_data_len);
  int netio_pack16_length(char *pack_data, int datalen);
  int netio_pack16(const char *data, int datalen, char *pack_data, int max_pack_data);

  int netio_parse32(const char *data, int datalen, char *parsed_data, int *parsed_data_len);
  int netio_pack32_length(char *pack_data, int datalen);
  int netio_pack32(const char *data, int datalen, char *pack_data, int max_pack_data);

  int netio_parse_text(const char *data, int datalen, char *parsed_data, int *parsed_data_len);
  int netio_pack_text(const char *data, int datalen, char *pack_data, int max_pack_data);

  //=======================================================================/

  /* debug用 ***/
  void netio_set_debug(int debug);

#ifdef __cplusplus
}
#endif

#endif /* !defined (__NETIO_H_INCLUDED__) */
