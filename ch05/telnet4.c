/**
 * 5.11.4 マルチプロセスによる並列処理 telnetクライアント多重化
 * ノンブロッキングソケットにするためのset_block()を使用
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <arpa/telnet.h>
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
 * シグナル割り込み時に子プロセスかどうかを判断するための、グローバル変数を追加
 */
// ソケット
int g_soc = -1;
// 終了フラグ
volatile sig_atomic_t g_end = 0;
// 子プロセス:1
int g_is_child = 0;

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
 * 送受信処理
 * ソケットからの受信とstdinからの読み込みを並列にすれば良いので、片方だけ子プロセスにすれば十分です。
 * ここではソケットからの受信エラーで終了処理に入るケースがほとんどなので、ソケットからの受信は親プロセスで
 * stdinからの読み込みを子プロセスにした
 */
int send_recv_loop(void)
{
    pid_t pid;
    char c;

    // エコーなし RAWモード
    (void) system("stty -echo raw");
    /**
     * バッファリングOFF
     * 
     * stdin stdoutはデフォルトではバッファリングされているのでOFFにする
     */
    (void) setbuf(stdin, NULL);
    (void) setbuf(stdout, NULL);

    if ((pid = fork()) == 0) {
        // 子プロセス
        g_is_child = 1;
        while (g_end == 0) {
            // stdinから読み込み
            c = getchar();
            // サーバに送信
            if (send(g_soc, &c, 1, 0) == -1) {
                perror("send");
                break;
            }
        }

        /**
         * この2つの処理が必要なのはなぜか
         * 1: 親プロセスにシグナル送信
         * 2: 子プロセス終了
         * 
         * 子プロセスで処理終了となった場合は親プロセスに知らせるために
         * TERMシグナルをkill()で送信する
         * 親プロセスのプロセスIDはgetpid()で取得できる。
         */
        // 親プロセスにシグナル送信
        (void) kill(getpid(), SIGTERM);
        // 子プロセス終了
        _exit(0);
    } else if (pid > 0) {
        // 親プロセス
        while (g_end == 0) {
            // ソケット受信
            if (recv_data() == -1) {
                break;
            }
        }

        /**
         * 親プロセスで処理終了となる場合は子プロセスにTERMシグナルを送信して
         * wait()で終了を待つ。
         */
        // 子プロセスにシグナル送信
        (void) kill(pid, SIGTERM);
        // 子プロセスの終了をwait
        (void) wait(NULL);
    } else {
        // fork()失敗
        perror("fork");
        return (-1);
    }
    return (0);
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
 * マルチプロセスクライアントでは終了処理に注意が必要
 * 
 * シグナルは親子それぞれで受信するので、終了処理も親子それぞれで実行される。
 * 
 * 親子それぞれで重複する処理をしないようにg_is_childで親子の判断を行い、1回で良い処理は親が行うようにする
 * ソケットのクローズは別プロセスなので、それぞれがクローズする
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
    send_recv_loop();
    /**
     * プログラム終了
     * ソケットクローズ
     */
    if (g_soc != -1) {
        (void) close(g_soc);
    }
    if (!g_is_child) {
        // エコーあり、cookedモード 8bit
        (void) system("stty echo cooked -istrip");
        (void) fprintf(stderr, "Connection Closed. \n");
    }
    return (EX_OK);
}
