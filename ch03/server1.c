/**
 * 特定のIPアドレスでbind()するようにしたサーバ
 */

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
 * 
 * 引数でホスト名、アドレスを渡して、そのアドレスをbind()させる
 */
int server_socket_by_hostname(const char *hostnm, const char *portnm) 
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    // アドレス情報のヒントをゼロクリア
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // アドレス情報の決定
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen, nbuf, sizeof(nbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }

    (void) fprintf(stderr, "addr=%s\nport=%s\n", nbuf, sbuf);
    // ソケットの生成
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    // ソケットオプション(再利用フラグ)設定
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    // ソケットにアドレスを指定
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    if (listen(soc, SOMAXCONN) == -1) {
      perror("listen");
      (void) close(soc);
      freeaddrinfo(res0);
      return (-1);
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
  // 引数にIPアドレス、ポート番号が指定されているか
  if (argc <= 2) {
    (void) fprintf(stderr, "server1 address port\n");
    return (EX_USAGE);
  }

  // サーバソケットの準備
  if ((soc = server_socket_by_hostname(argv[1], argv[2])) == -1) {
    (void) fprintf(stderr, "server_socket_by_hostname(%s, %s):error\n", argv[1], argv[2]);
    return (EX_UNAVAILABLE);
  }

  (void) fprintf(stderr, "ready for accept\n");
  // アクセプトループ
  accept_loop(soc);

  (void) fprintf(stderr, "closing the socket\n");
  // ソケットクローズ
  (void) close(soc);
  return (EX_OK);
}