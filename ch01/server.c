#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

void send_recv_loop(int);

/**
 * サーバソケットの準備
 */
int server_socket(const char *portnm)
{
  char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct addrinfo hints, *res0;
  int soc, opt, errcode;
  socklen_t opt_len;

  // アドレス情報のヒントをゼロクリア
  (void) memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // IP
  hints.ai_socktype = SOCK_STREAM; // TCP
  hints.ai_flags = AI_PASSIVE; // サーバソケット

  /**
   * アドレス情報の決定
   * 
   * getaddrinfoはアドレス情報を決定するためのヒントを与えることでsockaddr型構造体を得ることができる。
   * ヒントとなる情報を格納した不完全なaddrinfo構造体を与えると、必要なメンバが全てそろったaddrinfo型構造体を返す。
   * 
   * 第4引数に、決定されたアドレス情報を格納したaddrinfo構造体のポインタを渡す。
   * getaddrinfoを呼んだ時点でアロケートされるため利用後はfreeaddrinfo()で解放する
   */
  if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
    (void) fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(errcode));
    return -1;
  }

  if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen, nbuf, sizeof(nbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
    (void) fprintf(stderr, "getnameinfo(): %s\n", gai_strerror(errcode));
    freeaddrinfo(res0);
    return -1;
  }

  (void) fprintf(stderr, "port=%s\n", sbuf);

  /**
   * ソケットの生成
   * 
   * socket()でソケットディスクリプタを得る
   */
  if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
    perror("socket");
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * ソケットオプション(再利用フラグ)設定
   * 
   * 再利用フラグの設定を行わずにbind()してしまうと、クライアントの通信が中断した場合や、
   * 並列処理でクライアントとの通信が終わってしまった場合などに、同じアドレスとポートの組み合わせでbind()できなくなる
   * 
   * 試す場合はこの処理をコメントアウトして起動して、再起動するとエラーになる
   */
  opt = 1;
  opt_len = sizeof(opt);
  if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
    perror("setsockopt");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * ソケットにアドレスを指定
   * 
   * bind()でソケットにアドレスを指定する
   * sockaddr構造体を使ってアドレスを指定する
   * 
   * res0->ai_addrが sockaddr型　その構造体のサイズであるres0->ai_addrlenを指定する。
   */
  // ソケットにアドレスを指定
  if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
    perror("bind");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * アクセスバックログの指定
   * 
   * listen()を呼び出すことで、このソケットに対するアクセスバックログ(接続待ちのキューの数)を指定する。
   * 
   * SOMAXCONNはシステムでの最大値となる linuxでは128
   * 
   * 小さい数を指定すると、接続要求(SYN)に対して、何も応答しないという現象が起きやすくなる。
   * クライアント側は応答がないのでSYNの再送を繰り返してしまう。
   * 
   * listen()を呼び出すとソケットは接続待ち受け可能な状態になる
   * socket()で作られたばかりのソケットは、待ち受け可能ではないので、listen()せずにaccept()するとエラーになる
   * 
   * listen()されたソケットに対してクライアントからの接続要求があった場合 TCPの3way handshakeが加療する
   */
  if (listen(soc, SOMAXCONN) == -1) {
    perror("listen");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  freeaddrinfo(res0);
  return (soc);
}

/**
 * アクセプトループ
 * 
 * accept()を呼び出すと、待ち受け状態から1つの接続を受け付ける。
 * 1つも待ちがない場合この関数はブロックする
 * 
 * 他の処理と多重化したい場合はselect() poll()などを利用する。
 */
void accept_loop(int soc)
{
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct sockaddr_storage from;
  int acc;
  socklen_t len;

  for (;;) {
    len = (socklen_t) sizeof(from);

    // 接続受付
    if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
      if (errno != EINTR) {
        perror("accept");
      }
    } else {
      (void) getnameinfo((struct sockaddr *) &from, len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
      (void) fprintf(stderr, "accept: %s:%s\n", hbuf, sbuf);

      // 送受信ループ
      send_recv_loop(acc);

      // アクセプトソケットのクローズ
      (void) close(acc);
      acc = 0;
    }
  }
}

/**
 * テスト
 */
size_t mystrlcat(char *dst, const char *src, size_t size)
{
  const char *ps;
  char *pd, *pde;
  size_t dlen, lest;

  for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--);
  dlen = pd - dst;
  if (size - dlen == 0) {
    return (dlen + strlen(src));
  }

  pde = dst + size - 1;
  for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
    *pd = *ps;
  }

  for (; pd <= pde; pd++) {
    *pd = '\0';
  }

  while(*ps++);
  return (dlen + (ps - src - 1));
}
/**
 * 送受信ループ
 * send() 送信
 * recv() 受信
 */
void send_recv_loop(int acc)
{
  char buf[512], *ptr;
  ssize_t len;
  for (;;) {
    // 受信
    if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
      // エラー
      perror("recv");
      break;
    }

    if (len == 0) {
      // end of file
      (void) fprintf(stderr, "recv:EOF\n");
      break;
    }

    // 文字列化・表示
    buf[len] = '\0';
    if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
      *ptr = '\0';
    }
    (void) fprintf(stderr, "[client]%s\n", buf);

    // 応答文字列作成
    (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
    len = (ssize_t) strlen(buf);

    // 応答
    if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
      // エラー
      perror("send");
      break;
    }
  }
}

int main(int argc, char *argv[])
{
  int soc;
  // 引数にポート番号が指定されているか
  if (argc <= 1) {
    (void) fprintf(stderr, "server port\n");
    return (EX_USAGE);
  }

  // サーバソケットの準備
  if ((soc = server_socket(argv[1])) == -1) {
    (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
    return (EX_UNAVAILABLE);
  }

  (void) fprintf(stderr, "ready for accept\n");
  // アクセプトループ
  accept_loop(soc);

  // ソケットクローズ
  (void) close(soc);
  return (EX_OK);
}