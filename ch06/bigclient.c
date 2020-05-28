/**
 * 6.4.2 送信プログラム
 * 大きなサイズのデータ送信用クライアント
 * 
 * ch01 client.cをベースに作成
 * 
 * ブロッキング・ノンブロッキング両方のテスト可能
 * 送信サイズは1,000,000バイト
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
 * クライアントもサーバ同様に大きなバッファのため、グローバル変数の配列を使う
 */
// 送信バッファ
char g_buf[1000 * 1000];

/**
 * サーバにソケット接続
 * 
 * ch01 client.cと同様
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
 * 送信処理
 * 
 * 送信を1回行うのみの処理
 * 
 * 送信バッファをクリアし、単純に1回send()を実行し、戻り値をstderrに表示する
 * ノンブロッキングの時send()でEAGAINが発生する場合があるが、サーバ側と違い、perror()でエラー表示するのみ
 * 再送はしない
 * 
 * 後半に再送をするバージョンを取り上げる
 */
void send_one(int soc)
{
    ssize_t len;
    (void) memset(g_buf, 0, sizeof(g_buf));

    // 送信
    if ((len = send(soc, g_buf, sizeof(g_buf), 0)) == -1) {
        // エラー EAGAINでもリトライしない
        perror("send");
    }
    (void) fprintf(stderr, "send:%d\n", (int) len);
}

/**
 * 指定したサイズ分すべて送信する
 * send_oneと切り替えて試してみる
 */
ssize_t send_all(int soc, char *buf, size_t size, int flag)
{
    ssize_t len, lest;
    char *ptr;
    for (ptr = buf, lest = size; lest > 0; ptr += len, lest -= len) {
        if ((len = send(soc, ptr, lest, flag)) == -1) {
            if (errno != EAGAIN) {
                return (-1);
            }
            len = 0;
        } else {
            // デバッグ用
            (void) fprintf(stderr, "send:%d\n", len);
        }
    }
    return (size);
}

/**
 * main関数
 * 
 * 第4引数がnの場合にノンブロッキングモードにする処理を追加する
 * サーバに接続後、set_block()を使って切り替える。
 */
int main(int argc, char *argv[])
{
    int soc;
    // 引数にホスト名・ポートが指定されているか?
    if (argc <= 2) {
        (void) fprintf(stderr, "bigclient server-host port\n");
        return (EX_USAGE);
    }
    // サーバにソケット接続
    if ((soc = client_socket(argv[1], argv[2])) == -1) {
        (void) fprintf(stderr, "client_socket():error\n");
        return (EX_UNAVAILABLE);
    }
    // ブロッキングモードオプションの判定
    if (argc >= 4 && argv[3][0] == 'n') {
        (void) fprintf(stderr, "Nonblocking mode\n");
        // ノンブロッキングモード
        (void) set_block(soc, 0);
    }

    // 送信処理
    send_one(soc);

    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}
