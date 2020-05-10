/**
 * p64 
 * ハングアップシグナル(SIGHUP)をキャッチした際にexecv3()で自分自身を再起動させることで
 * パラメータ設定を新たに読み込んだ処理を起動できる。
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// グローバル変数
// コマンドライン引数　環境変数のアドレス保持用
int *argc_;
char ***argv_;
char ***envp_;

/**
 * SIGHUPのシグナルハンドラ
 * execve()で自分自身を上書き再起動する
 * 
 * execve()
 * 第1引数: 実行ファイルのパス
 * 第2引数: コマンドライン引数
 * 第3引数: 環境変数
 * 
 * execve()に成功すると、元々のプロセスの領域が上書きされ、同じプロセスIDで新たなプログラムとしての動作が解すする
 * 
 * クローズされていないディスクリプタも継承する
 */
void sig_hangup_handler(int sig)
{
    (void) fprintf(stderr, "sig_hangup_handler(%d)\n", sig);
    //　自プロセスの上書き再実行
    if (execve((*argv_)[0], (*argv_), (*envp_)) == -1) {
        perror("execve");
    }
}


int main(int argc, char *argv[], char *envp[])
{
    struct sigaction sa;
    int i;

    /**
     * コマンドライン引数　環境変数のアドレスをグローバルに保持
     * プロセス起動して　別ターミナルで kill -1 ${pid}   -1はSIGHUP
     */
    argc_=&argc;
    argv_=&argv;
    envp_=&envp;

    /**
     * コマンドライン引数　環境変数の表示
     */
    (void) fprintf(stderr, "start pid=%d\n", getpid());
    (void) fprintf(stderr, "argc=%d\n", argc);
    for (i = 0; argv[i] != NULL; i++) {
        (void) fprintf(stderr, "argv[%d]=%s\n", i, argv[i]);
    }
    for (i = 0; envp[i] != NULL; i++) {
        (void) fprintf(stderr, "envp[%d]=%s\n", i, envp[i]);
    }

    // sleep()によるSIGALRMを解除: Linuxで行わなくても良いが移植性のため
    (void) signal(SIGALRM, SIG_IGN);

    // SIGHUPのハンドラを指定
#ifdef USE_SIGNAL
    (void) signal(SIGHUP, sig_hangup_handler);

    // 現状の表示
    (void) sigaction(SIGHUP, (struct sigaction *) NULL, &sa);
    (void) fprintf(stderr, "SA_ONSTACK=%d\n", (sa.sa_flags&SA_ONSTACK) ? 1 : 0);
    (void) fprintf(stderr, "SA_RESETHAND=%d\n", (sa.sa_flags&SA_RESETHAND) ? 1 : 0);
    (void) fprintf(stderr, "SA_NODEFER=%d\n", (sa.sa_flags&SA_NODEFER) ? 1 : 0);
    (void) fprintf(stderr, "SA_RESTART=%d\n", (sa.sa_flags&SA_RESTART) ? 1 : 0);
    (void) fprintf(stderr, "SA_SIGINFO=%d\n", (sa.sa_flags&SA_SIGINFO) ? 1 : 0);
    (void) fprintf(stderr, "signal():end\n");
#else
    (void) sigaction(SIGHUP, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_hangup_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGHUP, &sa, (struct sigaction *) NULL);


    // 現状の表示
    (void) sigaction(SIGHUP, (struct sigaction *) NULL, &sa);
    (void) fprintf(stderr, "SA_ONSTACK=%d\n", (sa.sa_flags&SA_ONSTACK) ? 1 : 0);
    (void) fprintf(stderr, "SA_RESETHAND=%d\n", (sa.sa_flags&SA_RESETHAND) ? 1 : 0);
    (void) fprintf(stderr, "SA_NODEFER=%d\n", (sa.sa_flags&SA_NODEFER) ? 1 : 0);
    (void) fprintf(stderr, "SA_RESTART=%d\n", (sa.sa_flags&SA_RESTART) ? 1 : 0);
    (void) fprintf(stderr, "SA_SIGINFO=%d\n", (sa.sa_flags&SA_SIGINFO) ? 1 : 0);
    (void) fprintf(stderr, "signal():end\n");
#endif
    // 5秒おきにカウント表示
    for (i = 0;; i++) {
        (void) fprintf(stderr, "count=%d\n", i);
        sleep(5);
    }
}