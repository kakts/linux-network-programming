/**
 * ch05
 * 5.6.2 マルチスレッドによる多重化 マルチクライアント化
 * 
 * pthreadを使ってマルチスレッドの機能を使う
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
#include <pthread.h> // 追加
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
 * 前のコードとはかなり異なる。
 * マルチプロセスではfork()した時点で並列処理が開始されるので、関数の途中で枝分かれして並列処理が開始されていた
 * 
 * マルチスレッドではpthread_create()という関数に明示的にスレッドとして生成したい関数を指定する。
 * その指定された関数から別スレッドになる
 * 
 * ここではsend_recv_loop()をスレッド開始関数とする　今回は名前を変えてsend_recv_thread()という関数にする
 */


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
 * 
 * 送受信スレッド
 * 
 * 送受信自体はこれまでと同じだが、pthread_create()に渡すために、関数のシグニチャ(型)が変わっている。
 */
void * send_recv_thread(void *arg)
{
    char buf[512], *ptr;
    ssize_t len;
    int acc;

    /**
     * スレッドのデタッチ
     * 
     * 関数の先頭で行う。
     * これを行うか、pthread_create()の第2引数でPTHREAD_CREATE_DETACHEDを指定しておかないと
     * スレッドが終了してもpthread_join()で終了状態を得るまでリソースが解放されない。
     * マルチプロセスのfork() wait()のようなもの
     */
    (void) pthread_detach(pthread_self());
    // 引数の取得
    acc = (int) arg;

    for (;;) {
        /** 受信 */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            /* エラー */
            perror("recv");
            break;
        }
        if (len == 0) {
            /* EOF */
            (void) fprintf(stderr, "<%d>recv:EOF\n", (int) pthread_self());
            break;
        }

        /* 文字列化・表示 */
        buf[len] = '\0';
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        (void) fprintf(stderr, "<%d>[client]%s\n", (int) pthread_self(), buf);
        /* 応答文字列作成 */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);
        /* 応答 */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            /* エラー */
            perror("send");
            break;
        }
    }
    /* アクセプトソケットのクローズ
     * マルチスレッドの場合はあくまでも1プロセスの中の処理で、クローズやメモリの解放忘れはプロセスが終了するまで解放されない。
     */
    (void) close(acc);
    pthread_exit((void *) 0);
    /* NOT REACHED */
    return ((void *) 0);
}

/**
 * アクセプトループ
 */
void accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc;
    socklen_t len;
    pthread_t thread_id;
    void *send_recv_thread(void *arg);

    for (;;) {
        len = (socklen_t) sizeof(from);
        // 接続受付
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            (void) getnameinfo((struct sockaddr *) &from, &len,
                                        hbuf, sizeof(hbuf),
                                        sbuf, sizeof(sbuf),
                                        NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /**
             * スレッド生成
             * 
             * pthread_createでsend_recv_threadを開始関数としてスレッド生成する。
             * pthread_createでは1つだけスレッド開始関数に引数を渡せる。今回はアクセプトソケットを渡すようにしている。
             * 本来はグローバル変数を確保して、それのアドレスを渡さないといけない。その場合はロックを行わないと以上動作する可能性がある。
             * そのため値渡しにしている
             * 
             * pthread_create()の引数
             * 1: スレッド属性 NULLを指定するとデフォルト属性(合流可能・デフォルトスケジューリング)
             * 
             * マルチスレッドの場合スレッド生成後に子スレッドはこの関数には2度と戻れない
             */
            if (pthread_create(&thread_id, NULL, send_recv_thread, (void *) acc) != 0) {
                perror("pthread_create");
            } else {
                (void) fprintf(stderr, "pthread_create:create:thread_id=%d\n", (int) thread_id);
            }
        }
    }
}


/**
 * main
 * 
 * ch01 server.cとほぼ同じ
 */
int main(int argc, char *argv[])
{
    int soc;

    // 引数にポート番号が指定されているか?
    if (argc <= 1) {
        (void) fprintf(stderr, "server5 port\n");
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

