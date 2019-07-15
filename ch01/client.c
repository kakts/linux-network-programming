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

/**
 * サーバにソケット接続
 */
int client_socket(const char *hostnm, const char *portnm)
{
  char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct addrinfo hints, *res0;
  int soc, errcode;

  // アドレス情報のヒントをゼロクリア
  (void) memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  /**
   * アドレス情報の決定
   * 第1引数はサーバではNULLだったが、クライアントでは明示的にIPやホスト名を指定する必要がある
   */
  if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
    (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
    return -1;
  }

  if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen, nbuf, sizeof(nbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
    (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
    freeaddrinfo(res0);
    return -1;
  }

  (void) fprintf(stderr, "addr=%s\n", nbuf);
  (void) fprintf(stderr, "port=%s\n", sbuf);

  // ソケットの生成
  /**
   * サーバ側では、socket()の後にbind()でアドレスのソケットにアドレスを指定するが、
   * クライアントでは主に自分のアドレス、ポートは明示的に固定しない場合が多い
   * このサンプルでもbind()は呼び出していない
   * 
   * 明示しない場合は　接続に使用できるip, テンポラリなポートが自動的に割り当てられる。
   * こうするとポート番号が重複しないので、1つのホストで複数のクライアントを同時に使用できる。
   * 
   */
  if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
    perror("socket");
    freeaddrinfo(res0);
    return -1;
  }

  // コネクト
  if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
    perror("connect");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  freeaddrinfo(res0);

  // サーバへの送受信が可能になったソケットを返す
  return soc;
}

/**
 * 送受信処理
 * サーバと同様にsend() recv()で送受信できる
 * 
 * 標準入力から1行読み込み、サーバに送信し、サーバからのデータが受信可能であれば受信して、標準出力に出力する
 */
void send_recv_loop(int soc)
{
  char buf[512];
  struct timeval timeout;
  int end, width;
  ssize_t len;
  fd_set mask, ready;

  /**
   * select()するためにははじめに読み込み可能かどうかを調べたいディスクリプタをセットしたマスクデータを作る
   * FD_ZERO()マクロでクリアし、FD_SET()マクロでチェック対象に加えたいディスクリプタをセット
   */

  // select()用マスク
  FD_ZERO(&mask);
  
  // ソケットディスクリプタをセット
  FD_SET(soc, &mask);

  // 標準入力をセット
  FD_SET(0, &mask);
  width = soc + 1;

  /**
   * 送受信
   */
  for (end = 0;;) {
    // マスクの代入
    // select()はマスクを書き換えるので毎回変数にコピーしてから渡す
    ready = mask;

    // タイムアウト値のセット
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    /**
     * 標準入力からの入力とサーバからの受信は交互に起きるという過程はせず
     * select()を用いて標準入力とソケットが読み込み可能(readyと表現することもある)かをチェックしている
     * 
     * このような方法を入力の多重化という。
     * recv(),read()はディスクリプタがデフォルトのブロッキングモードの場合に、読み取れるデータが全くないとブロックしてしまい、
     * 他の処理ができなくなる。
     * 
     * 適当な待ちの処理をいれないとCPUを無駄に消費するが、この処理を入れるのが難しいため
     * 安全な多重化を行うためにselect()を使用するのが一番　多重化に関してはchapter05
     * 
     * select()するためには事前にFD_SET()で加えたいディスクリプタをセットしておく
     */
    switch (select(width, (fd_set *) &ready, NULL, NULL, &timeout)) {
    case -1:
      // たいていは禁止していないシグナルを受けた場合や引数が異常な場合
      // error
      perror("select");
      break;
    case 0:
      // timeout
      break;
    default:
      // ready有り
      // socket ready
      if (FD_ISSET(soc, &ready)) {
        // 受信
        if ((len = recv(soc, buf, sizeof(buf), 0)) == -1) {
          // error
          perror("recv");
          end = 1;
          break;
        }
        if (len == 0) {
          // end of file
          (void) fprintf(stderr, "recv:EOF\n");
          end = 1;
          break;
        }

        // 文字列化・表示
        buf[len] = '\0';
        (void) printf("> %s", buf);
      }

      // 標準入力ready
      if (FD_ISSET(0, &ready)) {
        // 標準入力から1行読み込み
        (void) fgets(buf, sizeof(buf), stdin);
        if (feof(stdin)) {
          end = 1;
          break;
        }

        // 送信
        if ((len = send(soc, buf, strlen(buf), 0)) == -1) {
          // error
          perror("send");
          end = 1;
          break;
        }
      }
      break;
    }
    if (end) {
      break;
    }
  }
}

int main(int argc, char *argv[])
{
  int soc;
  // 引数チェック　ホスト名・ポート名が指定されているか
  if (argc <= 2) {
    (void) fprintf(stderr, "client server-host port\n");
    return (EX_USAGE);
  }

  // サーバーにソケット接続
  if ((soc = client_socket(argv[1], argv[2])) == -1) {
    (void) fprintf(stderr, "client_socket():error\n");
    return (EX_UNAVAILABLE);
  }

  // 送受信処理
  send_recv_loop(soc);

  // ソケットクローズ
  (void) close(soc);

  return (EX_OK);
}