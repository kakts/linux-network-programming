/**
 * 5.11.1 クライアントの多重化 select()による多重化
 * 
 * サーバだけでなく、クライアントでも多重化が必要。
 * 例えばサーバからの受信やキーボードからの入力など
 * 
 * ch01のclient.cではselect()を使って多重化したが、サーバと同じく様々な方法がある。
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <arpa/telnet.h> // 追加
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
 * 非同期シグナル処理に関連したグローバル変数を使用
 */
// ソケット
int g_soc = -1;

// 終了フラグ
volatile sig_atomic_t g_end = 0;

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
 * 
 * telnetクライアントは端末設定が「エコーあり、行編集モード」のデフォルト状態では実用にならない
 * そのためsttyコマンドを利用して「エコーなし、RAWモード」にしている。
 */
void send_recv_loop(void)
{
    struct timeval timeout;
    int width;
    fd_set mask, ready;
    char c;

    // エコーなし RAWモード
    (void) system("stty -cho raw");
    /**
     * バッファリングOFF
     * 
     * stdin stdoutはデフォルトではバッファリングされているのでOFFにする
     */
    (void) setbuf(stdin, NULL);
    (void) setbuf(stdout, NULL);

    // マスク作成
    FD_ZERO(&mask);
    FD_SET(0, &mask);
    FD_SET(g_soc, &mask);
    width = g_soc + 1;
    for (;;) {
        // マスクの代入
        ready = mask;
        // タイムアウト値のセット
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        switch (select(width, &ready, NULL, NULL, &timeout)) {
        case -1:
            if (errno != EINTR) {
                perror("select");
                g_end = 1;
            }
            break;
        case 0:
            // timeout
            break;
        default:
            // readyあり
            if (FD_ISSET(g_soc, &ready)) {
                // ソケット受信ready
                if (recv_data() == -1) {
                    g_end = 1;
                    break;
                }
            }
            // stdin ready
            if (FD_ISSET(0, &ready)) {
                /**
                 * getcharで1文字読み込み、ソケットにsend()で送信
                 * 端末設定がデフォルトの状態では1文字入力してもgetchar()ですぐに得ることができない
                 * RAWモードだとすぐにgetchar()で取得できる。
                 */
                c = getchar();
                // サーバに送信
                if (send(g_soc, &c, 1, 0) == -1) {
                    perror("send");
                    g_end = 1;
                    break;
                }
            }
            break;
        }
        if (g_end) {
            // for break;
            break;
        }
    }
    /**
     * エコーあり cookedモード 8ビット
     * 関数開始時にsystem()関数から呼び出したsttyコマンドで端末モードを変更したので、通常モードに戻してから終了
     */
    (void) system("Stty echo cooked -istrip");
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
 */
void init_signal(void)
{
    // 終了関連
    (void) signal(SIGINT, sig_term_handler);
    (void) signal(SIGTERM, sig_term_handler);
    (void) signal(SIGQUIT, sig_term_handler);
    (void) signal(SIGHUP, sig_term_handler);
}

/**
 * main
 * 
 * 一般的なtelnetと同様に引数でホストとポートを指定する。
 * ポートの指定がない場合はデフォルトtelnetポート
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

    // プログラム終了
    // ソケットクローズ
    if (g_soc != -1) {
        (void) close(g_soc);
    }
    (void) fprintf(stderr, "Connection Closed.\n");
    return (EX_OK);
}
