/**
 * 6.4.1 受信プログラム
 * 大きなサイズのデータ受信が可能なサーバ
 * 
 * ch01 server.cをベースに作成
 * 
 * ブロッキング・ノンブロッキング両方のテスト可能
 * 受信バッファは1,000,000バイト
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
#include <fcntl.h> // add
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * グローバル変数
 * 
 * - ブロッキング・ノンブロッキングのモードを区別する変数
 * - 受信バッファ
 * 受信バッファはサイズが大きいため、ローカル変数にするとスタックを消費して、思わぬエラーでプログラムが
 * 強制終了する。
 */

// ノンブロッキング:'n'
char g_mode = 'b';
// 受信バッファ
char g_buf[1000 * 1000];

/**
 * サーバソケットの準備
 * ch01 server.cとほぼ同様
 * 
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
 * 受信ループ
 * ch01 server.cでは送受信ループだったが
 * 今回は受信のみ
 * 
 * 起動時にノンブロッキングモードが指定された場合はset_block()で変更し、recv()で受信
 * ノンブロッキングの場合はrecv()でEAGAINが発生することがあり、この場合はリトライが必要
 */
void recv_loop(int acc)
{
    ssize_t total, len;
    if (g_mode == 'n') {
        // ノンブロッキングモード
        (void) set_block(acc, 0);
    }
    for (total = 0; ;) {
        // 受信
        if ((len = recv(acc, g_buf, sizeof(g_buf), 0)) == -1) {
            // error
            if (errno == EAGAIN) {
                (void) fprintf(stderr, ".");
                continue;
            } else {
                perror("recv");
                break;
            }
        }
        if (len == 0) {
            // EOF
            (void) fprintf(stderr, "recv:EOF\n");
            break;
        }
        (void) fprintf(stderr, "recv:%d\n", (int) len);
        total += len;
    }
    (void) fprintf(stderr, "total:%d\n", (int) total);
}

/**
 * アクセプトループ
 * 
 * accept()を呼び出すと、待ち受け状態から1つの接続を受け付ける。
 * 1つも待ちがない場合この関数はブロックする
 * 
 * 他の処理と多重化したい場合はselect() poll()などを利用する。
 * 
 * 今回は受信だけを行うサーバなので accept_loop()内から
 * send_recv_loop()の代わりにrecv_loop()を呼ぶ
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

      // 受信ループ
      recv_loop(acc);

      // アクセプトソケットのクローズ
      (void) close(acc);
      acc = 0;
    }
  }
}

/**
 * ブロッキングモードのセット
 */
int set_block(int fd, int flag)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
        perror("fcntl");
        return (-1);
    }
    if (flag == 0) {
        // ノンブロッキング
        (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else if (flag == 1) {
        // ブロッキング
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return (0);
}

/**
 * main
 * 
 * 第3引数に'n'を指定した場合はノンブロッキングモード
 */
int main(int argc, char *argv[])
{
    int soc;
    // 引数にポートが指定されているか
    if (argc <= 1) {
        (void) fprintf(stderr, "bigserver port \n");
        return (EX_USAGE);
    }
    // ブロッキングモードオプションの判定
    if (argc >= 3 && argv[2][0] == 'n') {
        (void) fprintf(stderr, "Nonblocking mode\n");
        g_mode = 'n';
    } else {
        g_mode = 'b';
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