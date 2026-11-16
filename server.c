/*
 * server.c - Tic-Tac-Toe TCP server
 *
 * Accepts two clients, assigns X to the first and O to the second, keeps the
 * authoritative board, validates moves, enforces turns, and announces the
 * result. All game logic lives here; clients only display and forward input.
 *
 * Build: gcc -std=c11 -Wall -Wextra -pedantic server.c -o server
 * Run:   ./server <port>
 *
 * ---------------------------------------------------------------------------
 * PROTOCOL (plain ASCII text, every message ends with '\n')
 *
 * Client -> Server
 *   MOVE <n>        place my symbol on square n (1-9)
 *   QUIT            leave the game
 *
 * Server -> Client
 *   WELCOME <X|O>   your assigned symbol
 *   INFO <text>     informational text to print
 *   BOARD <9 chars> board row by row, 'X', 'O' or '-' for empty
 *   TURN <X|O>      whose turn it is
 *   YOURTURN        prompt the user for a move now
 *   INVALID <text>  the last move was rejected (turn does not advance)
 *   WIN <X|O>       game over, that symbol won
 *   DRAW            game over, board full with no winner
 *   OPPONENT_LEFT   game over, the other player quit or disconnected
 *   BYE             acknowledgement of QUIT
 *
 * Because TCP is a byte stream, each side keeps a receive buffer and only
 * acts on a message once a complete '\n'-terminated line has arrived.
 * ---------------------------------------------------------------------------
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SIZE      3
#define BUF_SIZE  512
#define LINE_LEN  256
#define EMPTY     ' '

typedef struct {
    int    fd;              /* socket, -1 when not connected      */
    char   symbol;          /* 'X' or 'O'                          */
    char   buf[BUF_SIZE];   /* bytes received but not yet consumed */
    size_t len;             /* number of bytes in buf              */
} Player;

/* ======================= Board / game rules ============================ */

static void init_board(char board[SIZE][SIZE])
{
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            board[r][c] = EMPTY;
}

/* Encode the board as 9 characters for the BOARD message. */
static void board_to_string(char board[SIZE][SIZE], char out[SIZE * SIZE + 1])
{
    int k = 0;
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            out[k++] = (board[r][c] == EMPTY) ? '-' : board[r][c];
    out[k] = '\0';
}

/* Print the board on the server console (for logging). */
static void print_board(char board[SIZE][SIZE])
{
    for (int r = 0; r < SIZE; r++) {
        printf(" ");
        for (int c = 0; c < SIZE; c++) {
            char ch = board[r][c];
            if (ch == EMPTY)
                ch = (char)('1' + r * SIZE + c);
            printf(" %c %s", ch, c < SIZE - 1 ? "|" : "");
        }
        printf("\n");
        if (r < SIZE - 1)
            printf("  ---+---+---\n");
    }
    fflush(stdout);
}

/*
 * Parse the argument of a MOVE command. Returns the square 1-9, or 0 if the
 * text is not a whole number in that range.
 */
static int parse_square(const char *text)
{
    char *end;
    long  n;

    while (*text == ' ')
        text++;
    if (*text == '\0')
        return 0;

    errno = 0;
    n = strtol(text, &end, 10);
    while (*end == ' ')
        end++;
    if (errno != 0 || *end != '\0' || n < 1 || n > 9)
        return 0;
    return (int)n;
}

static int is_occupied(char board[SIZE][SIZE], int square)
{
    int idx = square - 1;
    return board[idx / SIZE][idx % SIZE] != EMPTY;
}

static void place_symbol(char board[SIZE][SIZE], int square, char symbol)
{
    int idx = square - 1;
    board[idx / SIZE][idx % SIZE] = symbol;
}

/* Returns 'X' or 'O' if that symbol has three in a row, otherwise 0. */
static char check_winner(char board[SIZE][SIZE])
{
    for (int i = 0; i < SIZE; i++) {
        /* row i */
        if (board[i][0] != EMPTY &&
            board[i][0] == board[i][1] && board[i][1] == board[i][2])
            return board[i][0];
        /* column i */
        if (board[0][i] != EMPTY &&
            board[0][i] == board[1][i] && board[1][i] == board[2][i])
            return board[0][i];
    }
    /* diagonals */
    if (board[1][1] != EMPTY &&
        ((board[0][0] == board[1][1] && board[1][1] == board[2][2]) ||
         (board[0][2] == board[1][1] && board[1][1] == board[2][0])))
        return board[1][1];
    return 0;
}

static int board_full(char board[SIZE][SIZE])
{
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            if (board[r][c] == EMPTY)
                return 0;
    return 1;
}

/* ========================= Networking helpers ========================== */

/* send() may write fewer bytes than asked; loop until everything is sent. */
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

/* printf-style: format one protocol message and append the '\n'. */
static int send_msg(int fd, const char *fmt, ...)
{
    char    line[LINE_LEN];
    va_list ap;
    int     n;

    if (fd < 0)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n > sizeof line - 2)
        n = (int)(sizeof line - 2);
    line[n++] = '\n';
    return send_all(fd, line, (size_t)n);
}

/* Read whatever bytes are available into the player's buffer.
 * Returns >0 bytes read, 0 if the peer closed, -1 on error. */
static ssize_t recv_into(Player *p)
{
    ssize_t n;

    if (p->len == sizeof p->buf) {
        /* A "line" longer than the buffer: discard it. */
        p->len = 0;
        send_msg(p->fd, "INVALID Message too long");
    }
    do {
        n = recv(p->fd, p->buf + p->len, sizeof p->buf - p->len, 0);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        p->len += (size_t)n;
    return n;
}

/* If a full line is buffered, copy it (without "\r\n") into out and
 * return 1. Otherwise return 0 and keep the partial data for later. */
static int next_line(Player *p, char *out, size_t out_size)
{
    char  *nl = memchr(p->buf, '\n', p->len);
    size_t line_len, copy;

    if (nl == NULL)
        return 0;

    line_len = (size_t)(nl - p->buf);
    copy = line_len < out_size - 1 ? line_len : out_size - 1;
    memcpy(out, p->buf, copy);
    out[copy] = '\0';
    if (copy > 0 && out[copy - 1] == '\r')
        out[copy - 1] = '\0';

    /* shift remaining bytes to the front of the buffer */
    p->len -= line_len + 1;
    memmove(p->buf, nl + 1, p->len);
    return 1;
}

static int create_listener(int port)
{
    struct sockaddr_in addr;
    int fd, yes = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 2) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void close_player(Player *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd  = -1;
    p->len = 0;
}

/*
 * Wait until two players are connected. While only X is connected we also
 * watch X's socket, so if X leaves before O arrives the slot is freed and
 * the next client to connect becomes X.
 */
static int wait_for_players(int listen_fd, Player players[2])
{
    int  count = 0;
    char line[LINE_LEN];

    while (count < 2) {
        fd_set rfds;
        int    maxfd = listen_fd;

        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        if (count == 1) {
            FD_SET(players[0].fd, &rfds);
            if (players[0].fd > maxfd)
                maxfd = players[0].fd;
        }
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return -1;
        }

        /* Did the waiting X player send something or disconnect? */
        if (count == 1 && FD_ISSET(players[0].fd, &rfds)) {
            int left = 0;
            if (recv_into(&players[0]) <= 0) {
                left = 1;
            } else {
                while (next_line(&players[0], line, sizeof line)) {
                    if (strcmp(line, "QUIT") == 0) {
                        send_msg(players[0].fd, "BYE");
                        left = 1;
                        break;
                    }
                    send_msg(players[0].fd,
                             "INFO The game has not started yet");
                }
            }
            if (left) {
                printf("Player X left before the game started. "
                       "Waiting for a new player...\n");
                close_player(&players[0]);
                count = 0;
                continue;
            }
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof caddr;
            char ip[INET_ADDRSTRLEN];
            int  fd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);

            if (fd < 0) {
                if (errno != EINTR)
                    perror("accept");
                continue;
            }
            players[count].fd     = fd;
            players[count].len    = 0;
            players[count].symbol = (count == 0) ? 'X' : 'O';

            inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof ip);
            printf("Player %c connected from %s:%d\n", players[count].symbol,
                   ip, ntohs(caddr.sin_port));

            send_msg(fd, "WELCOME %c", players[count].symbol);
            if (count == 0)
                send_msg(fd, "INFO Waiting for an opponent to connect...");
            count++;
        }
    }
    return 0;
}

/* ============================ Game flow ================================ */

static void broadcast_board(Player players[2], char board[SIZE][SIZE])
{
    char s[SIZE * SIZE + 1];
    board_to_string(board, s);
    for (int i = 0; i < 2; i++)
        send_msg(players[i].fd, "BOARD %s", s);
}

/* Tell both players whose turn it is and prompt the current player. */
static void announce_turn(Player players[2], int current)
{
    for (int i = 0; i < 2; i++)
        send_msg(players[i].fd, "TURN %c", players[current].symbol);
    send_msg(players[current].fd, "YOURTURN");
    printf("Turn: %c\n", players[current].symbol);
    fflush(stdout);
}

/* Player `who` quit or disconnected: notify the other one. */
static void player_left(Player players[2], int who)
{
    printf("Player %c left the game.\n", players[who].symbol);
    send_msg(players[1 - who].fd, "OPPONENT_LEFT");
}

/*
 * Handle one complete command line from player `who`.
 * Returns 1 if the game is over, 0 to keep playing.
 */
static int handle_command(Player players[2], int who, const char *line,
                          char board[SIZE][SIZE], int *current)
{
    Player *p = &players[who];

    if (strcmp(line, "QUIT") == 0) {
        send_msg(p->fd, "BYE");
        player_left(players, who);
        return 1;
    }

    if (strncmp(line, "MOVE", 4) != 0 || (line[4] != ' ' && line[4] != '\0')) {
        send_msg(p->fd, "INVALID Unknown command");
        if (who == *current)
            send_msg(p->fd, "YOURTURN");
        return 0;
    }

    if (who != *current) {
        send_msg(p->fd, "INVALID It is not your turn");
        return 0;
    }

    int square = parse_square(line + 4);
    if (square == 0) {
        printf("Rejected move from %c: \"%s\" (not a number 1-9)\n",
               p->symbol, line);
        send_msg(p->fd, "INVALID Square must be a number from 1 to 9");
        send_msg(p->fd, "YOURTURN");
        return 0;
    }
    if (is_occupied(board, square)) {
        printf("Rejected move from %c: square %d occupied\n", p->symbol, square);
        send_msg(p->fd, "INVALID Square %d is already taken", square);
        send_msg(p->fd, "YOURTURN");
        return 0;
    }

    /* Legal move. */
    place_symbol(board, square, p->symbol);
    printf("%c plays square %d\n", p->symbol, square);
    print_board(board);
    broadcast_board(players, board);

    char winner = check_winner(board);
    if (winner) {
        printf("Result: %c wins!\n", winner);
        for (int i = 0; i < 2; i++)
            send_msg(players[i].fd, "WIN %c", winner);
        return 1;
    }
    if (board_full(board)) {
        printf("Result: draw.\n");
        for (int i = 0; i < 2; i++)
            send_msg(players[i].fd, "DRAW");
        return 1;
    }

    *current = 1 - *current;
    announce_turn(players, *current);
    return 0;
}

/* Main loop: watch both sockets so a disconnect is noticed at any time. */
static void play_game(Player players[2])
{
    char board[SIZE][SIZE];
    char line[LINE_LEN];
    int  current = 0;   /* X moves first */
    int  over = 0;

    init_board(board);
    for (int i = 0; i < 2; i++)
        send_msg(players[i].fd, "INFO Both players connected. X goes first.");
    printf("Game started.\n");
    print_board(board);
    broadcast_board(players, board);
    announce_turn(players, current);

    while (!over) {
        fd_set rfds;
        int    maxfd = players[0].fd > players[1].fd ? players[0].fd
                                                     : players[1].fd;
        FD_ZERO(&rfds);
        FD_SET(players[0].fd, &rfds);
        FD_SET(players[1].fd, &rfds);

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return;
        }

        for (int i = 0; i < 2 && !over; i++) {
            if (!FD_ISSET(players[i].fd, &rfds))
                continue;
            if (recv_into(&players[i]) <= 0) {
                player_left(players, i);
                over = 1;
                break;
            }
            while (!over && next_line(&players[i], line, sizeof line))
                over = handle_command(players, i, line, board, &current);
        }
    }
}

int main(int argc, char *argv[])
{
    Player players[2] = { { -1, 'X', {0}, 0 }, { -1, 'O', {0}, 0 } };
    char  *end;
    long   port;
    int    listen_fd;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    port = strtol(argv[1], &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Writing to a closed socket must return an error, not kill us. */
    signal(SIGPIPE, SIG_IGN);

    listen_fd = create_listener((int)port);
    if (listen_fd < 0)
        return EXIT_FAILURE;
    printf("Tic-Tac-Toe server listening on port %ld\n", port);
    fflush(stdout);

    if (wait_for_players(listen_fd, players) == 0) {
        close(listen_fd);   /* one match per run: stop accepting */
        listen_fd = -1;
        play_game(players);
    }

    close_player(&players[0]);
    close_player(&players[1]);
    if (listen_fd >= 0)
        close(listen_fd);
    printf("Server shutting down.\n");
    return EXIT_SUCCESS;
}
