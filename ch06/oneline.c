/**
 * 6.5 行単位の受信サーバ
 * 行単位の取得は、fgets()では問題がある
 * 通信など、ストリームの入出力では高水準入出力のバッファリングが邪魔になる
 * windowsへの移植性でも問題がある
 * 
 * 高水準入出力関数群を使わない行単位の受信サーバの実装を行う
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
 * グローバル変数
 * 
 * 固定サイズのバッファを使うモードと、動的にバッファを確保するモードを
 * 選択するための変数
 */
// 固定バッファ:1 動的バッファ:2
int g_mode;

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
 * ソケットから1行受信
 * 
 * 固定バッファに入る範囲で1行受信
 * 1byteをrecv()で受信し、エラーならエラー終了
 * 切断の場合はすでに受信したデータがあれば正常に帰り、
 * 受信データがない場合は切断として返る
 * 
 * 文字列なのでバッファ配列の最後にはヌル文字を入れる必要がある
 * 
 * 1byteずつrecv()で受信するのと、まとめて受信してバッファリングして、そこから1行単位で切り出すのは
 * どちらもほとんど差が出ない
 * recv()はカーネルがすでに受信してバッファに入れたものをコピーしているだけなので
 * syscallを呼ぶオーバーヘッドが増えるだけでパケット自体が細切れになるわけではない
 */
ssize_t recv_one_line_1(int soc, char *buf, size_t bufsize, int flag)
{
    int end;
    ssize_t len, pos, rv;
    char c;
    // 初期化
    buf[0] = '\0';
    pos = 0;
    do {
        end = 0;
        // 1byte受信
        c = '\0';
        if ((len = recv(soc, &c, 1, flag)) == -1) {
            // error
            perror("recv");
            rv = -1;
            end = 1;
        } else if (len == 0) {
            // 切断
            (void) fprintf(stderr, "recv:EOF\n");
            if (pos > 0) {
                // すでに受信データあり 正常終了
                rv = pos;
                end = 1;
            } else {
                // 受信データなし、切断終了
                rv = 0;
                end = 1;
            }
        } else {
            // 正常受信
            buf[pos++] = c;
            if (c == '\n') {
                // 改行:終了
                rv = pos;
                end = 1;
            }
            if (pos == bufsize - 1) {
                // 指定サイズ:終了
                rv = pos;
                end = 1;
            }
        }
    } while (end == 0);
    buf[pos] = '\0';
    return (rv);
}

/**
 * ソケットから1行受信 動的バッファ
 * 
 * バッファを動的に確保しつつ、1行が終わるまで受信し続ける
 * 
 * 処理として
 * 固定バッファで1行受信するrecv_one_line_1()を繰り返し使用し、バッファ領域を動的に
 * 確保・拡大しながら連結することを、末尾が改行になるまで繰り返す。
 * 
 * realloc()は小さな単位で拡大を繰り返すと、ヒープ領域に未使用エリアの断片がたくさんできて
 * 必要以上にメモリを抱え込む問題がある
 * ある程度大きなきりのよい単位で拡大する使い方を最小限行うように注意すれば問題なく、
 * 非常に便利な関数
 * 
 * 1行単位の受信時の注意として、この関数をインターネットからのアクセスを受け付けるメールサーバなどで
 * 使用すると、1行の長さを本当に無制限にするのは危険
 * 改行文字がないデータを送りつけられるとリソースが枯渇する
 * そのためこの関数ではバッファサイズがRECV_ONE_LINE_2_ALLOC_LIMITで定義したサイズ以上になる場合は
 * エラー終了する
 */
ssize_t recv_one_line_2(int soc, char **ret_buf, int flag)
{
#define RECV_ONE_LINE_2_ALLOC_SIZE (1024)
#define RECV_ONE_LINE_2_ALLOC_LIMIT (1024 * 1024)
    char buf[RECV_ONE_LINE_2_ALLOC_SIZE], *data = NULL;
    int end;
    ssize_t size = 0, now_len = 0, len, rv;
    *ret_buf = NULL;
    do {
        end = 0;
        // 1行受信
        if ((len = recv_one_line_1(soc, buf, sizeof(buf), flag)) == -1) {
            // error
            free(data);
            data = NULL;
            rv = -1;
            end = 1;
        } else if (len == 0) {
            // 切断
            if (now_len > 0) {
                // 受信データあり
                rv = now_len;
                end = 1;
            } else {
                // 受信データなし
                rv = 0;
                end = 1;
            }
        } else {
            // 正常受信
            if (now_len + len >= size) {
                // 領域不足
                size += RECV_ONE_LINE_2_ALLOC_SIZE;
                if (size > RECV_ONE_LINE_2_ALLOC_LIMIT) {
                    free(data);
                    data = NULL;
                } else if (data == NULL) {
                    data = malloc(size);
                } else {
                    data = realloc(data, size);
                }
            }
            if (data == NULL) {
                // メモリー確保エラー
                perror("malloc or realloc or limit-over");
                rv = -1;
                end = 1;
            } else {
                // データ格納
                (void) memcpy(&data[now_len], buf, len);
                now_len += len;
                data[now_len] = '\0';
                if (data[now_len - 1] == '\n') {
                    // 末尾が改行
                    rv = now_len;
                    end = 1;
                }
            }
        }
    } while (end == 0);
    *ret_buf = data;
    return (rv);
}

/**
 * デバッグ表示
 */
void debug_print(char *buf)
{
    char *ptr;
    for (ptr = buf; *ptr != '\0'; ptr++) {
        if (isprint(*ptr)) {
            (void) fputc(*ptr, stderr);
        } else {
            (void) fprintf(stderr, "[%02X]", *ptr);
        }
        (void) fputc('\n', stderr);
    }
}

/**
 * 送受信ループ 固定バッファ
 * 
 * 固定バッファサイズはデバッグ用にあえて20byteとする
 */
void send_recv_loop_1(int acc)
{
#define FIXED_BUFFER_SIZE (20)
    char buf[FIXED_BUFFER_SIZE], buf2[512], *ptr;
    ssize_t len;
    (void) fprintf(stderr, "Fized buffer: sizeof(buf)%d\n", (int) sizeof(buf));
    for (;;) {
        // 受信
        if ((len = recv_one_line_1(acc, buf, sizeof(buf), 0)) == -1) {
            // error
            break;
        }
        if (len == 0) {
            // EOF
            break;
        }
        // デバッグ表示
        (void) fprintf(stderr, "[client(%d)]:", (int) len);
        debug_print(buf);
        // 末尾の改行文字をカット
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        // 応答文字列作成
        len = snprintf(buf2, sizeof(buf2), "%s:OK\r\n", buf);
        // 応答
        if ((len = send(acc, buf2, len, 0)) == -1) {
            // error
            perror("send");
            break;
        }
    }
}

/**
 * 送受信ループ:動的バッファ
 * 
 * 動的に確保しながら1行を受信するモード用
 * 
 * 関数内で動的にメモリを確保するので使用後は必ずfree()する
 */
void send_recv_loop_2(int acc)
{
    char *buf, *buf2, *ptr;
    ssize_t len;
    size_t alloc_len;
    for (;;) {
        // 受信
        if ((len = recv_one_line_2(acc, &buf, 0)) == -1) {
            // error
            break;
        }
        if (len == 0) {
            // EOF
            break;
        }
        // デバッグ表示
        (void) fprintf(stderr, "[client(%d)]:", len);
        debug_print(buf);
        // 末尾の改行文字をカット
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        // 応答文字列作成
        alloc_len = strlen(buf) + strlen(":OK\r\n") + 1;
        if ((buf2 = malloc(alloc_len)) == NULL) {
            perror("malloc");
            free(buf);
            break;
        }
        len = snprintf(buf2, alloc_len, "%s:OK\r\n", buf);
        // 応答
        if ((len = send(acc, buf2, len, 0)) == -1) {
            // error
            perror("send");
            free(buf2);
            free(buf);
            break;
        }
        free(buf2);
        free(buf);
    }
}

/**
 * アクセプトループ
 * 
 * バッファの確保の仕方のモードにより、send_recv_loop_1()とsend_recv_loop_2()を使い分ける
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
            (void) getnameinfo((struct sockaddr *) &from, len,
                                        hbuf, sizeof(hbuf),
                                        sbuf, sizeof(sbuf),
                                        NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);
            // 送受信ループ
            if (g_mode == 1) {
                // 固定バッファ
                send_recv_loop_1(acc);
            } else {
                // 動的バッファ
                send_recv_loop_2(acc);
            }
            // アクセプトソケットクローズ
            (void) close(acc);
            acc = 0;
        }
    }
}

/**
 * main
 */
int main(int argc, char *argv[])
{
    int soc;
    if (argc <= 2) {
        (void) fprintf(stderr, "oneline port\n");
        return (EX_USAGE);
    }
    if (argv[2][0] == '1') {
        (void) fprintf(stderr, "fixed buffer mode\n");
        g_mode = 1;
    } else {
        (void) fprintf(stderr, "variable buffer mode\n");
        g_mode = 2;
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
