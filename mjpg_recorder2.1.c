#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <locale.h>
#include <sys/stat.h>
#include <signal.h>
//#include <errno.h>
#include "image.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "bitmapfont.h"
#include "mem.h"

typedef enum {
    TERMINATED      = 2,
    COMPLETED       = 1,
    DISP_HELP       = 0,
    ARG_ERR         = -1,
    INTERNAL_ERR    = -2,
    CONNECTION_ERR  = -3,
    CONTENT_ERR     = -4,
    FILE_ERR        = -5,
    STREAM_ERR      = -6,
    MEMORY_ERR      = -7,
    LIBJPEG_ERR     = -8
} RETURN_CODE;

#define BOUNDARY_BUF_SIZE 128
    
RETURN_CODE begin_connection();
RETURN_CODE get_boundary(char *buf, int size);
char random_char();
bool file_exists(const char* filename);
void mkstemp2(char *path);
FILE *create_open_tmp_file(const char *dir, const char *name, char *buf, RETURN_CODE *rc);
RETURN_CODE process_image(bool debug);
char *generate_timestamp_chars(time_t now);
typedef enum {
    NOT_DRAW,
    LEFT_TOP,
    RIGHT_TOP,
    RIGHT_BOTTOM,
    LEFT_BOTTOM
} DRAWTEXTPOSITION;
void draw_text(image_t *img, char *text, DRAWTEXTPOSITION pos);
char* itoa(int value, char* result, int base);
int proc_args(int argc, char *argv[]);
char *move_to_end(char *p);
void create_output_filename(char *dst, time_t tm_start, time_t tm_end, int frame_count, int file_number);
bool copy_dir(char *dst, int buflen, char *src);
//int fcopy(const char *fnamer, const char *fnamew);
int read_char();
void push_char(char c);
int skip_until_content_start(FILE *fp);
bool skip_until(const char *word, int limit);
bool read_to_mem(MYMEMORY *pMem, int size);
time_t get_next_boder_time(int border_minute);

static char output_dir[1024];       ///tmp/ など
static char output_name[256];       //拡張子なし。%Y %M %D %H %I %S などが使える
static char target_host[256];       //192.168.0.26 など
static int opt_F = -1;              //１ファイルに保存するフレーム枚数
static int opt_T = -1;              //１ファイルに保存する秒数
static int opt_T0 = -1;             //１ファイルに保存する最初の秒数
static int opt_V = -1;              //ファイルへの書き出しを時刻の分に紐づけ
static int opt_N = -1;              //保存するファイル数
static int opt_I = 1;               //開始ファイル番号を指定。 %n で使用
static int opt_S = 1;               //フレームを間引く。フレームopt_S回に1回だけ保存
static char opt_u[256] = "/?action=stream"; //URI
static int opt_p = 8080;            //ポート番号
static DRAWTEXTPOSITION opt_t = NOT_DRAW;//タイムスタンプ表示位置 
static char opt_X[32] = "";         //表示テキスト
static DRAWTEXTPOSITION opt_x = LEFT_TOP;//テキスト表示位置 
static int opt_q = 75;              //jpeg出力品質
static char opt_m[1024] = "";       //サムネイルパス
static int opt_i = 0;               //入力データを調査する秒数
static int opt_c = 0;               //時計回りの回転角度 ROTATE_CLOCKWISE_xx の値を格納して使用

static int sofd;
static struct hostent     *shost;
static struct sockaddr_in sv_addr;

static MYMEMORY jpg_buffer;

/* SIGINTハンドラ */
volatile sig_atomic_t e_flag = 0;
void abrt_handler(int sig) { e_flag = 1; }


int main (int argc, char *argv[]) {

    /* 引数を処理 */
    int i = proc_args(argc, argv);
    if (i == 0) return DISP_HELP;
    if (i == -1) return ARG_ERR;

    /* 強制終了を処理するハンドラを設定 (ハンドラではe_flag = 1を実行) */
    if (signal(SIGINT, abrt_handler) == SIG_ERR) {
        fprintf(stderr, "%s: can not set SIGINT handler.\n", opt_X);
        return INTERNAL_ERR;
    }
    if (signal(SIGTERM, abrt_handler) == SIG_ERR) {
        fprintf(stderr, "%s: can not set SIGTERM handler.\n", opt_X);
        return INTERNAL_ERR;
    }

    /* ソケットの作成～接続 */
    RETURN_CODE rc;
    if ((rc = begin_connection()) < 0) {
        return rc;
    }

    /* boundaryの定義まで読み飛ばす */
    if (!skip_until(";boundary=", 2000)) {
		fprintf(stderr, "%s: err boundary identification is not found.\n", opt_X);
        close(sofd);
		return CONTENT_ERR;
    }

    /* boundary値を取得する */
    char boundary[BOUNDARY_BUF_SIZE + 6 + 2 + 1];
    if ((rc = get_boundary(boundary, BOUNDARY_BUF_SIZE)) < 0) {
        close(sofd);
        return rc;
    }

    /* 最初のboundaryまで読み飛ばす */
    if (!skip_until(boundary, 1000)) {
		fprintf(stderr, "%s: err can not reach start of contents.\n", opt_X);
        close(sofd);
		return CONTENT_ERR;
    }

    char tmp_path[256];
    char tmp_path_thumb[256];
    int skip_frame = opt_S;
    int receive_frame_count = 0;

    struct timespec start_clock;
    clock_gettime(CLOCK_MONOTONIC, &start_clock);
    //clock_t start_clock = clock();

    /* 停止シグナルまでループ */
    while (!e_flag) {

        /* mjpg書き込み用のファイルを開く */
        FILE *fp;
        if (opt_i == 0) {
            if ((fp = create_open_tmp_file(argv[1], "__work_XXXXXX", tmp_path, &rc)) == NULL) {
                close(sofd);
                return rc;
            }
        } else {
            //-iオプション時
            fp = NULL;
        }

        /* サムネイルファイルは未定 */
        tmp_path_thumb[0] = '\0';

        /* 開始時刻を保存 */
        time_t tm_start = time(NULL);

        /* 終了が時間指定の場合の終了時間を計算 */
        time_t end_time;
        if (opt_i == 0) {
            if (opt_T > 0) {
                struct tm *ts;
                if (opt_T0 > 0) {
                    //1回目
                    end_time = tm_start + opt_T0;
                    opt_T0 = 0;

                    //デバッグ
                    ts = localtime(&end_time);
                    fprintf(stderr, "%s: 1st file output at %d:%02d:%02d\n", opt_X, ts->tm_hour, ts->tm_min, ts->tm_sec);
                } else {
                    //2回目以降
                    end_time = tm_start + opt_T;

                    //デバッグ
                    ts = localtime(&end_time);
                    fprintf(stderr, "%s: next file output at %d:%02d:%02d\n", opt_X, ts->tm_hour, ts->tm_min, ts->tm_sec);
                }
            } else if (opt_V > 0) {
                end_time = get_next_boder_time(opt_V);
            }
        } else {
            //-iオプション時
            end_time = tm_start + opt_i;
        }

        int frame_count = 0;
        bool error_happend = false;
        while (!e_flag) {

            //\r\n\r\nまで読み飛ばし
            int content_length;
            if ((content_length = skip_until_content_start(fp)) < 0) {
                fprintf(stderr, "%s: err can not get contents.\n", opt_X);
                error_happend = true;
                rc = STREAM_ERR;
                break;
            }

            //Content-Lengthを取得できない
            if (content_length == 0) {
                fprintf(stderr, "%s: err can not get content-length.\n", opt_X);
                error_happend = true;
                rc = STREAM_ERR;
                break;
            }

            if (receive_frame_count == 0) {
                //jpeg処理バッファを最初のフレームの２倍のサイズで確保する
                if (jpg_buffer.allocated_size == 0)
                    if (!alloc_memory(&jpg_buffer, content_length * 2, ALLOC_NEW, false)) {
                        fprintf(stderr, "%s: err can not allocate %d bytes of memory.\n",
                            opt_X, content_length * 2);
                        error_happend = true;
                        rc = MEMORY_ERR;
                        break;
                    }
                //デバッグ
                fprintf(stderr, "%s: content-length: %d\n", opt_X, content_length);
            }

            //画像データをバッファに読み込む
            if (!read_to_mem(&jpg_buffer, content_length)) {
                fprintf(stderr, "%s: err can not get contents.\n", opt_X);
                error_happend = true;
                rc = STREAM_ERR;
                break;
            }
            ++receive_frame_count;

            if (opt_i == 0) {
                //情報収集モードでない

                if (--skip_frame == 0) {
                    skip_frame = opt_S;
                    //間引きフレームでない

                    //タイムスタンプ書き込みや回転など、必要な場合のみ画像を加工する
                    if (opt_c != ROTATE_CLOCKWISE_0 || opt_t != NOT_DRAW ||
                        (opt_X[0] != '\0' && opt_x != NOT_DRAW)) {
                        
                        //文字列やタイムスタンプを書き込む
                        if ((rc = process_image(frame_count == 0)) < 0) {
                            error_happend = true;
                            break;
                        }

                        //デバッグ
                        if (frame_count == 1)
                            fprintf(stderr, "%s: generated content-length: %d\n",
                                opt_X, jpg_buffer.active_size);
                    }

                    /* 画像データをファイルに書き出す */
                    fputs(&boundary[(frame_count == 0) ? 4 : 2], fp);
                    fputs("Content-Type: image/jpeg\r\n", fp);
                    fprintf(fp, "Content-Length: %d\r\n", jpg_buffer.active_size);
                    fputs("X-Timestamp: 0.000000\r\n\r\n", fp);
                    if (!save_memory_to_fp(&jpg_buffer, fp)) {
                        fprintf(stderr, "%s: err can not write target size %d bytes to mjpg file.\n",
                            opt_X, jpg_buffer.active_size);
                        error_happend = true;
                        rc = FILE_ERR;
                        break;
                    }

                    /* サムネイル作成 */
                    if (frame_count == 0 && opt_m[0] != '\0') {

                        /* jpg書き込み用のファイルを開く */
                        FILE *thumbFp;
                        if ((thumbFp = create_open_tmp_file(argv[1], "__thumb_XXXXXX",
                            tmp_path_thumb, &rc)) == NULL) {
                            
                            error_happend = true;
                            break;
                        }

                        if (!save_memory_to_fp(&jpg_buffer, thumbFp)) {
                            fprintf(stderr, "%s: err can not write target size %d bytes to thumbnail file.\n",
                                opt_X, jpg_buffer.active_size);
                            fclose(thumbFp);
                            error_happend = true;
                            rc = FILE_ERR;
                            break;
                        }
                        fclose(thumbFp);
                    }

                    ++frame_count;

                    /* 指定されたフレーム数を保存した */
                    if (opt_F > 0 && frame_count >= opt_F) break;

                    /* 指定された秒数が経過した */
                    if ((opt_T > 0 || opt_V > 0) && time(NULL) >= end_time) break;
                }
            } else {
                //-iオプション時
                /* 指定された秒数が経過した */
                if (time(NULL) >= end_time) break;
            }
        }

        //上記ループ内でエラーがあっても、ファイルが空でなければきりのいい状態に処理する

        /*  終了時刻を保存 */
        time_t tm_end = time(NULL);

        if (opt_i == 0) {
            /* mjpgファイルサイズを取得 */
            long mjpg_file_size = ftell(fp);
            fclose(fp);
            fprintf(stderr, "%s: %ld bytes of mjpg file created.\n", opt_X, mjpg_file_size);

            /* サムネイル画像の処理 */
            char correct_path[256];
            if (tmp_path_thumb[0] != '\0') {
                if (file_exists(tmp_path_thumb)) {

                    /* 元ファイルの権限を変更 600=>666 してPythonから触れるようにする */
                    if (chmod(tmp_path_thumb,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != 0) {
                        
                        fprintf(stderr, "%s: err can not modify auth of thumbnail file '%s'.\n",
                            opt_X, tmp_path_thumb);
                        if (!error_happend) {
                            error_happend = true;
                            rc = FILE_ERR;
                        }
                    } else {
                        /* 正規のファイル名を作成 */
                        strcpy(correct_path, opt_m);
                        create_output_filename(move_to_end(correct_path), tm_start, tm_end,
                            frame_count, opt_I);
                        strcat(correct_path, ".jpg");

                        /* ファイル名を.jpgに変更 */
                        if (rename(tmp_path_thumb, correct_path) != 0) {
                            fprintf(stderr, "%s: err can not rename thumbnail file '%s'.\n",
                                opt_X, tmp_path_thumb);
                            if (!error_happend) {
                                error_happend = true;
                                rc = FILE_ERR;
                            }
                        } else {
                            tmp_path_thumb[0] = '\0';
                        }
                    }
                } else {
                    tmp_path_thumb[0] = '\0';
                }
            }

            /* mjpgファイルの処理 */
            if (mjpg_file_size > 100000) {

                /* 元ファイルの権限を変更 600=>666 してPythonから触れるようにする */
                if (chmod(tmp_path,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != 0) {
                    
                    fprintf(stderr, "%s: err can not modify auth of mjpg file '%s'.\n",
                        opt_X, tmp_path);
                    if (!error_happend) {
                        error_happend = true;
                        rc = FILE_ERR;
                    }
                } else {
                    /* 正規のファイル名を作成 */
                    strcpy(correct_path, output_dir);
                    create_output_filename(move_to_end(correct_path), tm_start, tm_end,
                        frame_count, opt_I);
                    strcat(correct_path, ".mjpg");

                    /* ファイル名を.mjpgに変更 */
                    if (rename(tmp_path, correct_path) != 0) {
                        fprintf(stderr, "%s: err can not rename mjpg file '%s'.\n",
                            opt_X, tmp_path);
                        if (!error_happend) {
                            error_happend = true;
                            rc = FILE_ERR;
                        }
                    } else {
                        tmp_path[0] = '\0';
                    }
                }
            }
        } else {
            //-iオプション時
            if (!error_happend) {
                struct timespec end_clock;
                clock_gettime(CLOCK_MONOTONIC, &end_clock);
                //clock_t end_clock = clock();

                double elapsed = (end_clock.tv_sec - start_clock.tv_sec);
                elapsed += (double)(end_clock.tv_nsec - start_clock.tv_nsec) / 1000000000.0f;

                fprintf(stderr, "running time: %.2lf sec\n", elapsed);
                fprintf(stderr, "received frames: %d\n", receive_frame_count);
                fprintf(stderr, "frame rate: %.2lf\n", (double)receive_frame_count / elapsed);

                //疑似エラーとして処理
                error_happend = true;
                rc = COMPLETED;
            }
        }

        /* エラー処理 */
        if (error_happend) {
            free_memory(&jpg_buffer);
            close(sofd);
            if (tmp_path[0] != '\0') remove(tmp_path);
            if (tmp_path_thumb[0] != '\0') remove(tmp_path_thumb);
            return rc;
        }

        ++opt_I;

        /* 既定のファイル数に保存した */
        if (opt_N > 0) {
            if (--opt_N == 0)
                break;
        }
    }

    free_memory(&jpg_buffer);
    close(sofd);

    return (e_flag == 1) ? TERMINATED : COMPLETED;
}

//ソケットの作成～接続まで
RETURN_CODE begin_connection() {
    /* ソケットを作成 */
    sofd = socket(AF_INET, SOCK_STREAM, 0);
    if (sofd < 0) {
        fprintf(stderr, "%s: can not open SOCKET.\n", opt_X);
        return CONNECTION_ERR;
    }

    /* アドレスを定義 */
    shost = gethostbyname(target_host);
    if (shost == NULL) {
        fprintf(stderr, "%s: err happend in gethostbyname function.\n", opt_X);
        return CONNECTION_ERR;
    }

    memset(&sv_addr, 0, sizeof(sv_addr));
    sv_addr.sin_family = AF_INET;
    sv_addr.sin_port   = htons(opt_p);
    memcpy((char *)&sv_addr.sin_addr, (char *)shost->h_addr, shost->h_length);

    /* コネクトする */
    if (connect(sofd, (const struct sockaddr*)&sv_addr, sizeof(sv_addr)) < 0) {
        fprintf(stderr, "%s: err happend in connect function.\n", opt_X);
        return CONNECTION_ERR;
    }

    /* HTTPのやりとり */
    send(sofd, "GET ",       4,                   0);
    send(sofd, opt_u,        strlen(opt_u),       0);
    send(sofd, " HTTP/1.0",  9,                   0);
    send(sofd, "\r\n",       2,                   0);
    send(sofd, "Host: ",     6,                   0);
    send(sofd, target_host,  strlen(target_host), 0);
    send(sofd, "\r\n\r\n",   4,                   0);
    return 0;
}

RETURN_CODE get_boundary(char *buf, int size) {
    strcpy(buf, "\r\n\r\n--");
    buf += 6;
    int n = 0;
    char c;
    while (1) {
        c = read_char();
        if (c == -1) return CONNECTION_ERR;  //データなし
        if (c == '\r') {
            push_char(c);
            break;
        }
        *buf++ = c;
        if (--size <= 0) {
		    fprintf(stderr, "%s: err boundary length too long.\n", opt_X);
            return CONTENT_ERR;
        }
    }
    strcpy(buf, "\r\n");
    return 0;
}

char random_char() {
    static bool init = false;
    if (!init) {
        struct timeval myTime;
        gettimeofday(&myTime, NULL);
        srand((unsigned int)myTime.tv_usec);
        init = true;
    }

    int r = rand() & 63;
    if (r < 26) return 'A' + r;
    r -= 26;
    if (r < 26) return 'a' + r;
    r -= 26;
    if (r < 10) return '0' + r;
    return (r == 0) ? '-' : '_';
}

bool file_exists(const char* filename) {
    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

//パスの末尾に連続する'X'をファイル名の重複しない任意の文字に置き換えて返す
void mkstemp2(char *path) {
    if (path == NULL) return;
    int l = strlen(path);
    if (l == 0) return;
    int lx = 0;
    char *p = path;
    while (*p != '\0') {
        lx = (*p == 'X') ? lx + 1 : 0;
        ++p;
    }
    if (lx == 0) return;
    int i;
    do {
        p = path + l - 1;
        for (i = 0; i < lx; ++i) {
            *p = random_char();
            --p;
        }
    } while (file_exists(path));
}

FILE *create_open_tmp_file(const char *dir, const char *name, char *buf, RETURN_CODE *rc) {
    /* 一時ファイルを作成 */
    strcpy(buf, dir);
    strcat(buf, name);

    mkstemp2(buf); //bufは書き換えられる
    FILE *fp;
    if((fp = fopen(buf, "wb")) == NULL) {
        fprintf(stderr, "%s: err can not open(wb) file '%s'.\n", buf, opt_X);
        *rc = FILE_ERR;
        return NULL;
    }
    return fp;

/*    int fd = mkstemp(buf); //bufは書き換えられる
    if (fd == -1) {
        fprintf(stderr, "err can not create tmp file '%s'.\n", buf);
        *rc = FILE_ERR;
        return NULL;
    }*/
    /* バイナリ書き込みモードでファイルをオープン */
/*    FILE *fp;
    if((fp = fdopen(fd, "wb")) == NULL) {
        fprintf(stderr, "err can not open(wb) file '%s'.\n", buf);
        *rc = FILE_ERR;
        return NULL;
    }
    return fp;*/
}

RETURN_CODE process_image(bool debug) {
    static time_t timestamp = 0;
    static char *timestamp_chars = NULL;

    //バッファからファイルポインタを取得
    FILE* memFp;
    if ((memFp = memory_to_readable_fp(&jpg_buffer)) == NULL) {
        fprintf(stderr, "%s: err can not get readable file pointer from memory.\n", opt_X);
        return MEMORY_ERR;
    }

    //jpegデータからimage_tを作成
    //image_t *pImg = read_jpeg_stream(memFp);
    image_t *pImg = read_jpeg_streamEx(memFp, opt_c); //オプションに応じて回転して読み込む
    fclose(memFp);

    if (pImg == NULL) {
        fprintf(stderr, "%s: err can not convert jpeg to image_t on memory.\n", opt_X);
        return LIBJPEG_ERR;
    }

    if (debug)
        fprintf(stderr, "Image size: %d x %d\n", pImg->width, pImg->height);

    //テキストを描画
    if (opt_X[0] != '\0' && opt_x != NOT_DRAW) {
        draw_text(pImg, opt_X, opt_x);
    }

    //タイムスタンプを描画
    if (opt_t != NOT_DRAW) {
        time_t now = time(NULL);
        if (now != timestamp) {
            timestamp = now;
            timestamp_chars = generate_timestamp_chars(timestamp);
        }
        draw_text(pImg, timestamp_chars, opt_t);
    }

    bool retry = false;
    result_t res;
redo:
    //バッファに書き込むためのファイルポインタを取得
    if ((memFp = memory_to_writable_fp(&jpg_buffer)) == NULL) {
        fprintf(stderr, "%s: err can not get writable file pointer from memory.\n", opt_X);
        free_image(pImg);
        return MEMORY_ERR;
    }
    //image_tからjpegデータを作成
    res = write_jpeg_stream(memFp, pImg, opt_q);
    if (res != SUCCESS && !retry) {
        fclose(memFp);

        //失敗した場合は、１度だけバッファサイズを２倍にしてリトライ
        if (!alloc_memory(&jpg_buffer, jpg_buffer.allocated_size * 2,
            REALLOC_GROW_ONLY, false)) {
            
            fprintf(stderr, "%s: err can not grow %d bytes of memory.\n",
                 opt_X, jpg_buffer.allocated_size * 2);
            free_image(pImg);
            return MEMORY_ERR;
        }
        retry = true;
        goto redo;
    }

    free_image(pImg);
                
    if (res != SUCCESS) {
        fprintf(stderr, "%s: err can not convert image_t to jpeg on memory.\n", opt_X);
        fclose(memFp);
        return LIBJPEG_ERR;
    }

    //メモリに書き込まれたサイズを取得する
    jpg_buffer.active_size = ftell(memFp);
    fclose(memFp);
    return 0;
}

char *generate_timestamp_chars(time_t now) {
    struct tm *ts = localtime(&now);
    int Y = ts->tm_year - 100;  //2桁
    int M = ts->tm_mon + 1;
    int D = ts->tm_mday;
    int H = ts->tm_hour;
    int I = ts->tm_min;
    int S = ts->tm_sec;

    static char buf[20]; //YY/MM/DD HH:MM:SS
    char *p = &buf[0];
    itoa(Y, p, 10); p += 2;
    *p++ = '/';
    if (M < 10) *p++ = '0';
    itoa(M, p, 10);
    p += (M < 10) ? 1 : 2;
    *p++ = '/';
    if (D < 10) *p++ = '0';
    itoa(D, p, 10);
    p += (D < 10) ? 1 : 2;
    *p++ = ' ';
    if (H < 10) *p++ = '0';
    itoa(H, p, 10);
    p += (H < 10) ? 1 : 2;
    *p++ = ':';
    if (I < 10) *p++ = '0';
    itoa(I, p, 10);
    p += (I < 10) ? 1 : 2;
    *p++ = ':';
    if (S < 10) *p++ = '0';
    itoa(S, p, 10);

    return &buf[0];
}

/*void set_pixel(image_t *img, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (img == NULL ||
        img->color_type != COLOR_TYPE_RGB ||
        x < 0 || x >= img->width ||
        y < 0 || y >= img->height) return;
    pixcel_t *p = &img->map[y][x];
    p->r = r;
    p->g = g;
    p->b = b;
}*/

void draw_text(image_t *img, char *text, DRAWTEXTPOSITION pos) {
    if (img == NULL ||
        img->color_type != COLOR_TYPE_RGB ||
        img->height < 8 ||
        text == NULL) return;

    int l = strlen(text);
    int w = l << 3;
    if (img->width < w) return;

    int x,y;
    switch (pos) {
    case LEFT_TOP:
        x = 0;
        y = 0;
        break;
    case RIGHT_TOP:
        x = img->width - w;
        y = 0;
        break;
    case RIGHT_BOTTOM:
        x = img->width - w;
        y = img->height - 8;
        break;
    case LEFT_BOTTOM:
        x = 0;
        y = img->height - 8;
        break;
    default:
        return;
    }
    if (x < 0 || y < 0) return;

    unsigned char *p;
    pixcel_t *pp;
    unsigned char b;
    while (*text != '\0') {
        p = get_88font(*text++);
        if (p != NULL) {
            for (int yy = 0; yy < 8; ++yy) {
                b = *p++;
                pp = &img->map[y + yy][x];
                for (int xx = 0; xx < 8; ++xx) {
                    if (b & 1) {
                        pp->c.r = 0xff;
                        pp->c.g = 0xff;
                        pp->c.b = 0xff;
                    } else {
                        pp->c.r = 0x00;
                        pp->c.g = 0x00;
                        pp->c.b = 0x00;
                    }
                    b >>= 1;
                    ++pp;
                }
            }
        }        
        x += 8;
    }
}




/**
 * C++ version 0.4 char* style "itoa":
 * Written by Lukás Chmela
 * Released under GPLv3.

    */
char* itoa(int value, char* result, int base) {
    // check that the base if valid
    if (base < 2 || base > 36) { *result = '\0'; return result; }

    char* ptr = result, *ptr1 = result, tmp_char;
    int tmp_value;

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
    } while ( value );

    // Apply negative sign
    if (tmp_value < 0) *ptr++ = '-';
    *ptr-- = '\0';
    while(ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr--= *ptr1;
        *ptr1++ = tmp_char;
    }
    return result;
}

//-1:引数エラー 0:ヘルプを表示した 1:オプションOK。処理続行
int proc_args(int argc, char *argv[]) {

    /* ヘルプの表示 */
    if ((argc >=2 && strncmp(argv[1], "-h", 2) == 0) || argc < 4) {
        fprintf(stderr, "ネットワークカメラなどのモーションJPEG(.mjpg)をファイルに保存する\n");
        fprintf(stderr, "処理の停止はSIGINT(CTRL+C)で行えます\n");
        fprintf(stderr, "第１引数　mjpg出力ディレクトリ。/tmp/など（\"\"で囲える）\n");
        fprintf(stderr, "第２引数　mjpg出力ファイル名（ディレクトリ、拡張子なし、\"\"で囲える）。例 video-%%Y%%m%%d-%%H%%M%%Sなど\n");
        fprintf(stderr, "　　　　　次の書式文字列を使用できる\n");
        fprintf(stderr, "　　　　　%%Y %%M %%D %%H %%I %%S 開始時の 西暦4桁 月2桁 日2桁 時2桁 分2桁 秒2桁\n");
        fprintf(stderr, "　　　　　%%y %%m %%d %%h %%i %%s 終了時の 西暦4桁 月2桁 日2桁 時2桁 分2桁 秒2桁\n");
        fprintf(stderr, "　　　　　%%f 保存されたフレーム数\n");
        fprintf(stderr, "　　　　　%%t 保存された長さ（秒）\n");
        fprintf(stderr, "　　　　　%%n ファイル番号\n");
        fprintf(stderr, "第３引数　ホスト名。192.168.0.26など\n");
        fprintf(stderr, "第４引数以降はオプション。n:整数 s:文字列（\"\"で囲える） w:特定のワード\n");
        fprintf(stderr, "　　　　　-u s リクエスト先のURI（未指定の場合は /?action=stream ）\n");
        fprintf(stderr, "　　　　　-p n リクエスト先のポート番号（未指定の場合は 8080 ）\n");
        fprintf(stderr, "　　　　　-F n １ファイルに保存するフレーム枚数（-T,-Vオプションと併用不可）\n");
        fprintf(stderr, "　　　　　-T n １ファイルに保存する秒数（-F,-Vオプションと併用不可）\n");
        fprintf(stderr, "　　　　　-T n:n -Tオプションの別の指定方法。最初のnは最初の出力ファイルにのみ適用される \n");
        fprintf(stderr, "　　　　　-V n ファイルに書き出すタイミングを時刻の分に紐づける（-F,-Tオプションと併用不可）\n");
        fprintf(stderr, "　　　　　   n=1～60（但しnは60を割り切れる値であること）\n");
        fprintf(stderr, "　　　　　   例 n=10　任意の時刻の0,10,20,30,40,50分にファイルを吐き出す\n");
        fprintf(stderr, "　　　　　   例 n=30　任意の時刻の0,30分にファイルを吐き出す\n");
        fprintf(stderr, "　　　　　-N n 保存するファイル数（未指定の場合は無制限）\n");
        fprintf(stderr, "　　　　　-S n 保存するフレームを間引く。例えば3を指定した場合、3フレームに1回だけ保存する（デフォルトは 1）\n");
        fprintf(stderr, "　　　　　-I n 開始ファイル番号を指定。%%n書式で使用する（未指定の場合は 1 ）\n");
        fprintf(stderr, "　　　　　-t w タイムスタンプの表示位置 LT:左上 RT:右上 RB:右下 LB:左下（未指定は表示しない）\n");
        fprintf(stderr, "　　　　　-X s 任意のテキストを表示（日本語不可）\n");
        fprintf(stderr, "　　　　　-x w -Xオプションのテキスト表示位置 LT:左上 RT:右上 RB:右下 LB:左下（未指定はLT）\n");
        fprintf(stderr, "　　　　　-c n 時計回りに指定角度回転して保存する。 n=0,90,180,270 のいずれか\n");
        fprintf(stderr, "　　　　　-q n -tまたは-X,-cオプション使用時のjpeg出力品質[%%]（未指定は75）\n");
        fprintf(stderr, "　　　　　-m s サムネイルの出力ディレクトリを指定する。ファイル名はmjpgと同じで拡張子はjpgになる。例 /tmp/thumb/\n");
        fprintf(stderr, "　　　　　-i n 何も出力せずに入力ビデオの情報のみを出力する特殊オプション。nは調査する秒数（1以上。0または未指定で無効）\n");
        return 0;
    }

    if (!copy_dir(output_dir, sizeof(output_dir), argv[1])) {
        fprintf(stderr, "第1引数エラー。長すぎです\n");
        return -1;
    }

    if (!copy_dir(output_name, sizeof(output_name), argv[2])) {
        fprintf(stderr, "第2引数エラー。長すぎです\n");
        return -1;
    }

    if (strlen(argv[3]) >= sizeof(target_host)) {
        fprintf(stderr, "第3引数エラー。長すぎです\n");
        return -1;
    }
    strcpy(target_host, argv[3]);

    int i = 4;
    bool b = true;
    char c, *p;
    while (argc > i) {
        if (b) {
            if (argv[i][0] != '-') {
                fprintf(stderr, "第%d引数エラー。オプションは-で始めます\n", i);
                return -1;
            }
            if (strlen(argv[i]) != 2) {
                fprintf(stderr, "第%d引数エラー。オプションは2文字です\n", i);
                return -1;
            }
            c = argv[i][1];
            switch (c) {
                case 'F':
                case 'T':
                case 'V':
                case 'N':
                case 'I':
                case 'u':
                case 'p':
                case 't':
                case 'X':
                case 'x':
                case 'q':
                case 'm':
                case 'S':
                case 'i':
                case 'c':
                    break;
                default:
                    fprintf(stderr, "第%d引数エラー。不明なオプション\n", i);
                    return -1;
            }
            b = false;
        } else {
            switch (c) {
                case 'F':
                    opt_F = atoi(argv[i]);
                    if (opt_F <= 0) {
                        fprintf(stderr, "第%d引数エラー。-Fオプションは1以上です\n", i);
                        return -1;
                    }
                    if (opt_T != -1 || opt_V != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'T':
                    p = strchr(argv[i], ':');
                    if (p) {
                        opt_T0 = atoi(argv[i]);
                        opt_T = atoi(p + 1);
                    } else {
                        opt_T = atoi(argv[i]);
                        opt_T0 = opt_T;
                    }
                    if (opt_T <= 0 || opt_T0 <= 0) {
                        fprintf(stderr, "第%d引数エラー。-Tオプションは1以上です\n", i);
                        return -1;
                    }
                    if (opt_F != -1 || opt_V != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'V':
                    opt_V = atoi(argv[i]);
                    if (opt_V < 1 || opt_V > 60) {
                        fprintf(stderr, "第%d引数エラー。-Vオプションは1以上60以下です\n", i);
                        return -1;
                    }
                    if (60 % opt_V != 0) {
                        fprintf(stderr, "第%d引数エラー。-Vオプションは60を割り切れる値のみです\n", i);
                        return -1;
                    }
                    if (opt_F != -1 || opt_T != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'N':
                    opt_N = atoi(argv[i]);
                    if (opt_N == 0) {
                        fprintf(stderr, "第%d引数エラー。-Nオプションは1以上です\n", i);
                        return -1;
                    }
                    break;
                case 'S':
                    opt_S = atoi(argv[i]);
                    if (opt_S <= 0 || opt_S > 60) {
                        fprintf(stderr, "第%d引数エラー。-Sオプションは1以上、60以下です\n", i);
                        return -1;
                    }
                    break;
                case 'I':
                    opt_I = atoi(argv[i]);
                    break;
                case 'u':
                    if (!copy_dir(opt_u, sizeof(opt_u) - 1, argv[i])) {
                        fprintf(stderr, "第%d引数エラー。-uオプションが長すぎ\n", i);
                        return -1;
                    }
                    if (opt_u[strlen(opt_u) - 1] != '/')
                        strcat(opt_u, "/");
                    break;
                case 'p':
                    opt_p = atoi(argv[i]);
                    if (opt_p < 1 || opt_p > 65535) {
                        fprintf(stderr, "第%d引数エラー。-pオプションのポート番号が範囲外\n", i);
                        return -1;
                    }
                    break;
                case 't':
                    if (strcmp(argv[i],"LT") == 0)
                        opt_t = LEFT_TOP;
                    else if (strcmp(argv[i],"RT") == 0)
                        opt_t = RIGHT_TOP;
                    else if (strcmp(argv[i],"RB") == 0)
                        opt_t = RIGHT_BOTTOM;
                    else if (strcmp(argv[i],"LB") == 0)
                        opt_t = LEFT_BOTTOM;
                    else {
                        fprintf(stderr, "第%d引数エラー。-tオプションのワードが不適切\n", i);
                        return -1;
                    }
                    break;
                case 'X':
                    if (strlen(argv[i]) >= sizeof(opt_X)) {
                        fprintf(stderr, "第%d引数エラー。-Xオプションの文字列が長すぎ\n", i);
                        return -1;
                    }
                    p = argv[i];
                    strcpy(opt_X, (*p == '"') ? p + 1 : p);
                    if (*p == '"') {
                        p = strchr(opt_X, '"');
                        if (p != NULL) *p = '\0';
                    }
                    break;
                case 'x':
                    if (strcmp(argv[i],"LT") == 0)
                        opt_x = LEFT_TOP;
                    else if (strcmp(argv[i],"RT") == 0)
                        opt_x = RIGHT_TOP;
                    else if (strcmp(argv[i],"RB") == 0)
                        opt_x = RIGHT_BOTTOM;
                    else if (strcmp(argv[i],"LB") == 0)
                        opt_x = LEFT_BOTTOM;
                    else {
                        fprintf(stderr, "第%d引数エラー。-xオプションのワードが不適切\n", i);
                        return -1;
                    }
                    break;
                case 'q':
                    opt_q = atoi(argv[i]);
                    if (opt_q < 1 || opt_q > 100) {
                        fprintf(stderr, "第%d引数エラー。-qオプションは1～100[%%]です\n", i);
                        return -1;
                    }
                    break;
                case 'm':
                    if (strlen(argv[i]) >= sizeof(opt_m)) {
                        fprintf(stderr, "第%d引数エラー。-mオプションのパスが長すぎ\n", i);
                        return -1;
                    }
                    p = argv[i];
                    strcpy(opt_m, (*p == '"') ? p + 1 : p);
                    if (*p == '"') {
                        p = strchr(opt_X, '"');
                        if (p != NULL) *p = '\0';
                    }
                    break;
                case 'i':
                    opt_i = atoi(argv[i]);
                    if (opt_i < 0 || opt_i > 60) {
                        fprintf(stderr, "第%d引数エラー。-iオプションは0以上、60以下です\n", i);
                        return -1;
                    }
                    break;
                case 'c':
                    switch (atoi(argv[i])) {
                        case 0:   opt_c = ROTATE_CLOCKWISE_0; break;
                        case 90:  opt_c = ROTATE_CLOCKWISE_90; break;
                        case 180: opt_c = ROTATE_CLOCKWISE_180; break;
                        case 270: opt_c = ROTATE_CLOCKWISE_270; break;
                        default:
                            fprintf(stderr, "第%d引数エラー。-cオプションは0,90,180,270のみ有効です\n", i);
                            return -1;
                    }
                    break;
            }
            b = true;
        }
        ++i;
    }
    return 1;
}

/* 文字列の末尾のポインタを返す */
char *move_to_end(char *p) {
    while (*p != '\0') ++p;
    return p;
}

/* output_nameからファイル名を生成 */
void create_output_filename(char *dst, time_t tm_start, time_t tm_end, int frame_count, int file_number) {
    struct tm *ts;

    ts = localtime(&tm_start);
    int Y = ts->tm_year + 1900;
    int M = ts->tm_mon + 1;
    int D = ts->tm_mday;
    int H = ts->tm_hour;
    int I = ts->tm_min;
    int S = ts->tm_sec;

    ts = localtime(&tm_end);
    int y = ts->tm_year + 1900;
    int m = ts->tm_mon + 1;
    int d = ts->tm_mday;
    int h = ts->tm_hour;
    int i = ts->tm_min;
    int s = ts->tm_sec;

    char *p = &output_name[0];
    bool f = false;
    while (*p != '\0') {
        if (f) {
            switch (*p) {
                case 'Y':
                    itoa(Y, dst, 10); dst = move_to_end(dst); break;
                case 'M':
                    if (M < 10) *dst++ = '0';
                    itoa(M, dst, 10); dst = move_to_end(dst); break;
                case 'D':
                    if (D < 10) *dst++ = '0';
                    itoa(D, dst, 10); dst = move_to_end(dst); break;
                case 'H':
                    if (H < 10) *dst++ = '0';
                    itoa(H, dst, 10); dst = move_to_end(dst); break;
                case 'I':
                    if (I < 10) *dst++ = '0';
                    itoa(I, dst, 10); dst = move_to_end(dst); break;
                case 'S':
                    if (S < 10) *dst++ = '0';
                    itoa(S, dst, 10); dst = move_to_end(dst); break;
                case 'y':
                    itoa(y, dst, 10); dst = move_to_end(dst); break;
                case 'm':
                    if (m < 10) *dst++ = '0';
                    itoa(m, dst, 10); dst = move_to_end(dst); break;
                case 'd':
                    if (d < 10) *dst++ = '0';
                    itoa(d, dst, 10); dst = move_to_end(dst); break;
                case 'h':
                    if (h < 10) *dst++ = '0';
                    itoa(h, dst, 10); dst = move_to_end(dst); break;
                case 'i':
                    if (i < 10) *dst++ = '0';
                    itoa(i, dst, 10); dst = move_to_end(dst); break;
                case 's':
                    if (s < 10) *dst++ = '0';
                    itoa(s, dst, 10); dst = move_to_end(dst); break;
                case 'f':
                    itoa(frame_count, dst, 10); dst = move_to_end(dst); break;
                case 't':
                    itoa(tm_end - tm_start, dst, 10);
                    dst = move_to_end(dst); break;
                case 'n':
                    itoa(file_number, dst, 10); dst = move_to_end(dst); break;
                default:
                    *dst++ = '%';
                    *dst++ = *p;
            }
            f = false;
        } else if (*p == '%') {
            f = true;
        } else {
            *dst++ = *p;
        }
        ++p;
    }
    *dst = '\0';
}

//コマンドライン引数のダブルクォーテーション除去
bool copy_dir(char *dst, int buflen, char *src) {
    bool b = (*src == '"');
    if (b) ++src;
    char *p = src;
    int n = 0;
    while (*p != '\0') {
        if (b && *p == '"') break;
        ++n;
        ++p;
    }
    if (n >= buflen) return false;
    memcpy(dst, src, n);
    *(dst + n) = '\0';
    return true;
}

//ファイルコピー
/*int fcopy(const char *fnamer, const char *fnamew) {
	FILE *fpr = fopen(fnamer, "rb");
	FILE *fpw = fopen(fnamew, "wb");
	if (fpr == NULL || fpw == NULL) {
        if (fpr != NULL) fclose(fpr); 
        if (fpw != NULL) fclose(fpw); 
		return -1;
	}

    char buf[1024];
    while (1) {
        size_t sz = fread(buf, 1, sizeof(buf), fpr);
        if (sz == 0) break;
        fwrite(buf, 1, sz, fpw);
    }

	fclose(fpw);
	fclose(fpr);
    return 0;
}*/

#define HTTP_BUF_SIZE 1024
static char http_res[HTTP_BUF_SIZE];
static int pop_char = -1;
static int http_res_len = 0;
static int http_res_read_index = 0;

int read_char() {
    if (pop_char != -1) {
        int c = pop_char;
        pop_char = -1;
        return c;
    }
    if (http_res_len == http_res_read_index) {
        http_res_len = recv(sofd, http_res, sizeof(http_res), 0);
        http_res_read_index = 0;
    }
    if (http_res_len == http_res_read_index)
        return -1;
    return http_res[http_res_read_index++] & 255;
}

void push_char(char c) {
    pop_char = (unsigned int)((unsigned char)c);
}

bool skip_until(const char *word, int limit) {
    int c, l = limit;
    const char *p = word;
    while (1) {
        c = read_char();
        if (c == -1) return false;
        if (c == *p) {
            if (*++p == '\0') return true;
        } else {
            p = word;
        }
        if (limit > 0 && --l <= 0) return false;
    }
}

//\r\n\r\n出現までスキップ
//Content-Length値を返す
//エラーの場合は-1を返す
int skip_until_content_start(FILE *fp) {
    const char *content_length_word = "Content-Length:";
    int content_length = -1;
    const char *p = content_length_word;
    bool buffering = false;
    char buf1[16];
    int j = 0;
    char buf2[5];
    buf2[4] = '\0';
    int i = 0;
    int c;
    int limit = 1000;
    while (1) {
        c = read_char();
        if (c == -1) return -1; //no more data

        if (content_length == -1) {
            if (!buffering) {
                if (c == *p) {
                    if (*++p == '\0') buffering = true;
                } else {
                    p = content_length_word;
                }
            } else {
                if (c != '\r' && c != '\n') {
                    buf1[j++] = c;
                    if (j == sizeof(buf1)) return -1; //buf1[] full
                } else {
                    buf1[j] = '\0';
                    content_length = atoi(buf1);
                }
            }
        }

        buf2[i] = c;
        i = ++i & 3;
        if (c == '\n' && (strcmp(buf2, "\r\n\r\n") == 0 || strcmp(buf2, "\n\r\n\r") == 0))
            return (content_length == -1) ? 0 : content_length;

        if (--limit == 0) return -1;
    }
}

bool read_to_mem(MYMEMORY *pMem, int size) {
    char *p = (char *)pMem->ptr;
    int n = size;
    while (n-- > 0) {
        int c = read_char();
        if (c == -1) return false;
        *p++ = c;
    }
    pMem->active_size = size;
    return true;
}

//border_minuteは1～60の値かつ60を割り切れる値であること
time_t get_next_boder_time(int border_minute) {
    time_t now = time(NULL);
    struct tm *pts = localtime(&now);
    int min = (int)(pts->tm_min / border_minute) * border_minute;
    while (min <= pts->tm_min) min += border_minute;
    struct tm ts;
    memcpy(&ts, pts, sizeof(ts));
    ts.tm_min = min;
    ts.tm_sec = 0;
    return mktime(&ts);
}
