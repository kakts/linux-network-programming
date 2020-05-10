#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>

// クローズする最大ディスクリプタ値
#define MAXFD 64

// デーモン化
/**
 * 2度のfork()と、セッションリーダー化、HUPシグナルの無視で、デーモン状態になり、引数で指定された
 * オプションに応じ、ルートディレクトリに移動、ファイルディスクリプタのクローズを行う。
 * 
 * ファイルディスクリプタはそのまま閉じるとstdin stderrへの出力があるとエラーになる
 * なので/dev/nullにリダイレクトする
 * 
 * リダイレクトはdup2()を使う。
 * dup2(fd, 1)は、ディスクリプタの1(stdout)をクローズし、fdが指している/dev/nullへのディスクリプタを
 * ディスクリプタの1がさすようにする
 * 
 * これ以降、ディスクリプタの1に書き込みを行うと、/dev/nullへの書き込みになる。
 * 念の為2(stdin, stdout, stderrではないこと)より大きいことを確認してclose()
 * 
 * exit()でなく_exit()を利用する理由
 * exit()はatexit()やon_exit()によって登録された関数があるとそれを実行してしまい、
 * 予期せぬ不具合をおこすかもしれない。
 */
int daemonize(int nochdir, int noclose)
{
    int i, fd;
    pid_t pid;
    if ((pid = fork()) == -1) {
        return (-1);
    } else if (pid != 0) {
        // 親プロセスの終了
        _exit(0);
    }

    /**
     * 最初の子プロセス
     * セッションリーダーに
     */
    (void) setsid();

    // HUPシグナルを無視するように瀬テチ
    (void) signal(SIGHUP, SIG_IGN);
    if ((pid = fork()) != 0) {
        // 最初の子プロセスの終了
        _exit(0);
    }
    // デーモンプロセス
    if (nochdir == 0) {
        // ルートディレクトリに移動
        (void) chdir("/");
    }
    if (noclose == 0) {
        // 全てのファイルディスクリプタのクローズ
        for (i = 0; i < MAXFD; i++) {
            (void) close(i);
        }

        // stdin, stdout, stderrを /dev/nullでオープン
        if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
            (void) dup2(fd, 0);
            (void) dup2(fd, 1);
            (void) dup2(fd, 2);
            if (fd > 2) {
                (void) close(fd);
            }
        }
    }
    return (0);
}

/** 
 * テスト用main関数
 * 
 * UNIT_TESTが定義されている場合のみ有効
 */
#ifdef UNIT_TEST
#include <syslog.h>
int main(int argc, char *argv[])
{
    char buf[256];
    //　デーモン化
    (void) daemonize(0, 0);

    (void) fprintf(stderr, "stderr\n");

    syslog(LOG_USER|LOG_NOTICE, "daemon:cwd=%s\n", getcwd(buf, sizeof(buf)));
    return (EX_OK);
}
#endif
