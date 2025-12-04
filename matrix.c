#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

#define ASCII_CHARS          "0123456789*+-<=>|"
// clear screen, reset cursor, show cursor, flip to main buffer
#define ANSI_EXIT_CLEANUP    "\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l"
// flip to alternate buffer, clear screen, reset cursor, hide cursor
#define ANSI_SETUP_CONSOLE   "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l"
#define ANSI_RESET_CURSOR    "\x1b[H"
#define ANSI_SET_COLOUR_RGB  "\x1b[38;2;%d;%d;%dm"
#define ANSI_RESET_COLOUR    "\x1b[39m"

// ==========- OPTIONS -==========

// speed of trails falling
// (framerate dependant)
#define SPEED            0.5
#define FPS              30.0

// min and max trail length
#define MIN_TRAIL        14
#define MAX_TRAIL        20

// chance of trail spawning for any
// "free slot" in the array of trails
#define INIT_CHANCE      1E-3

// size of the array of trails
#define MAX_NUM_TRAILS   1024

// ===============================


// character trail
typedef struct char_trail {
    // top left is 0,0 increasing as you go down or right
    float    counter;                // used for speed control
    uint32_t x;                      // screen y value of the head of the trail.
    uint32_t y;                      // screen x value of trail
    uint32_t length;                 // length of trail, converted to bytes
    uint32_t characters[MAX_TRAIL];  // list of characters in trail
} char_trail;


// rgb colour ¯\_(ツ)_/¯
typedef struct rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb;


typedef struct screen_size {
    int screen_width;
    int screen_height;
} screen_size;


#ifdef _WIN32
    #include <windows.h>
    #include <stdio.h>

    // sets up ANSI control codes on windows & sets the output to
    // utf-8 rather than utf-16LE
    int win_fixes() {
        SetConsoleOutputCP(CP_UTF8);
        HANDLE h_console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD console_mode;
        if (!GetConsoleMode(h_console, &console_mode)) {
            return 1;
        }
        else {
            console_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (!SetConsoleMode(h_console, console_mode)) {
                return 1;
            }
        }
        return 0;
    }

    // gets the current terminal size. sets it to 80x24 if malformed or nulled
    screen_size get_screen_size() {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        int width, height;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        width  = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (width <= 0) { width = 80; }
        if (height <= 0) { height = 24; }
        screen_size current_size = {width, height};
        return current_size;
    }
#else
    // ioctl is supported on linux, mac & bsd - assume it works everywhere
    #include <sys/ioctl.h>
    // gets the current terminal size. sets it to 80x24 if malformed or nulled
    screen_size get_screen_size() {
        struct winsize size;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
        int width = size.ws_col;
        int height = size.ws_row;
        if (width <= 0) { width = 80; }
        if (height <= 0) { height = 24; }
        screen_size current_size = {width, height};
        return current_size;
    }
#endif


// write utf codepoint to utf8 string buffer
// does not write null terminator
// returns number of bytes written
int write_utf8_buf(const size_t max_output_length, char* output, const size_t input_length, const uint32_t* input) {
    size_t bytes_written = 0;

    for (size_t i = 0; i < input_length; ++i) {
        if (input[i] < 0x80) {
            if (bytes_written >= max_output_length - 1) { return bytes_written; }

            output[bytes_written]     = (char)(((input[i])       & 0x7F) | 0x00);
            bytes_written += 1;
        }
        else if (input[i] < 0x800) {
            if (bytes_written >= max_output_length - 2) { return bytes_written; }

            output[bytes_written]     = (char)(((input[i] >> 6)  & 0x1F) | 0xC0);
            output[bytes_written + 1] = (char)(((input[i])       & 0x3F) | 0x80);
            bytes_written += 2;
        }
        else if (input[i] < 0x11000) {
            if (bytes_written >= max_output_length - 3) { return bytes_written; }

            output[bytes_written]     = (char)(((input[i] >> 12) & 0x0F) | 0xE0);
            output[bytes_written + 1] = (char)(((input[i] >> 6)  & 0x3F) | 0x80);
            output[bytes_written + 2] = (char)(((input[i])       & 0x3F) | 0x80);
            bytes_written += 3;
        }
        else {
            if (bytes_written >= max_output_length - 4) { return bytes_written; }
            output[bytes_written]     = (char)(((input[i] >> 18) & 0x07) | 0xF0);
            output[bytes_written + 1] = (char)(((input[i] >> 12) & 0x3F) | 0x80);
            output[bytes_written + 2] = (char)(((input[i] >> 6)  & 0x3F) | 0x80);
            output[bytes_written + 3] = (char)(((input[i])       & 0x3F) | 0x80);
            bytes_written += 4;
        }
    }
    return (int)bytes_written;
}


// randomly generate either ascii or half-width katakana
uint32_t generate_random_char() {
    if ((rand() % 5) <= 3) {  // 3/5 chance of generating a half width katakana
        // 0xFF60 -> half width katakana plane
        // we want characters FF66 - FF9D, so we mod by 0x38 and add 0x06
        return (uint32_t)(0xff60 + (rand() % 0x38) + 0x06);
    }
    // otherwise return a random ascii character from a preselected set
    return (uint32_t)ASCII_CHARS[rand() % (sizeof(ASCII_CHARS) - 1)];
}


// create character trail object
char_trail* create_trail(uint32_t x) {
    char_trail* trail = (char_trail*)malloc(sizeof(char_trail));
    trail->counter = 0.0;
    trail->length = (rand() % (MAX_TRAIL - MIN_TRAIL)) + MIN_TRAIL;
    trail->x = x;
    trail->y = -trail->length;
    for (uint32_t i = 0; i < trail->length; ++i) {
        trail->characters[i] = generate_random_char();
    }
    return trail;
}


// calculate colour for given character in a trail
rgb calc_colour_from_pos(const uint32_t index, const uint32_t length) {
    if (index == 0) {
        const rgb colour = {200, 200, 200};
        return colour;
    }
    const double coefficient = 1.0 - ((float)index / ((float)length * 1.1));
    const rgb colour = {(uint8_t)(40.0  * coefficient),
                        (uint8_t)(255.0 * coefficient),
                        (uint8_t)(40.0  * coefficient)};
    return colour;
}


// update trail (iterate position, move characters)
void update_trail(char_trail* trail) {
    trail->y += 1.0;
    memmove(trail->characters + 1, trail->characters, (trail->length - 1) * sizeof(trail->characters[0]));
    trail->characters[0] = generate_random_char();
}


// write trail to intermediate buffer
void write_trail(char_trail* trail, char* buffer, int width, int height) {
    for (uint32_t j = 0; j < (trail->length - 1); j++) {
        // calculates index into the intermedaite buffer
        uint32_t index = (trail->x + ((trail->y - j) * width)) * 32;

        // as index is unsigned, it's always > 0, so no lower bound check neededc
        if (index > (uint32_t)((width * height * 32)-32)) {
            continue; // skip if out of bounds
        }

        // clearing prev write if overwriting
        memset(&(buffer[index]), 0, 32);

        // calculating and writing colour as ANSI control code
        rgb colour = calc_colour_from_pos(j, trail->length);
        int temp_offset = snprintf(&buffer[index], 24, ANSI_SET_COLOUR_RGB, colour.red, colour.green, colour.blue);

        // take character from codepoint form, write it into the buffer as utf8 string
        temp_offset += write_utf8_buf(32, &buffer[index + temp_offset], 1, &trail->characters[j]);
        snprintf(&buffer[index + temp_offset], 6, ANSI_RESET_COLOUR);
    }
}


void ctrlc_handler(int signal_num) {printf(ANSI_EXIT_CLEANUP); (void)signal_num; exit(0); }


int main() {
    #ifdef _WIN32
    // windows specific fixes for ANSI sequences, enabling utf-8 output
    if (win_fixes()) {
        printf(ANSI_EXIT_CLEANUP);
        printf("Not supported");
        exit(0);
    }
    #endif

    signal(SIGINT, ctrlc_handler);

    printf(ANSI_SETUP_CONSOLE);

    struct timeval frame_start, frame_end;
    char_trail* trails[MAX_NUM_TRAILS] = {};  // array storing the trails
    screen_size current_size = get_screen_size();
    int width = current_size.screen_width;
    int height = current_size.screen_height;

    // intermediate buffer
    // width x height of 32-byte strings
    // max size is only like ~1MB, all is fine
    char* buffer = (char*)malloc(width * height * 32);

    // screen buffer
    // enough to contain the max. possible length of everything
    // oversized, but mostly not an issue
    char* screen_buffer = (char*)malloc(width * height * 32);

    while (1) {
        gettimeofday(&frame_start, NULL);

        // new screen size
        current_size = get_screen_size();
        width = current_size.screen_width;
        height = current_size.screen_height;

        // reallocate buffers according to screen size
        screen_buffer = (char*)realloc(screen_buffer, width * height * 32);
        buffer = (char*)realloc(buffer, width * height * 32);

        // exit if buffers leak
        if (screen_buffer == NULL || buffer == NULL) {
            printf(ANSI_EXIT_CLEANUP);
            printf("realloc failed (%dx%d)", width, height);
            return 0;
        }

        // fill the screen buffer with spaces, clear out the intermediate buffer
        memset(screen_buffer, (char)0x20, width * height * 32);
        memset(buffer, 0, width * height * 32);

        // iterate through list of trails, create new instances if slots are empty,
        // otherwise iterate & write into intermediate buffer
        for (int i = 0; i < MAX_NUM_TRAILS; i++) {

            // spawning new trails in free slots
            if (trails[i] == NULL) {
                if (((float)rand() / (float)RAND_MAX) < INIT_CHANCE) {
                    trails[i] = create_trail(rand() % width);
                }
                // process next trail
                continue;
            }

            // update trails
            // the logic around update_trail is for speed control of the trails
            trails[i]->counter += SPEED;
            while (trails[i]->counter >= 1.0) {
                update_trail(trails[i]);
                trails[i]->counter -= 1.0;
            }

            // freeing trails that have gone offscreen
            if ((int)(trails[i]->y - trails[i]->length) > height) {
                free(trails[i]);
                trails[i] = NULL;
                // process next trail
            }

            // writing trails into the intermediate buffer
            else {
                write_trail(trails[i], buffer, width, height);
            }
        }

        // writing from the intermediate buffer into the screen buffer
        // as the intermediate buffer is composed of 32 byte wide , and utf-8 is not constant width,
        // we can iterate through "buffer", but must track the pointer to the byte being writen
        // in screen_buffer
        int ptr = 0;
        int num_characters = 0;
        for (int i = 0; i < (width * height); i++) {
            // if a string has been written to a slot
            if ((num_characters % (width)) == 0) {
                #ifndef _WIN32
                    screen_buffer[ptr] = '\n';
                    ptr++;
                #endif
            }

            if (buffer[i*32]) {
                // functionally equivalent to strncpy, but I need the # of bytes written afterward to increment
                // the pointer to the head of screen_buffer
                int tmp_ptr = 0;
                while (tmp_ptr < 32 && buffer[(i*32) + tmp_ptr] != 0) {
                    screen_buffer[ptr + tmp_ptr] = buffer[(i*32) + tmp_ptr];
                    tmp_ptr++;
                }
                ptr += tmp_ptr;
            }
            // skip empty intermediate buffer slots as the
            // screen buffer is filled with spaces by default
            else {
                ptr++;
            }
            num_characters++;
        }

        printf(ANSI_RESET_CURSOR);  // moves cursor to top-left of terminal
        fwrite(screen_buffer, 1, ptr, stdout);
        fflush(stdout);

        // target the fps defined at the top by waiting after frames that didn't take long
        gettimeofday(&frame_end, NULL);
        int64_t duration =  (frame_end.tv_sec - frame_start.tv_sec) * 1000000 + (frame_end.tv_usec - frame_start.tv_usec);
        duration = (1000000.0 / FPS) - duration;
        if (duration > 1024) {
            usleep(duration);
        }
    }
    return 0;
}
