/*
 * 'Portal' outro recreation.
 * Reproducing the 'Still Alive' terminal credits in pure c
 * I really enjoyed this game and this ending!
 * Made by: Sidharth "Siddhi" Sharma (sidharthify)
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#include "stillalive_data.h"

#define CHAR_DASH 0x2d
#define AUDIO_START_MS 6750
#define CREDITS_START_MS 9000
#define BLINK_MS 300
#define CREDIT_VELOCITY_MS 68.623562

#define MAX_GRID_ROWS 256
#define MAX_GRID_COLS 512

/*
 * scaling modes for adapting to different terminal dimensions.
 * aspect mode keeps the original four by three ratio and fills the height.
 * fill mode stretches the boxes across all rows and columns.
 * classic mode locks the window to the original ninety eight by thirty eight box.
 */
typedef enum {
    SCALE_ASPECT = 0,
    SCALE_FILL = 1,
    SCALE_CLASSIC = 2,
    SCALE_MAX
} scale_mode_t;

static scale_mode_t g_scale_mode = SCALE_ASPECT;
static volatile sig_atomic_t g_winch_flag = 0;

static struct termios g_orig_termios;
static bool g_raw_mode = false;
static pid_t g_audio_pid = 0;
static bool g_running = true;
static bool g_paused = false;

static uint32_t g_credit_starts[NUM_CREDITS];
static uint32_t g_credit_durs[NUM_CREDITS];

static int g_current_art_id = ART_NONE;
static bool g_getting_faster = false;
static uint32_t g_next_shuffle_ms = 0;
static uint32_t g_shuffle_interval_ms = 5000;

static char g_grid[MAX_GRID_ROWS][MAX_GRID_COLS];

/*
 * when exiting or interrupted we need to make sure the terminal is left
 * in a clean usable state. that means showing the cursor again, returning
 * from the alternate screen buffer, and restoring canonical echo mode.
 * any child player spawned for audio is also cleaned up here.
 */
static void write_all(int fd, const void *buf, size_t count){
    const char *p = (const char *)buf;
    while (count > 0){
        ssize_t n = write(fd, p, count);
        if (n < 0){
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK){
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
        p += n;
        count -= (size_t)n;
    }
}

static void restore_terminal(void){
    if (g_audio_pid > 0) {
        kill(g_audio_pid, SIGTERM);
        waitpid(g_audio_pid, NULL, WNOHANG);
        g_audio_pid = 0;
    }
    printf("\033[?2025l\033[?7h\033[?25h\033[?1049l\033[0m");
    fflush(stdout);
    if (g_raw_mode){
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_raw_mode = false;
    }
}

static void handle_signal(int sig){
    (void)sig;
    g_running = false;
}

/*
 * sigwinch fires whenever the user resizes their window or goes fullscreen.
 * we record the event so the renderer can immediately issue a full wipe
 * before rendering the newly scaled layout.
 */
static void handle_winch(int sig) {
    (void)sig;
    g_winch_flag = 1;
}

/*
 * raw mode turns off line buffering and echo so keypresses like spacebar
 * or the quit key register immediately without waiting for enter.
 */
static void enable_raw_mode(void){
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0){
        return;
    }
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        g_raw_mode = true;
    }
}

static uint64_t get_time_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/*
 * calculate the cumulative typing timestamps for the credits roll.
 * the velocity is roughly sixty eight milliseconds per character based on
 * the original source engine scripts, which keeps the roll perfectly in sync
 * with the track lyrics.
 */
static void init_credits_timeline(void) {
    double cur = (double)CREDITS_START_MS;
    for (int i = 0; i < NUM_CREDITS; i += 1){
        g_credit_starts[i] = (uint32_t)cur;
        int len = (int)strlen(g_credits[i]);
        double d = (len == 0 ? 1.0 : (double)len) * CREDIT_VELOCITY_MS;
        g_credit_durs[i] = (uint32_t)d;
        cur += d;
    }
}

static const char *find_audio_file(void){
    static const char *candidates[] = {
        "res/song/stillalive.ogg",
        "res/song/stillalive.mp3",
        "res/song/stillalive.m4a",
        NULL
    };
    for (int i = 0; candidates[i] != NULL; i += 1) {
        if (access(candidates[i], R_OK) == 0){
            return candidates[i];
        }
    }
    return NULL;
}

/*
 * rather than linking a heavy audio framework, we fork an external player.
 * pipewire, pulseaudio, sox, alsa, and ffmpeg CLI tools are probed in order
 * so the audio works out of the box on almost any linux or bsd install.
 */
static pid_t launch_audio_player(const char *path, double start_sec) {
    if (!path){
        return 0;
    }
    static const char *players[] = {
        "pw\055play",
        "play",
        "ffplay",
        "afplay",
        "aplay",
        "mpv",
        NULL
    };

    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0){
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char sec_buf[32];
        snprintf(sec_buf, sizeof(sec_buf), "%.2f", start_sec);

        char mpv_start[64];
        snprintf(mpv_start, sizeof(mpv_start), "\055\055start=%.2f", start_sec);

        for (int i = 0; players[i] != NULL; i += 1){
            const char *p = players[i];
            if (strcmp(p, "ffplay") == 0) {
                if (start_sec > 0.0){
                    char *args[] = {(char *)p, "\055nodisp", "\055autoexit", "\055loglevel", "quiet", "\055ss", sec_buf, (char *)path, NULL};
                    execvp(p, args);
                } else {
                    char *args[] = {(char *)p, "\055nodisp", "\055autoexit", "\055loglevel", "quiet", (char *)path, NULL};
                    execvp(p, args);
                }
            } else if (strcmp(p, "mpv") == 0){
                if (start_sec > 0.0) {
                    char *args[] = {(char *)p, "\055\055no\055video", "\055\055really\055quiet", mpv_start, (char *)path, NULL};
                    execvp(p, args);
                } else {
                    char *args[] = {(char *)p, "\055\055no\055video", "\055\055really\055quiet", (char *)path, NULL};
                    execvp(p, args);
                }
            } else if (strcmp(p, "play") == 0) {
                if (start_sec > 0.0){
                    char *args[] = {(char *)p, (char *)path, "trim", sec_buf, NULL};
                    execvp(p, args);
                } else {
                    char *args[] = {(char *)p, (char *)path, NULL};
                    execvp(p, args);
                }
            } else {
                char *args[] = {(char *)p, (char *)path, NULL};
                execvp(p, args);
            }
        }
        _exit(1);
    }
    return pid;
}

/*
 * the main rendering engine.
 * it sizes the boxes according to the active scaling mode, clears the canvas,
 * draws ascii borders, types lyrics character by character, scrolls the credits,
 * centers the ascii art, and writes the entire screen buffer to stdout.
 * every line is wiped with an ansi erase code so resizing leaves no artifacts.
 */
static void render_screen(uint32_t elapsed_ms, int term_rows, int term_cols){
    static int s_last_cols = 0;
    static int s_last_rows = 0;
    static scale_mode_t s_last_mode = (scale_mode_t)-1;
    bool needs_clear = false;

    if (g_winch_flag || term_cols != s_last_cols || term_rows != s_last_rows || g_scale_mode != s_last_mode) {
        needs_clear = true;
        g_winch_flag = 0;
        s_last_cols = term_cols;
        s_last_rows = term_rows;
        s_last_mode = g_scale_mode;
    }

    int min_w = 98;
    int min_h = 38;
    int frame_w = min_w;
    int frame_h = min_h;

    if (g_scale_mode == SCALE_FILL){
        frame_w = term_cols >= min_w ? term_cols - 2 : min_w;
        frame_h = term_rows >= min_h ? term_rows - 2 : min_h;
    } else if (g_scale_mode == SCALE_ASPECT) {
        int avail_h = term_rows >= min_h ? term_rows - 2 : min_h;
        int avail_w = term_cols >= min_w ? term_cols - 2 : min_w;
        double ratio = 98.0 / 38.0;
        int h = avail_h;
        int w = (int)((double)h * ratio);
        if (w > avail_w){
            w = avail_w;
            h = (int)((double)w / ratio);
        }
        if (w < min_w) {
            w = min_w;
        }
        if (h < min_h){
            h = min_h;
        }
        frame_w = w;
        frame_h = h;
    } else {
        frame_w = min_w;
        frame_h = min_h;
    }

    if (frame_h > MAX_GRID_ROWS) {
        frame_h = MAX_GRID_ROWS;
    }
    if (frame_w >= MAX_GRID_COLS){
        frame_w = MAX_GRID_COLS - 1;
    }

    int left_w = (int)((double)frame_w * 47.0 / 98.0);
    if (left_w < 24) {
        left_w = 24;
    }
    if (left_w > frame_w - 20){
        left_w = frame_w - 20;
    }

    int gap = 2;
    int right_x = left_w + gap;
    int right_w = frame_w - right_x;
    if (right_w < 20) {
        right_w = 20;
    }

    int credits_h = (int)((double)frame_h * 17.0 / 38.0);
    if (credits_h < 6){
        credits_h = 6;
    }
    if (credits_h > frame_h - 10) {
        credits_h = frame_h - 10;
    }

    for (int r = 0; r < frame_h; r += 1){
        for (int c = 0; c < frame_w; c += 1) {
            g_grid[r][c] = ' ';
        }
        g_grid[r][frame_w] = '\0';
    }

    /* left box borders */
    for (int c = 0; c < left_w; c += 1){
        g_grid[0][c] = (char)CHAR_DASH;
        g_grid[frame_h - 1][c] = (char)CHAR_DASH;
    }
    for (int r = 1; r < frame_h - 1; r += 1) {
        g_grid[r][0] = '|';
        g_grid[r][left_w - 1] = '|';
    }

    /* right box borders */
    for (int c = right_x; c < right_x + right_w; c += 1){
        g_grid[0][c] = (char)CHAR_DASH;
        g_grid[credits_h][c] = (char)CHAR_DASH;
    }
    for (int r = 1; r < credits_h; r += 1) {
        g_grid[r][right_x] = '|';
        g_grid[r][right_x + right_w - 1] = '|';
    }

    /* find active container */
    int active_c = 0;
    for (int i = 0; i < NUM_CONTAINERS; i += 1){
        if (elapsed_ms >= g_containers[i].start_ms) {
            active_c = i;
        }
    }

    const lyric_container_t *c_ptr = &g_containers[active_c];
    uint32_t rel_ms = elapsed_ms - (*c_ptr).start_ms;

    /*
     * toward the end of the song the terminal enters celebrate mode,
     * shuffling random aperture science ascii illustrations at an
     * accelerating rate until the track concludes.
     */
    if ((*c_ptr).is_celebrate){
        if (g_next_shuffle_ms == 0) {
            g_next_shuffle_ms = elapsed_ms + g_shuffle_interval_ms;
            g_current_art_id = rand() % NUM_ASCII_ARTS;
        } else if (elapsed_ms >= g_next_shuffle_ms){
            g_current_art_id = rand() % NUM_ASCII_ARTS;
            if (g_getting_faster) {
                if (g_shuffle_interval_ms > 89){
                    g_shuffle_interval_ms -= 39;
                } else {
                    g_shuffle_interval_ms = 50;
                }
            }
            g_next_shuffle_ms = elapsed_ms + g_shuffle_interval_ms;
        }
    } else {
        for (int i = 0; i < (*c_ptr).item_count; i += 1){
            const lyric_item_t *item = &(*c_ptr).items[i];
            if (!(*item).is_br && rel_ms >= (*item).start_ms) {
                if ((*item).art_id != ART_NONE){
                    g_current_art_id = (*item).art_id;
                }
                if ((*item).is_play_game) {
                    g_getting_faster = true;
                }
            }
        }
    }

    /*
     * typewriter calculation for the lyrics pane on the left.
     * text strings are revealed progressively according to their item duration.
     */
    int max_l_rows = frame_h - 2;
    if (max_l_rows > 64){
        max_l_rows = 64;
    }
    int max_l_cols = left_w - 3;
    if (max_l_cols > 120) {
        max_l_cols = 120;
    }

    char l_lines[64][128];
    int l_line_lens[64];
    for (int i = 0; i < 64; i += 1){
        l_lines[i][0] = '\0';
        l_line_lens[i] = 0;
    }

    int cur_r = 0;
    int cur_c = 0;

    for (int i = 0; i < (*c_ptr).item_count; i += 1) {
        const lyric_item_t *item = &(*c_ptr).items[i];
        if ((*item).is_br){
            if (rel_ms >= (*item).start_ms) {
                if (cur_r < max_l_rows - 1){
                    cur_r += 1;
                    cur_c = 0;
                }
            }
        } else {
            if (rel_ms >= (*item).start_ms){
                const char *txt = (*item).text;
                int len = (int)strlen(txt);
                int total_steps = len + ((*item).append_br ? 1 : 0);
                double step_dur = total_steps == 0 ? 0.0 : (double)(*item).dur_ms / (double)total_steps;
                uint32_t dt = rel_ms - (*item).start_ms;
                int steps = step_dur <= 0.0 ? total_steps : (int)((double)dt / step_dur);
                int num_chars = steps < len ? steps : len;

                for (int ch_idx = 0; ch_idx < num_chars; ch_idx += 1){
                    if (cur_c < max_l_cols) {
                        l_lines[cur_r][cur_c] = txt[ch_idx];
                        cur_c += 1;
                        l_lines[cur_r][cur_c] = '\0';
                        l_line_lens[cur_r] = cur_c;
                    }
                }

                if ((*item).append_br && steps >= len){
                    if (cur_r < max_l_rows - 1) {
                        cur_r += 1;
                        cur_c = 0;
                    }
                } else if (!(*item).append_br && steps >= len && (*item).add_space) {
                    if (cur_c < max_l_cols){
                        l_lines[cur_r][cur_c] = ' ';
                        cur_c += 1;
                        l_lines[cur_r][cur_c] = '\0';
                        l_line_lens[cur_r] = cur_c;
                    }
                }
            }
        }
    }

    /* copy lyrics lines into frame buffer with space padding */
    for (int r = 0; r <= cur_r && r < max_l_rows; r += 1){
        for (int c = 0; c < l_line_lens[r] && c < max_l_cols; c += 1) {
            g_grid[r + 1][c + 2] = l_lines[r][c];
        }
    }

    /* blinking underscore cursor */
    bool blink_on = ((elapsed_ms / BLINK_MS) % 2) == 0;
    if (blink_on && cur_r < max_l_rows && cur_c < max_l_cols){
        g_grid[cur_r + 1][cur_c + 2] = '_';
    }

    /*
     * the credits roll on the top right.
     * past lines scroll upward while the latest credit types out at the bottom.
     */
    int credit_cur = 0;
    int c_rows = credits_h - 1;
    if (elapsed_ms >= CREDITS_START_MS) {
        while (credit_cur < NUM_CREDITS - 1 && elapsed_ms >= g_credit_starts[credit_cur + 1]){
            credit_cur += 1;
        }

        for (int row_idx = 0; row_idx < c_rows - 1; row_idx += 1) {
            int hist_k = credit_cur - ((c_rows - 1) - row_idx);
            if (hist_k >= 0 && hist_k < NUM_CREDITS){
                const char *ctext = g_credits[hist_k];
                int clen = (int)strlen(ctext);
                for (int c = 0; c < clen && (c + 2) < (right_w - 1); c += 1) {
                    g_grid[row_idx + 1][right_x + 2 + c] = ctext[c];
                }
            }
        }

        const char *act_text = g_credits[credit_cur];
        int act_len = (int)strlen(act_text);
        uint32_t dt = elapsed_ms - g_credit_starts[credit_cur];
        double c_step_dur = act_len == 0 ? (double)g_credit_durs[credit_cur] : (double)g_credit_durs[credit_cur] / (double)act_len;
        int act_chars = c_step_dur <= 0.0 ? act_len : (int)((double)dt / c_step_dur);
        if (act_chars > act_len){
            act_chars = act_len;
        }

        for (int c = 0; c < act_chars && (c + 2) < (right_w - 1); c += 1) {
            g_grid[c_rows][right_x + 2 + c] = act_text[c];
        }
        if (blink_on && (act_chars + 2) < (right_w - 1)){
            g_grid[c_rows][right_x + 2 + act_chars] = '_';
        }
    } else {
        if (blink_on) {
            g_grid[c_rows][right_x + 2] = '_';
        }
    }

    /*
     * ascii art sits inside the lower right compartment.
     * we calculate centering offsets horizontally and vertically
     * so drawings look well balanced regardless of window size.
     */
    if (g_current_art_id >= 0 && g_current_art_id < NUM_ASCII_ARTS){
        const ascii_art_t *art = &g_ascii_arts[g_current_art_id];
        int art_avail_h = (frame_h - 1) - (credits_h + 1);
        int art_max_w = 0;
        for (int r = 0; r < (*art).line_count; r += 1) {
            int len = (int)strlen((*art).lines[r]);
            if (len > art_max_w){
                art_max_w = len;
            }
        }
        int art_pad_y = 0;
        if (art_avail_h > (*art).line_count) {
            art_pad_y = (art_avail_h - (*art).line_count) / 2;
        }
        int art_pad_x = 2;
        if (right_w > art_max_w){
            art_pad_x = (right_w - art_max_w) / 2;
        }
        for (int r = 0; r < (*art).line_count && (art_pad_y + r) < art_avail_h; r += 1) {
            const char *aline = (*art).lines[r];
            int alen = (int)strlen(aline);
            int dest_r = credits_h + 1 + art_pad_y + r;
            if (dest_r >= frame_h - 1){
                break;
            }
            for (int c = 0; c < alen && (art_pad_x + c) < (right_w - 1); c += 1) {
                g_grid[dest_r][right_x + art_pad_x + c] = aline[c];
            }
        }
    }

    /* center the rendered frame on the screen */
    int off_x = 0;
    int off_y = 0;
    if (term_cols > frame_w){
        off_x = (term_cols - frame_w) / 2;
    }
    if (term_rows > frame_h) {
        off_y = (term_rows - frame_h) / 2;
    }

    static char out_buf[262144];
    int pos = 0;

    if (needs_clear){
        pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "\033[?2025h\033[2J\033[H\033[38;2;222;190;95m");
    } else {
        pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "\033[?2025h\033[38;2;222;190;95m");
    }

    /*
     * direct cursor positioning per line without newline characters.
     * positioning with target row and column avoids redrawing blanks
     * and avoids the line erasing escape that causes terminal tearing.
     */
    for (int r = 0; r < frame_h; r += 1) {
        int target_row = off_y + r + 1;
        if (target_row > term_rows) {
            break;
        }
        if (off_x > 0){
            pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "\033[%d;%dH%s", target_row, off_x + 1, g_grid[r]);
        } else {
            pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "\033[%d;1H%s", target_row, g_grid[r]);
        }
    }

    pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "\033[?2025l");

    if (pos > 0){
        write_all(STDOUT_FILENO, out_buf, (size_t)pos);
    }
}

int main(int argc, char *argv[]) {
    bool autoplay = false;
    bool no_audio = false;
    uint64_t initial_offset_ms = 0;

    for (int i = 1; i < argc; i += 1){
        if (strcmp(argv[i], "autoplay") == 0 || strcmp(argv[i], "\055a") == 0 || strcmp(argv[i], "\055\055autoplay") == 0) {
            autoplay = true;
        } else if (strcmp(argv[i], "silent") == 0 || strcmp(argv[i], "no\055audio") == 0 || strcmp(argv[i], "\055s") == 0){
            no_audio = true;
        } else if (strncmp(argv[i], "start=", 6) == 0) {
            initial_offset_ms = (uint64_t)strtoull(argv[i] + 6, NULL, 10);
            autoplay = true;
        } else if (strcmp(argv[i], "classic") == 0){
            g_scale_mode = SCALE_CLASSIC;
        } else if (strcmp(argv[i], "fill") == 0) {
            g_scale_mode = SCALE_FILL;
        } else if (strcmp(argv[i], "aspect") == 0 || strcmp(argv[i], "fit") == 0){
            g_scale_mode = SCALE_ASPECT;
        } else if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "\055h") == 0 || strcmp(argv[i], "\055\055help") == 0) {
            printf("usage: %s [autoplay] [silent] [start=<ms>] [aspect|fill|classic]\n", argv[0]);
            printf("controls:\n");
            printf("  space : pause / resume\n");
            printf("  s     : cycle scaling mode (aspect / fill / classic)\n");
            printf("  r     : restart\n");
            printf("  q/esc : quit\n");
            return 0;
        }
    }

    srand((unsigned int)time(NULL));
    init_credits_timeline();

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGWINCH, handle_winch);

    atexit(restore_terminal);
    enable_raw_mode();

    printf("\033[?1049h\033[?25l\033[?7l\033[2J");
    fflush(stdout);

    const char *audio_path = find_audio_file();

    /* initial boot screen */
    if (!autoplay){
        printf("\033[H\033[38;2;222;190;95m");
        printf("\n");
        printf(" Loading Aperture Science Genetic Lifeform and Disk Operating System Singing module...\n\n");
        printf(" FULL SCREEN RECOMMENDED!\n\n");
        printf(" [ Press SPACE or ENTER to begin ]\n");
        printf(" [ Press 's' to cycle scaling mode ]\n");
        printf(" [ Press 'q' to quit ]\n");
        fflush(stdout);

        while (g_running) {
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) > 0){
                if (ch == 'q' || ch == 'Q' || ch == 27) {
                    return 0;
                }
                if (ch == 's' || ch == 'S'){
                    g_scale_mode = (g_scale_mode + 1) % SCALE_MAX;
                }
                if (ch == ' ' || ch == '\n' || ch == '\r') {
                    break;
                }
            }
            usleep(15000);
        }
    }

    printf("\033[2J");
    fflush(stdout);
    g_winch_flag = 1;

    uint64_t start_time = get_time_ms() - initial_offset_ms;
    uint64_t pause_start = 0;
    bool audio_started = false;

    while (g_running){
        /* check user input */
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == 'q' || ch == 'Q' || ch == 27){
                break;
            } else if (ch == 's' || ch == 'S') {
                g_scale_mode = (g_scale_mode + 1) % SCALE_MAX;
                g_winch_flag = 1;
            } else if (ch == ' '){
                g_paused = !g_paused;
                if (g_paused) {
                    pause_start = get_time_ms();
                    if (g_audio_pid > 0){
                        kill(g_audio_pid, SIGSTOP);
                    }
                } else {
                    uint64_t p_dur = get_time_ms() - pause_start;
                    start_time += p_dur;
                    if (g_audio_pid > 0) {
                        kill(g_audio_pid, SIGCONT);
                    }
                }
            } else if (ch == 'r' || ch == 'R'){
                if (g_audio_pid > 0) {
                    kill(g_audio_pid, SIGTERM);
                    waitpid(g_audio_pid, NULL, WNOHANG);
                    g_audio_pid = 0;
                }
                start_time = get_time_ms();
                audio_started = false;
                g_current_art_id = ART_NONE;
                g_getting_faster = false;
                g_next_shuffle_ms = 0;
                g_shuffle_interval_ms = 5000;
                g_paused = false;
                g_winch_flag = 1;
            }
        }

        if (!g_paused) {
            uint64_t now = get_time_ms();
            uint32_t elapsed = (uint32_t)(now - start_time);

            /* launch audio at exact sync timestamp */
            if (!no_audio && !audio_started && elapsed >= AUDIO_START_MS){
                double seek_s = 0.0;
                if (elapsed > AUDIO_START_MS) {
                    seek_s = (double)(elapsed - AUDIO_START_MS) / 1000.0;
                }
                g_audio_pid = launch_audio_player(audio_path, seek_s);
                audio_started = true;
            }

            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0){
                ws.ws_row = 40;
                ws.ws_col = 100;
            }

            render_screen(elapsed, ws.ws_row, ws.ws_col);
        }

        usleep(16000);
    }

    return 0; //  finally
}
