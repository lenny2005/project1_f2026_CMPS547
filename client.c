/*
 * client.c - Tic-Tac-Toe TCP client
 *
 * Connects to the server, shows the assigned symbol, board and turn
 * information, and reads a move from the keyboard only when the server
 * sends YOURTURN. The client never decides whether a move is legal; it
 * forwards what the user typed and displays the server's response.
 *
 * Build: gcc -std=c11 -Wall -Wextra -pedantic client.c -o client
 * Run:   ./client <server IPv4> <port>
 *
 * See server.c for the full protocol description.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SIZE      3
#define BUF_SIZE  512
#define LINE_LEN  256

typedef struct {
    int    fd;
    char   buf[BUF_SIZE];
    size_t len;
} Connection;

/* ========================= Networking helpers ========================== */

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += n;
        len  -= (size_t)n;
    }
    return 0;
}

/* Send "<cmd>\n". */
static int send_line(int fd, const char *cmd)
{
    if (send_all(fd, cmd, strlen(cmd)) < 0)
        return -1;
    return send_all(fd, "\n", 1);
}

/*
 * Receive exactly one '\n'-terminated message. A single recv() may return
 * part of a message or several messages, so bytes are buffered in `conn`
 * until a newline is found. Returns 1 on success, 0 if the server closed
 * the connection, -1 on error.
 */
static int recv_line(Connection *conn, char *out, size_t out_size)
{
    for (;;) {
        char *nl = memchr(conn->buf, '\n', conn->len);
        if (nl != NULL) {
            size_t line_len = (size_t)(nl - conn->buf);
            size_t copy = line_len < out_size - 1 ? line_len : out_size - 1;

            memcpy(out, conn->buf, copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r')
                out[copy - 1] = '\0';
            conn->len -= line_len + 1;
            memmove(conn->buf, nl + 1, conn->len);
            return 1;
        }
        if (conn->len == sizeof conn->buf)  /* over-long line: drop it */
            conn->len = 0;

        ssize_t n = recv(conn->fd, conn->buf + conn->len,
                         sizeof conn->buf - conn->len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return (int)n;
        conn->len += (size_t)n;
    }
}

static int connect_to_server(const char *ip, const char *port_str)
{
    struct sockaddr_in addr;
    char *end;
    long  port = strtol(port_str, &end, 10);
    int   fd;

    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", port_str);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", ip);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* ============================ Display ================================== */

/* Board string is 9 chars ('X', 'O', '-'). Empty squares show their number. */
static void display_board(const char *s)
{
    printf("\n");
    for (int r = 0; r < SIZE; r++) {
        printf(" ");
        for (int c = 0; c < SIZE; c++) {
            int  idx = r * SIZE + c;
            char ch  = s[idx] == '-' ? (char)('1' + idx) : s[idx];
            printf(" %c %s", ch, c < SIZE - 1 ? "|" : "");
        }
        printf("\n");
        if (r < SIZE - 1)
            printf("  ---+---+---\n");
    }
    printf("\n");
}

/* Remove leading/trailing whitespace in place. */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' '  || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/*
 * Prompt the user and send the move to the server. Anything typed while it
 * was not this player's turn is discarded first, so input is only read when
 * prompted. Returns 0 normally, 1 if the user chose to quit.
 */
static int prompt_and_send_move(int fd)
{
    char input[64];
    char msg[96];
    char *move;

    if (isatty(STDIN_FILENO))
        tcflush(STDIN_FILENO, TCIFLUSH);

    printf("Your move - enter a square (1-9) or q to quit: ");
    fflush(stdout);

    if (fgets(input, sizeof input, stdin) == NULL) {
        send_line(fd, "QUIT");
        return 1;
    }
    /* discard the rest of an over-long input line */
    if (strchr(input, '\n') == NULL) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF)
            ;
    }

    move = trim(input);
    if (strcmp(move, "q") == 0 || strcmp(move, "Q") == 0 ||
        strcmp(move, "quit") == 0) {
        send_line(fd, "QUIT");
        return 1;
    }

    /* The server decides whether this is valid. */
    snprintf(msg, sizeof msg, "MOVE %s", move);
    send_line(fd, msg);
    return 0;
}

/* ============================== Main =================================== */

int main(int argc, char *argv[])
{
    Connection conn;
    char line[LINE_LEN];
    char my_symbol = '?';
    int  running = 1;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server IPv4> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    conn.fd  = connect_to_server(argv[1], argv[2]);
    conn.len = 0;
    if (conn.fd < 0)
        return EXIT_FAILURE;
    printf("Connected to %s:%s\n", argv[1], argv[2]);

    while (running) {
        int r = recv_line(&conn, line, sizeof line);
        if (r <= 0) {
            printf("\nDisconnected from server.\n");
            break;
        }

        if (strncmp(line, "WELCOME ", 8) == 0) {
            my_symbol = line[8];
            printf("You are player %c.\n", my_symbol);
        } else if (strncmp(line, "INFO ", 5) == 0) {
            printf("%s\n", line + 5);
        } else if (strncmp(line, "BOARD ", 6) == 0 && strlen(line + 6) == 9) {
            display_board(line + 6);
        } else if (strncmp(line, "TURN ", 5) == 0) {
            if (line[5] != my_symbol)
                printf("Waiting for player %c to move...\n", line[5]);
        } else if (strcmp(line, "YOURTURN") == 0) {
            if (prompt_and_send_move(conn.fd))
                printf("You quit the game.\n");
        } else if (strncmp(line, "INVALID ", 8) == 0) {
            printf("Invalid move: %s\n", line + 8);
        } else if (strncmp(line, "WIN ", 4) == 0) {
            if (line[4] == my_symbol)
                printf("*** You win! ***\n");
            else
                printf("*** Player %c wins. You lose. ***\n", line[4]);
            running = 0;
        } else if (strcmp(line, "DRAW") == 0) {
            printf("*** It's a draw! ***\n");
            running = 0;
        } else if (strcmp(line, "OPPONENT_LEFT") == 0) {
            printf("Your opponent disconnected or quit. Game over.\n");
            running = 0;
        } else if (strcmp(line, "BYE") == 0) {
            running = 0;
        } else {
            printf("[unknown message from server: %s]\n", line);
        }
        fflush(stdout);
    }

    close(conn.fd);
    return EXIT_SUCCESS;
}
