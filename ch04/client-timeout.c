/**
 * 4.4 コネクトのタイムアウト
 * タイムアウト処理が追加されたclient
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <fcntl.h> // ソケットモードをデフォルトのブロッキングモードから、ノンブロッキングモードに変更するために追加
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * ブロッキングモードのセット
 * 
 */
int set_block(int fd, int flag)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
        perror("fcntl");
        return (-1);
    }
    /**
     * fcntlを使ってフラグをセットする
     * ディスクリプタを指定してフラグ設定
     * https://linuxjm.osdn.jp/html/LDP_man-pages/man2/fcntl.2.html
     */
    if (flag == 0) {
        // ノンブロッキング
        (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else if (flag == 1) {
        // ブロッキング
        // O_NONBLOCKビットをクリアする
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return (0);
}

/**
 * サーバにソケット接続(タイムアウト可能版)
 * 
 * 関数の引数にタイムアウト秒を指定できるようにする
 * 負の値の場合はタイムアウトなし
 * それ以外はタイムアウトを行う
 */
int client_socket_with_timeout(const char *hostnm, const char *portnm, int timeout_sec)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    struct timeval timeout;
    int soc, errcode, width, val;
    socklen_t len;
    fd_set mask, write_mask, read_mask;

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
    // タイムアウトなし
    if (timeout_sec < 0) {
        if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
            perror("connect");
            (void) close(soc);
            freeaddrinfo(res0);
            return (-1);
        }
        freeaddrinfo(res0);
        return (soc);
    } else {
        // タイムアウトあり
        // ノンブロッキングモードにする
        (void) set_block(soc, 0);

        /**
         * コネクト
         * connect()はエラー時に-1を返す。エラーの種類はerrnoというグローバル変数を参照することでわかる
         */
        if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
            /**
             * 進行中以外はエラーにする
             * EINPROGRESS:接続処理が進行中
             * ソケットがノンブロッキングモードの場合すぐにconnect()が成功しなかった場合になる
             * 
             * EINPROGRESSの場合はエラーとせず処理を継続。「コネクト結果待ち」以降で接続できたかを確認する
             * SolarisではEINTRにあたる
             * 
             * connect()が0の場合はタイムアウト以前に接続完了したことになる
             */
            if (errno != EINPROGRESS) {
                perror("connect");
                (void) close(soc);
                freeaddrinfo(res0);
                return (-1);
            }
        } else {
            // コネクト完了
            (void) set_block(soc, 1);
            freeaddrinfo(res0);
            return (soc);
            /* NOT REACHED */
        }

        // コネクト結果待ち
        width = 0;
        FD_ZERO(&mask);
        FD_SET(soc, &mask);
        width = soc + 1;
        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;
        for (;;) {
            write_mask = mask;
            read_mask = mask;

            /**
             * select()でソケットが状態変化を待つ(読み込み/書き込み可能になるか)
             * 
             * select()がエラーの場合や、タイムアウトした場合は、この関数自体もタイムアウトしたとみなし、ソケットをクローズして-1を返す
             */
            switch (select(width, &read_mask, &write_mask, NULL, &timeout)) {
            case -1:
                if (errno != EINTR) {
                    // selectエラー
                    perror("select");
                    (void) close(soc);
                    freeaddrinfo(res0);
                    return (-1);
                }
                break;
            case 0:
                // selectタイムアウト
                (void) fprintf(stderr, "select:timeout\n");
                (void) close(soc);
                freeaddrinfo(res0);
                return (-1);
                /* NOT REACHED */
                break;
            default:
                /**
                 * 指定時間内にソケットの状態変化が起きた場合
                 * getsockopt()でソケットのエラー状態を調べる
                 */
                if (FD_ISSET(soc, &write_mask) || FD_ISSET(soc, &read_mask)) {
                    len = sizeof(len);

                    /**
                     * getsockopt() ソケットに対するオプションやエラーの状態を取得する
                     * 1: ソケットディスクリプタ
                     * 2: プロトコル層 ソケットに関する情報がほしいのでSOL_SOCKET
                     * 3: 取得したい情報の名前 SO_ERROR
                     * 4: 値の取得先
                     * 5: 値の取得先の長さ
                     */
                    if (getsockopt(soc, SOL_SOCKET, SO_ERROR, &val, &len) != -1) {
                        /**
                         * valが0の場合はエラーなし状態 connect()が無事に成功したと判断する
                         * ソケットをブロッキングモードに戻し、ソケットのディスクリプタを返す
                         */
                        if (val == 0) {
                            (void) set_block(soc, 1);
                            freeaddrinfo(res0);
                            return (soc);
                        } else {
                            /**
                             * 0以外はコネクト失敗
                             */
                            (void) fprintf(stderr, "getsockopt:%d:%s\n", val, strerror(val));
                            (void) close(soc);
                            freeaddrinfo(res0);
                            return (-1);
                        }
                    } else {
                        // getsockoptエラー
                        perror("getsockopt");
                        (void) close(soc);
                        freeaddrinfo(res0);
                        return (-1);
                    }
                }
                break;
            }
        }
    }
}

/**
 * ch01のclient.cのsend_recv_loop同じ
 *
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

/**
 * main関数
 * 起動時の引数からタイムアウト値を取るようにする
 */
int main(int argc, char *argv[])
{
    int soc;

    // 引数にホスト名、ポート番号、タイムアウトが指定されているか
    if (argc <= 3) {
        (void) fprintf(stderr, "client-timeout", "server-host port timeout-sec(-1:no-timeout)\n");
        return (EX_USAGE);
    }
    // サーバにソケット接続
    if ((soc = client_socket_with_timeout(argv[1], argv[2], atoi(argv[3]))) == -1) {
        (void) fprintf(stderr, "client_socket_with_timeout():error\n");
        return (EX_UNAVAILABLE);
    }

    // 送受信処理
    send_recv_loop(soc);
    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}