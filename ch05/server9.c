/**
 * ch05
 * 5.8 多重化の組み合わせ
 * 
 * select() poll() EPOLLを用いたマルチクライアント化では、ディスクリプタがreadyになった後、
 * accept()やrecv()、send()が同じ流れで処理されるため、もしsend()に時間がかかる場合、
 * 次のaccept()が遅れることになる。
 * 
 * マルチプロセスやマルチスレッドでこの問題を解決できるが、多数のクライアントを処理するために大量のプロセス・スレッドを起動することになり、
 * リソースを消費する。
 * 
 * ここでは accept()やrecv()はselect() poll() EPOLLで多重化することによりマルチクライアント化し、send()は専用のスレッドに任せる方法を採用する
 * このような専用のスレッドをワーカースレッドと呼ぶ。
 * 
 * EPOLLを用いたserver4.cをベースにsend()を専用のワーカースレッドに任せるサンプルを実装する。
 * 送受信が別れると当然スレッド間のデータの受け渡しが必要となる。
 */
// sys/epoll.h macでは使えないので除外
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h> // ワーカースレッドのために必要
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * プリプロセッサ定義・グローバル変数
 * 
 * 1つのスレッドでアクセプト・受診を行い、送信は他のスレッドにデータを渡して行うことになる。
 * 今までよりも複雑になる。
 * ここでは5.8.2で説明したキュー(リングバッファ)を配列として定義する
 */
#define MAXQUEUESZ 4096
#define MAXSENDER 2
#define QUEUE_NEXT(i_) (((i_) + 1) % MAXQUEUESZ)

struct queue_data {
    int acc;
    char buf[512];
    ssize_t len;
};

/**
 * リングバッファキュー
 */
struct queue {
    int front;
    int last;
    struct queue_data data[MAXQUEUESZ];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct queue g_queue[MAXSENDER];

/**
 * 接続受付
 * server4.cと同じ
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
 * server4.cと違い、send_recv()を自分のスレッド内では呼ばず、代わりにキューに受診したデータとディスクリプタをエンキューするようにする
 * 
 * 
 * server3.cのpoll()を使用した例と似ているが、準備と監視方法が違う
 * EPOLLを利用する際の流れ
 * - epoll_create()でEPOLLを使うためのディスクリプタを得る
 * - epoll_event型構造体のfd, eventsメンバに開始対象をセットし、epoll_ctl()で
 * 　EPOLLのディスクリプタにセットする
 * - epoll_wait()でセットされたディスクリプタがreadyになるのを待つ
 * - epoll_wait()の引数に指定したepoll_event型構造体のfdメンバのディスクリプタに処理を行う
 * - epoll_ctl()で監視が不要になったディスクリプタを削除する
 *  
 * 注意点
 * epoll_event型構造体の取り扱い
 * この構造体は監視対象のセットにも使ったり、readyになったディスクリプタを得るためにも使う。
 * 
 * 同じ構造体変数を使いまわすと混乱するので下記の2つの変数を使う
 * ev: セット用
 * events: 読み取り用
 */

// 最大同時処理数
#define MAX_CHILD (20)

/**
 * アクセプトループ
 */
void accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc, count, i, qi, epollfd, nfds;
    socklen_t lfen;
    struct epoll_event ev, events[MAX_CHILD + 1];

    // epoll_create()でEPOLLを使うためのディスクリプタを得る
    if ((epollfd = epoll_create(1)) == -1) {
        perror("epoll_create");
        return;
    }

    /**
     * EPOLL用データの作成
     * epoll_event型構造体のfd, eventsメンバに開始対象をセットし、epoll_cli()で
     * 　EPOLLのディスクリプタにセットする
     * 
     * EPOLLの監視情報ではエッジトリガ動作を指定可能(EPOLLET)
     * エッジトリガ: ディスクリプタの状態が変化した瞬間に通知される動作。
     * レベルトリガ: 変化している最中は常に通知される動作
     */
    ev.data.fd = soc;
    ev.events = EPOLLIN; // データの読み出しready
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, soc, &ev) == -1) {
        perror("epoll_ctl");
        (void) close(epollfd);
        return;
    }
    count = 0;
    for (;;) {
        (void) fprintf(stderr, "<<child count: %d>>\n", count);
        // epoll_wait()でセットされたディスクリプタがreadyになるのを待つ
        switch ((nfds = epoll_wait(epollfd, events, MAX_CHILD + 1, 10 * 1000))) {
        case -1:
            // エラー
            perror("epoll_wait");
            break;
        case 0:
            // タイムアウト
            break;
        default:
            // ソケットがready
            /**
             * poll()との違いは、epoll_wait()から戻った後、ループする回数が本当に処理するべきディスクリプタ数(nfds)のみ
             */
            for (i = 0; i < nfds; i++) {
                // ソケットがreadyになっている
                if (events[i].data.fd == soc) {
                    flen = (socklen_t) sizeof(from);

                    // 接続受付
                    if ((acc = accept(soc, (struct sockaddr *)&from, &flen)) == -1) {
                        if (errno != EINTR) {
                            perror("accept");
                        }
                    } else {
                        (void) getnameinfo((struct sockaddr *) &from, flen,
                                                    hbuf, sizeof(hbuf),
                                                    sbuf, sizeof(sbuf),
                                                    NI_NUMERICHOST | NI_NUMERICSERV);
                        (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);
                        
                        // 空きが無い
                        if (coutn + 1 >= MAX_CHILD) {
                            // これ以上接続できない
                            (void) fprintf(stderr, "connection is full : cannot accept\n");
                            // クローズ
                            (void) close(acc);
                        } else {
                            ev.data.fd = acc;
                            ev.events = EPOLLIN;
                            if (epoll_ctl(epollfd, EPOLL_CTL_ADD, acc, &ev) == -1) {
                                perror("epoll_ctl");
                                return;
                            }
                            count++;
                        }
                    }
                } else {
                    // リングバッファキュー用のインデックス計算
                    qi = events[i].data.fd % MAXSENDER;
                    
                    // キューの末尾にデータ設定
                    g_queue[qi].data[g_queue[qi].last].acc = events[i].data.fd;
                    g_queue[qi].data[g_queue[qi].last].len
                        = recv(g_queue[qi].data[g_queue[qi].last].acc,
                               g_queue[qi].data[g_queue[qi].last].buf,
                               sizeof(g_queue[qi].data[g_queue[qi].last].buf),
                               0);
                    // 受信
                    switch (g_queue[qi].data[g_queue[qi].last].len) {
                    case -1:
                        // エラー
                        perror("recv");
                        break; // 本だとここのbreakがなかった
                    case 0:
                        // EOF
                        (void) fprintf(stderr, "[child%d]recv:EOF\n", events[i].data.fd);
                        // エラーまたは切断
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, &ev) == -1) {
                            perror("epoll_ctl");
                            return;
                        }
                        // クローズ
                        (void) close(events[i].data.fd);
                        count--;
                        break;
                    default:
                        // キューを次に進める。
                        (void) pthread_mutex_lock(&g_queue[qi].mutex);
                        g_queue[qi].last = QUEUE_NEXT(g_queue[qi].last);
                        (void) pthread_cond_signal(&g_queue[qi].cond); // 通知
                        (void) pthread_mutex_unlock(&g_queue[qi].mutex);
                        break;
                    }
                }
            }
            break;
        }
    }
    (void) close(epollfd);
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
 * server4.cと違い、送信はスレッドとなるため、send_recv()はsend_thread()というスレッド開始スレッドにする。
 * 
 * キューの説明通り、デキューをしてそのデータを送信するような関数とする。
 */
// 送信スレッド
void send_thread(void *arg)
{
    char *ptr;
    ssize_t len;
    int i, qi;
    qi = (int) arg; // 引数からリングバッファキューのインデックスを取得

    for (;;) {
        (void) pthread_mutex_lock(&g_queue[qi].mutex);
        // リングバッファキューが満杯でない場合
        if (g_queue[qi].last != g_queue[qi].front) {
            i = g_queue[qi].front;
            g_queue[qi].front = QUEUE_NEXT(g_queue[qi].front); // frontのインデックスを進める。
            (void) pthread_mutex_unlock(&g_queue[qi].mutex);
        } else {
            // リングバッファキューが満杯の場合
            (void) pthread_cond_wait(&g_queue[qi].cond, &g_queue[qi].mutex);
            (void) pthread_mutex_unlock(&g_queue[qi].mutex);
            continue;
        }

        // 文字列化・表示
        g_queue[qi].data[i].buf[g_queue[qi].data[i].len] = '\0';
        if ((ptr = strpbrk(g_queue[qi].data[i].buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "[child%d]%s\n", g_queue[qi].data[i].acc, g_queue[qi].data[i].buf);

        // 応答文字列作成
        (void) mystrlcat(g_queue[qi].data[i].buf, ":OK\r\n", sizeof(g_queue[qi].data[i].buf));
        g_queue[qi].data[i].len = strlen(g_queue[qi].data[i].buf);

        // 応答
        if ((len = send(g_queue[qi].data[i].acc, g_queue[qi].data[i].buf, g_queue[qi].data[i].len, 0)) == -1) {
            // エラー
            perror("send");
        }
    }
    pthread_exit((void *) 0);
    // NOT REACHED
    return ((void *) 0);
}


/**
 * main
 * ch01と同様
 * サーバソケットを作ってアクセプトループに入る
 */
int main(int argc, char *argv[])
{
    int soc, i;
    pthread_t id;
    // 引数にポート番号が指定されているか?
    if (argc <= 1) {
        (void) fprintf(stderr, "server9 port\n");
        return (EX_USAGE);
    }

    for (i = 0; i < MAXSENDER; i++) {
        // mutex初期化
        (void) pthread_mutex_init(&g_queue[i].mutex, NULL);
        (void) pthread_cond_init(&g_queue[i].cond, NULL);
        // 送信用ワーカースレッドの作成
        (void) pthread_create(&id, NULL, (void *) send_thread, (void *) i);
    }

    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }
    (void) fprintf(stderr, "ready for accept\n");
    // アクセプトループ
    accept_loop(soc);
    pthread_join(id, NULL);

    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}
