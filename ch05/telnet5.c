/**
 * 5.11.5 マルチスレッドによる並列処理 telnetクライアント多重化
 * pthreadによるマルチスレッド化
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <arpa/telnet.h>
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
 * グローバル変数
 * 
 * 親スレッドと子スレッドの識別用に、スレッドIDをグローバル変数で用意
 */
// ソケット
int g_soc = -1;
// 終了フラグ
volatile sig_atomic_t g_end = 0;
// 親スレッドID
pthread_t g_parent_thread = (pthread_t) -1;
// 子スレッドID
pthread_t g_child_thread = (pthread_t) -1;

/**
 * 送信スレッド
 * 
 * マルチスレッドではfork()でのマルチプロセスのように関数の途中から処理を開始することはできず
 * スレッド関数からの開始になり、1つ関数を追加する必要がある
 */
void * send_thread(void *arg)
{
    char c;
    while (g_end == 0) {
        // stdinから読み込み
        c = getchar();
        // サーバに送信
        if (send(g_soc, &c, 1, 0) == -1) {
            break;
        }
    }

    // 親スレッドにシグナル送信
    (void) pthread_kill(g_parent_thread, SIGTERM);
    pthread_exit((void *) 0);

    // NOT REACHED
    return ((void *) 0);
}

/**
 * サーバにソケット接続
 * ch01のclient_socket()と同様
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
    // アドレス情報の決定
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                                    nbuf, sizeof(nbuf),
                                    sbuf, sizeof(sbuf),
                                    NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "addr=%s\n", nbuf);
    (void) fprintf(stderr, "port=%s\n", sbuf);
    // ソケットの生成
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }
    // コネクト
    if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("connect");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }
    freeaddrinfo(res0);
    return (soc);
}

/**
 * 受信データチェック
 * 
 * telnetプロトコル特有の処理。
 * 受信データがIAC(10進で255)の場合にコマンド開始、のちに2バイトのコマンドデータが続く。
 */
int recv_data(void)
{
    char buf[8];
    char c;

    // 1byte受信
    if (recv(g_soc, &c, 1, 0) <= 0) {
        return (-1);
    }
    // コマンド開始 IAC
    if ((int) (c & 0xFF) == IAC) {
        // コマンド
        // IACの場合はさらに2byte受信
        if (recv(g_soc, &c, 1, 0) == -1) {
            perror("recv");
            return (-1);
        }
        if (recv(g_soc, &c, 1, 0) == -1) {
            perror("recv");
            return (-1);
        }

        // 否定で応答　3byteの否定応答を返す
        (void) sprintf(buf, sizeof(buf), "%c%c%c", IAC, WONT, c);
        if (send(g_soc, buf, 3, 0) == -1) {
            perror("send");
            return (-1);
        }
    } else {
        // 画面へ
        (void) fputc(c & 0xFF, stdout);
    }
    return (0);
}


/**
 * メインループ
 * 受信処理
 * 送信処理がスレッド関数として切り出されたので、ここでは送信スレッドの起動と受信処理を行う
 */
void recv_loop(void)
{
    void *ret;

    // エコーなし RAWモード
    (void) system("stty -echo raw");
    /**
     * バッファリングOFF
     * 
     * stdin stdoutはデフォルトではバッファリングされているのでOFFにする
     */
    (void) setbuf(stdin, NULL);
    (void) setbuf(stdout, NULL);

    /**
     * 送信スレッドを起動
     * pthread_create()で行い、スレッド関数への引数は不要なのでNULLを指定。
     * 第2引数はスレッドパラメータの指定　これもデフォルトで問題がないのでNULL
     * 
     * 子スレッドのIDはg_child_threadグローバル変数で保持
     * 親スレッドIDはpthread_self()で取得して、g_parent_threadに保持
     * 
     * 受信終了時には子スレッドにpthread_kill()でTERMシグナル送信し、子スレッドの終了を
     * pthread_join()で待つ。
     * 
     * 今回は終了時に子スレッドと合流(マルチプロセスでのwait()に相当する処理)を行いたいので
     * 開始関数の先頭デタッチは行わず、最後にpthread_join()で合流する
     */
    if (pthread_create(&g_child_thread, NULL, send_thread, (void *) NULL) != 0) {
        // pthread_create()失敗
        perror("pthread_create");
        return;
    } else {
        // 親スレッドIDセット
        g_parent_thread = pthread_self();
        for (;;) {
            // ソケット受信
            if (recv_data() == -1) {
                break;
            }
        }
        // 子スレッドにシグナル送信
        (void) pthread_kill(g_child_thread, SIGTERM);
        // 子スレッドをジョイン
        (void) pthread_join(g_child_thread, &ret);
    }
}

/**
 * シグナルハンドラ
 * 終了時のシグナル
 */
void sig_term_handler(int sig)
{
    g_end = sig;
}

/**
 * シグナルの設定
 * 終了関連のシグナルに対して、sig_term_handler()をハンドラにセットする
 * 
 * telnet3.cまではsignal()で設定したが、通常Linuxでsignal()を用いてハンドラを設定すると
 * SA_RESTART付きで設定される
 * 
 * SA_RESTARTが設定されていると、プログラムがrecv()やsend()を呼び出してブロッキングしている状態で
 * 親プロセスや子プロセスから終了シグナルを投げても、ブロックが解除されない
 * 
 * SA_RESTARTなしだと、recv()やsend()の最中にシグナルを受けると、そのシステムコールは
 * グローバル変数errnoにEINTRを設定して終了する。
 * そのため、telnet3.cのinit_signal()を流用すると、コネクションが切断されても終了しないプログラムになる
 * 
 * 今回はsigaction()でSA_RESTARTをなしでハンドラを登録させる。
 */
void init_signal(void)
{
    // 終了関連
    struct sigaction sa;

    // SIGINT
    (void) sigaction(SIGINT, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGINT, &sa, (struct sigaction *) NULL);
    // SIGTERM
    (void) sigaction(SIGTERM, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGTERM, &sa, (struct sigaction *) NULL);
    // SIGQUIT
    (void) sigaction(SIGQUIT, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGQUIT, &sa, (struct sigaction *) NULL);
    // SIGHUP
    (void) sigaction(SIGHUP, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGHUP, &sa, (struct sigaction *) NULL);
}

/**
 * main
 * 
 * 終了処理は工夫が必要
 * 子スレッドの場合は単にスレッドが終了するのみでよく、_exit()をするとプログラム全体が終了する
 * 
 * シグナルは親子それぞれで受信するので、終了処理も親子それぞれで実行される。
 * 
 * 1回で良い処理は親スレッドでおこなう
 * 
 * マルチプロセスと違い、ソケットのクローズは1回で良い処理となる。
 * 
 * 親子の判別はpthread_self()で得たスレッドIDとグローバルで保持している親子のスレッドIDを比較
 */
int main(int argc, char *argv[])
{
    char *port;
    if (argc <= 1) {
        (void) fprintf(stderr, "telnet1 hostname [port]\n");
        return (EX_USAGE);
    } else if (argc <= 2) {
        // デフォルトtelnetポートを指定
        port = "telnet";
    } else {
        port = argv[2];
    }

    // ソケット接続
    if ((g_soc = client_socket(argv[1], port)) == -1) {
        return (EX_IOERR);
    }
    // シグナル設定
    init_signal();
    // メインループ
    recv_loop();
    /**
     * プログラム終了
     * ソケットクローズ
     */
    if (pthread_self() == g_parent_thread) {
        // エコーあり、cookedモード 8bit
        (void) system("stty echo cooked -istrip");
        (void) fprintf(stderr, "Connection Closed. \n");
        // ソケットクローズ
        if (g_soc != -1) {
            (void) close(g_soc);
        }
    } else {
        // 子スレッド
        pthread_exit((void *) 0);
    }
    return (EX_OK);
}
