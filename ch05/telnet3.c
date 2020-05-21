/**
 * 5.11.3 ノンブロッキングI/Oによるクライアントの多重化
 * ノンブロッキングソケットにするためのset_block()を使用
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
#include <fcntl.h> // 追加
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * ブロッキングモードのセット
 * ch04で説明したset_block()を使用する
 * 第1引数にディスクリプタ　第2引数0でノンブロッキングを指定
 * set_block()はソケット以外のディスクリプタに対しても使用できる。
 * 今回はユーザーからの入力が行われるstdin(0)にも使う。 
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
 * 5.11.2では set_block()でノンブロッキングモードにし、入力のreadyを調べずにいきなりソケットから受信
 * stdinから読み込みを行っている。
 * 
 * telnetクライアントは端末設定が「エコーあり、行編集モード」のデフォルト状態では実用にならない
 * そのためsttyコマンドを利用して「エコーなし、RAWモード」にしている。
 */
void send_recv_loop(void)
{
    int data_flag;
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

    /**
     * ノンブロッキングにする
     */
    (void) set_block(0, 0); // stdin(0)もノンブロッキングにする
    (void) set_block(g_soc, 0);

    for (;;) {
        data_flag = 0;
        // ソケットから受信
        if (recv_data() == -1) {
            if (errno != EAGAIN) {
                // Linuxでは切断でEAGAINになり、判別できない
                // 切断・エラー
                break;
            }
        } else {
            data_flag = 1;
        }
        // stdinから読み込み
        c = getchar();
        if (c != EOF) {
            // サーバに送信
            if (send(g_soc, &c, 1, 0) == -1) {
                /**
                 * ノンブロッキングでは1byteも受信データがない場合にもエラーになる
                 * このときerrnoがEAGAINになる
                 * この場合は処理場のエラーとはしないようにする
                 */
                if (errno != EAGAIN) {
                    // 切断・エラー
                    break;
                }
            }
            data_flag = 1;
        }
        if (data_flag == 0) {
            /**
             * stdin、ソケット共に1byteも得られない場合はusleep()で0.01秒スリープさせる。
             * CPU負荷を下げるためスリープ
             * これがないとループが休みなく回り続けることになり、CPUパワーを限界まで消費する。
             * ノンブロッキングI/Oを使う際にはこのような工夫が非常に重要
             */
            (void) usleep(10000);
        }
    }
    // ブロッキングに戻す
    (void) set_block(0, 1); // stdin
    (void) set_block(g_soc, 1);

    // エコーあり cookedモード 8ビット
    (void) system("stty echo cooked -istrip");
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
