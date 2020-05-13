/**
 * ch01 client.cを変更したもの
 * getaddrinfo()とaddrinfo型構造体を使わないでclientを実装する
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

/**
 * サーバにソケット接続
 * ch01とは違い、getaddrinfo()とaddrinfo型構造体を使わないでclientを実装する
 */
int client_socket(const char *hostnm, const char *portnm)
{
  char buf[MAXHOSTNAMELEN];
  struct sockaddr_in server;
  struct in_addr addr;
  int soc, portno;
  struct hostent *host;
  struct servent *se;

  // アドレス情報をゼロクリア
  (void) memset(&server, 0, sizeof(server));
  // ホスト名がIPアドレスと仮定してホスト情報取得
  if (inet_pton(AF_INET, hostnm, &addr) == 0) {
    /**
     * ホスト情報取得
     * gethostbyname2() 与えられた名前から、下記のいずれかからIPアドレスを調べる。
     *  - DNS問い合わせ
     *  - /etc/hosts
     *  - NIS
     * どれから調べるかは/etc/nsswitch.confの設定に依存する
     * 
     * /etc/hostsではファイル読み込みが発生、NIS DNSでは通信が発生して時間がかかる。
     * なのでclientの処理速度のために、まずはホスト名がIPアドレスと仮定して、ダメであればホスト名と考えるべき
     */
    if ((host = gethostbyname2(hostnm, AF_INET)) == NULL) {
      // ホストが見つからない
      (void) fprintf(stderr, "gethostbyname2():error\n");
      return (-1);
    }
    (void) memcpy(&addr, (struct in_addr *) *host->h_addr_list, sizeof(struct in_addr));
  }
  (void) fprintf(stderr, "addr=%s\n", inet_ntop(AF_INET, &addr, buf, sizeof(buf)));
  // ホストアドレスのセット
  server.sin_addr = addr;
  // ポート番号の決定
  // 先頭が数字
  if (isdigit(portnm[0])) {
    // 数値化するとゼロ以下になる場合
    if ((portno = atoi(portnm)) <= 0) {
      (void) fprintf(stderr, "bad port no \n");
      return (-1);
    }
    server.sin_port = htons(portno);
  } else {
    /**
     * 見つからない場合
     * 
     * getservbyname()は サービス名とプロトコルを指定して、サービス情報データから情報を検索する。
     * 今回getaddrinfo()を使わない場合指定されたポート番号が数値の場合と、サービス名で指定される場合で処理を分岐させる必要がある。
     * 
     * isdigit()の方が簡単・高速で、頻度も多いので先にisdigit()で調べる。
     */
    if ((se = getservbyname(portnm, "tcp")) == NULL) {
      (void) fprintf(stderr, "getservbyname():error\n");
      return (-1);
    } else {
      // サービスに見つかった　該当ポート番号
      server.sin_port = se->s_port;
    }
  }
  (void) fprintf(stderr, "port=%d\n", ntohs(server.sin_port));


  // ソケットの生成
  if ((soc = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return -1;
  }

  // コネクト
  if (connect(soc, (struct sockaddr *) &server, sizeof(server)) == -1) {
    perror("connect");
    (void) close(soc);
    return -1;
  }
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