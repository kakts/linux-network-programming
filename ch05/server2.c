/**
 * ch05
 * 5.5.1 select()を使用したマルチクライアント化
 * 
 * ch01のserver.cをベースに拡張をおこなったもの
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
 * 接続受付準備
 * 
 * ソケットの生成から接続待受の段階ではマルチクライアントに関する配慮は必要ない
 * ch01 server.cと同じ関数を利用
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
 * 接続受付
 * 
 * ch01 accept_loop()では1つアクセプトすると、
 * 送受信を行うsend_recv_loop()で1つのクライアントの通信が終わるまで
 * 他のクライアントをアクセプトできなかった。
 * 
 * ここでは下記の2種類のソケットをselect() によって監視し、読み込み可能になったものを処理する
 * - 接続待受用のサーバソケット(soc)
 * - accept()によって得られたクライアントと送受信を行うソケット(acc)
 * 
 * select()利用の流れ
 * - FD_ZERO(): 監視するディスクリプタを登録するマスクをクリア
 * - FD_SET(): 監視するディスクリプタをマスクに登録する
 * - select(): マスクに登録されたディスクリプタがreadyになるのを待つ
 * - FD_ISSET(): どのディスクリプタがレディになったか調べる
 * 
 * select()の引数
 * - 1: ディスクリプタの最大値 + 1
 * - 2: 入力を監視したいディスクリプタを登録したマスク
 * - 3: 書き込み可能を監視したいディスクリプタを登録したマスク
 * - 4: 例外状態を監視したいディスクリプタを登録したマスク
 * - 5: タイムアウトまでの待ち時間
 */

// 最大同時処理数
#define MAX_CHILD (20)

/**
 * アクセプトループ
 */
void accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    int child[MAX_CHILD];
    struct timeval timeout;
    struct sockaddr_storage from;
    int acc, child_no, width, i, count, pos, ret;
    socklen_t len;
    fd_set mask;

    // child配列の初期化
    for (i = 0; i < MAX_CHILD; i++) {
        child[i] = -1;
    }
    child_no = 0;

    for (;;) {
        // select()用マスクの作成
        FD_ZERO(&mask); // ゼロクリア
        FD_SET(soc, &mask); // 監視するディスクリプタをマスクに登録する
        width = soc + 1;
        count = 0;
        for (i = 0; i < child_no; i++) {
            if (child[i] != -1) {
                FD_SET(child[i], &mask);
                if (child[i] + 1 > width) {
                    // select()の第1引数に渡す
                    width = child[i] + 1;
                    count++;
                }
            }
        }
        (void) fprintf(stderr, "<<child count:%d>>\n", count);

        /**
         * select()用タイムアウト値のセット
         */
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        /**
         * select()呼び出し
         * これにより引数にセットしたマスクデータは書き換えられるので、毎回セットする必要がある
         * 
         * タイムアウト時間を短くしすぎると、無限ループに近い状態になる。select()でreadyを監視する意味が薄れるので注意
         */
        switch (select(width, (fd_set *) &mask, NULL, NULL, &timeout)) {
        case -1:
            // エラー
            perror("select");
            break;
        case 0:
            // タイムアウト
            break;
        default:
            (void) fprintf(stderr, "default");
            // readyあり 1以上が返った場合
            if (FD_ISSET(soc, &mask)) {
                // サーバソケットready
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

                    // childの空きを検索
                    pos = -1;
                    for (i = 0; i < child_no; i++) {
                        if (child[i] == -1) {
                            pos = i;
                            break;
                        }
                    }

                    // childの空きがない場合
                    if (pos == -1) {
                        // childにこれ以上格納できない
                        if (child_no + 1 >= MAX_CHILD) {
                            (void) fprintf(stderr, "child is full : cannot accept \n");
                            // クローズ
                            (void) close(acc);
                        } else {
                            child_no++;
                            pos = child_no - 1;
                        }

                    }
                    if (pos != -1) {
                        // childに格納
                        child[pos] = acc;
                    }
                }
            }
            // アクセプトしたソケットがready
            for (i = 0; i < child_no; i++) {
                // childが存在している場合
                if (child[i] != -1) {
                    if (FD_ISSET(child[i], &mask)) {
                        // 送受信

                        // エラーまたは切断
                        if ((ret = send_recv(child[i], i)) == -1) {
                            // クローズする
                            (void) close(child[i]);

                            // childを空きにする
                            child[i] = -1;
                        }
                    }
                }
            }
            break;
        }
    }
}


/**
 * サイズ指定文字列連結
 * 
 */
size_t mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    for (pd = dst, lest = size; *pd != '\n' && lest != 0; pd++, lest--);
    dlen = pd - dst;
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }
    pde = dst + size - 1;
    for (ps = src; &ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    for (; pd <= pde; pd++) {
        *pd = '\0';
    }
    while (*ps++);
    return (dlen + (ps - src - 1));
}

/**
 * 送受信
 * 今回送受信は1回終えるごとにselect()のループに戻る必要がある
 * ch01とは処理が異なる
 * 
 * 単純にrecv()で受診し、応答をsend()で送信したら返るだけの処理
 */
int send_recv(int acc, int child_no)
{
    char buf[512], *ptr;
    ssize_t len;

    // 受信
    if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
        // エラー
        perror("recv");
        return (-1);
    }

    if (len == 0) {
        // EOF
        (void) fprintf(stderr, "[child%d] recv:EOF\n", child_no);
        return (-1);
    }

    // 文字列化・表示
    buf[len] = '\0';
    if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
        *ptr = '\0';
    }
    (void) fprintf(stderr, "[child%d]%s\n", child_no, buf);
    // 応答文字列作成
    (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
    len = strlen(buf);

    // 応答
    if ((len = send(acc, buf, len, 0)) == -1) {
        // エラー
        perror("send");
        return (-1);
    }
    return (0);
}

/**
 * main
 * ch01と同様
 * サーバソケットを作ってアクセプトループに入る
 */
int main(int argc, char *argv[])
{
    int soc;
    // 引数にポート番号が指定されているか?
    if (argc <= 1) {
        (void) fprintf(stderr, "server2 port\n");
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