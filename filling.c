/* -*- tab-width: 8; indent-tabs-mode: t -*-
 * filling.c: An implementation of the Nikoli game fillomino.
 * Copyright (C) 2007 Jonas K�lker.  See LICENSE for the license.
 */

/* TODO:
 *
 *  - use a typedef instead of int for numbers on the board
 *     + replace int with something else (signed char?)
 *        - the type should be signed (I use -board[i] temporarily)
 *        - problems are small (<= 9?): type can be char?
 *
 *  - make a somewhat more clever solver
 *
 *  - make the solver do recursion/backtracking.
 *     + This is for user-submitted puzzles, not for puzzle
 *       generation (on the other hand, never say never).
 *
 *  - prove that only w=h=2 needs a special case
 *
 *  - solo-like pencil marks?
 *
 *  - speed up generation of puzzles of size >= 11x11
 *
 *  - Allow square contents > 9?
 *     + I could use letters for digits (solo does this), but
 *       letters don't have numeric significance (normal people hate
 *       base36), which is relevant here (much more than in solo).
 *     + How much information is needed to solve?  Does one need to
 *       know the algorithm by which the largest number is set?
 *
 *  - eliminate puzzle instances with done chunks (1's in particular)?
 *     + that's what the qsort call is all about.
 *     + the 1's don't bother me that much.
 *     + but this takes a LONG time (not always possible)?
 *        - this may be affected by solver (lack of) quality.
 *        - weed them out by construction instead of post-cons check
 *           + but that interleaves make_board and new_game_desc: you
 *             have to alternate between changing the board and
 *             changing the hint set (instead of just creating the
 *             board once, then changing the hint set once -> done).
 *
 *  - use binary search when discovering the minimal sovable point
 *     + profile to show a need (but when the solver gets slower...)
 *     + avg 0.1s per 9x9, which _is_ human-patience noticable.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puzzles.h"

struct game_params {
    int w, h;
};

struct shared_state {
    struct game_params params;
    int *clues;
    int refcnt;
};

struct game_state {
    int *board;
    struct shared_state *shared;
    int completed, cheated;
};

static const struct game_params defaults[3] = {{5, 5}, {7, 7}, {9, 9}};

static game_params *default_params(void)
{
    game_params *ret = snew(game_params);

    *ret = defaults[1]; /* struct copy */

    return ret;
}

static int game_fetch_preset(int i, char **name, game_params **params)
{
    char buf[64];

    if (i < 0 || i >= lenof(defaults)) return FALSE;
    *params = snew(game_params);
    **params = defaults[i]; /* struct copy */
    sprintf(buf, "%dx%d", defaults[i].w, defaults[i].h);
    *name = dupstr(buf);

    return TRUE;
}

static void free_params(game_params *params)
{
    sfree(params);
}

static game_params *dup_params(game_params *params)
{
    game_params *ret = snew(game_params);
    *ret = *params; /* struct copy */
    return ret;
}

static void decode_params(game_params *ret, char const *string)
{
    ret->w = ret->h = atoi(string);
    while (*string && isdigit((unsigned char) *string)) ++string;
    if (*string == 'x') ret->h = atoi(++string);
}

static char *encode_params(game_params *params, int full)
{
    char buf[64];
    sprintf(buf, "%dx%d", params->w, params->h);
    return dupstr(buf);
}

static config_item *game_configure(game_params *params)
{
    config_item *ret;
    char buf[64];

    ret = snewn(3, config_item);

    ret[0].name = "Width";
    ret[0].type = C_STRING;
    sprintf(buf, "%d", params->w);
    ret[0].sval = dupstr(buf);
    ret[0].ival = 0;

    ret[1].name = "Height";
    ret[1].type = C_STRING;
    sprintf(buf, "%d", params->h);
    ret[1].sval = dupstr(buf);
    ret[1].ival = 0;

    ret[2].name = NULL;
    ret[2].type = C_END;
    ret[2].sval = NULL;
    ret[2].ival = 0;

    return ret;
}

static game_params *custom_params(config_item *cfg)
{
    game_params *ret = snew(game_params);

    ret->w = atoi(cfg[0].sval);
    ret->h = atoi(cfg[1].sval);

    return ret;
}

static char *validate_params(game_params *params, int full)
{
    if (params->w < 1) return "Width must be at least one";
    if (params->h < 1) return "Height must be at least one";

    return NULL;
}

/*****************************************************************************
 * STRINGIFICATION OF GAME STATE                                             *
 *****************************************************************************/

#define EMPTY 0

/* Example of plaintext rendering:
 *  +---+---+---+---+---+---+---+
 *  | 6 |   |   | 2 |   |   | 2 |
 *  +---+---+---+---+---+---+---+
 *  |   | 3 |   | 6 |   | 3 |   |
 *  +---+---+---+---+---+---+---+
 *  | 3 |   |   |   |   |   | 1 |
 *  +---+---+---+---+---+---+---+
 *  |   | 2 | 3 |   | 4 | 2 |   |
 *  +---+---+---+---+---+---+---+
 *  | 2 |   |   |   |   |   | 3 |
 *  +---+---+---+---+---+---+---+
 *  |   | 5 |   | 1 |   | 4 |   |
 *  +---+---+---+---+---+---+---+
 *  | 4 |   |   | 3 |   |   | 3 |
 *  +---+---+---+---+---+---+---+
 *
 * This puzzle instance is taken from the nikoli website
 * Encoded (unsolved and solved), the strings are these:
 * 7x7:6002002030603030000010230420200000305010404003003
 * 7x7:6662232336663232331311235422255544325413434443313
 */
static char *board_to_string(int *board, int w, int h) {
    const int sz = w * h;
    const int chw = (4*w + 2); /* +2 for trailing '+' and '\n' */
    const int chh = (2*h + 1); /* +1: n fence segments, n+1 posts */
    const int chlen = chw * chh;
    char *repr = snewn(chlen + 1, char);
    int i;

    assert(board);

    /* build the first line ("^(\+---){n}\+$") */
    for (i = 0; i < w; ++i) {
        repr[4*i + 0] = '+';
        repr[4*i + 1] = '-';
        repr[4*i + 2] = '-';
        repr[4*i + 3] = '-';
    }
    repr[4*i + 0] = '+';
    repr[4*i + 1] = '\n';

    /* ... and copy it onto the odd-numbered lines */
    for (i = 0; i < h; ++i) memcpy(repr + (2*i + 2) * chw, repr, chw);

    /* build the second line ("^(\|\t){n}\|$") */
    for (i = 0; i < w; ++i) {
        repr[chw + 4*i + 0] = '|';
        repr[chw + 4*i + 1] = ' ';
        repr[chw + 4*i + 2] = ' ';
        repr[chw + 4*i + 3] = ' ';
    }
    repr[chw + 4*i + 0] = '|';
    repr[chw + 4*i + 1] = '\n';

    /* ... and copy it onto the even-numbered lines */
    for (i = 1; i < h; ++i) memcpy(repr + (2*i + 1) * chw, repr + chw, chw);

    /* fill in the numbers */
    for (i = 0; i < sz; ++i) {
        const int x = i % w;
        const int y = i / w;
        if (board[i] == EMPTY) continue;
        repr[chw*(2*y + 1) + (4*x + 2)] = board[i] + '0';
    }

    repr[chlen] = '\0';
    return repr;
}

static char *game_text_format(game_state *state)
{
    const int w = state->shared->params.w;
    const int h = state->shared->params.h;
    return board_to_string(state->board, w, h);
}

/*****************************************************************************
 * GAME GENERATION AND SOLVER                                                *
 *****************************************************************************/

static const int dx[4] = {-1, 1, 0, 0};
static const int dy[4] = {0, 0, -1, 1};

/*
static void print_board(int *board, int w, int h) {
    char *repr = board_to_string(board, w, h);
    fputs(repr, stdout);
    free(repr);
}
*/

#define SENTINEL sz

/* determines whether a board (in dsf form) is valid.  If possible,
 * return a conflicting pair in *a and *b and a non-*b neighbour of *a
 * in *c.  If not possible, leave them unmodified. */
static void
validate_board(int *dsf, int w, int h, int *sq, int *a, int *b, int *c) {
    const int sz = w * h;
    int i;
    assert(*a == SENTINEL);
    assert(*b == SENTINEL);
    assert(*c == SENTINEL);
    for (i = 0; i < sz && *a == sz; ++i) {
        const int aa = dsf_canonify(dsf, sq[i]);
        int cc = sz;
        int j;
        for (j = 0; j < 4; ++j) {
            const int x = (sq[i] % w) + dx[j];
            const int y = (sq[i] / w) + dy[j];
            int bb;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            bb = dsf_canonify(dsf, w*y + x);
            if (aa == bb) continue;
            else if (dsf_size(dsf, aa) == dsf_size(dsf, bb)) {
                *a = aa;
                *b = bb;
                *c = cc;
            } else if (cc == sz) *c = cc = bb;
        }
    }
}

static game_state *new_game(midend *, game_params *, char *);
static void free_game(game_state *);

/* generate a random valid board; uses validate_board.  */
void make_board(int *board, int w, int h, random_state *rs) {
    int *dsf;

    const unsigned int sz = w * h;

    /* w=h=2 is a special case which requires a number > max(w, h) */
    /* TODO prove that this is the case ONLY for w=h=2. */
    const int maxsize = min(max(max(w, h), 3), 9);

    /* Note that if 1 in {w, h} then it's impossible to have a region
     * of size > w*h, so the special case only affects w=h=2. */

    int nboards = 0;

    int i;

    assert(w >= 1);
    assert(h >= 1);

    assert(board);

    dsf = snew_dsf(sz); /* implicit dsf_init */

    /* I abuse the board variable: when generating the puzzle, it
     * contains a shuffled list of numbers {0, ..., nsq-1}. */
    for (i = 0; i < sz; ++i) board[i] = i;

    while (1) {
        ++nboards;
        shuffle(board, sz, sizeof (int), rs);
        /* while the board can in principle be fixed */
        while (1) {
            int a = SENTINEL;
            int b = SENTINEL;
            int c = SENTINEL;
            validate_board(dsf, w, h, board, &a, &b, &c);
            if (a == SENTINEL /* meaning the board is valid */) {
                int i;
                for (i = 0; i < sz; ++i) board[i] = dsf_size(dsf, i);
                sfree(dsf);
                /* printf("returning board number %d\n", nboards); */
                return;
            } else {
                /* try to repair the invalid board */
                a = dsf_canonify(dsf, a);
                assert(a != dsf_canonify(dsf, b));
                if (c != sz) assert(a != dsf_canonify(dsf, c));
                dsf_merge(dsf, a, c == sz? b: c);
                /* if repair impossible; make a new board */
                if (dsf_size(dsf, a) > maxsize) break;
            }
        }
        dsf_init(dsf, sz); /* re-init the dsf */
    }
    assert(FALSE); /* unreachable */
}

static int rhofree(int *hop, int start) {
    int turtle = start, rabbit = hop[start];
    while (rabbit != turtle) { /* find a cycle */
        turtle = hop[turtle];
        rabbit = hop[hop[rabbit]];
    }
    do { /* check that start is in the cycle */
        rabbit = hop[rabbit];
        if (start == rabbit) return 1;
    } while (rabbit != turtle);
    return 0;
}

static void merge(int *dsf, int *connected, int a, int b) {
    int c;
    assert(dsf);
    assert(connected);
    assert(rhofree(connected, a));
    assert(rhofree(connected, b));
    a = dsf_canonify(dsf, a);
    b = dsf_canonify(dsf, b);
    if (a == b) return;
    dsf_merge(dsf, a, b);
    c = connected[a];
    connected[a] = connected[b];
    connected[b] = c;
    assert(rhofree(connected, a));
    assert(rhofree(connected, b));
}

static void *memdup(const void *ptr, size_t len, size_t esz) {
    void *dup = smalloc(len * esz);
    assert(ptr);
    memcpy(dup, ptr, len * esz);
    return dup;
}

static void expand(int *board, int *connected, int *dsf, int w, int h,
                   int dst, int src, int *empty, int *learn) {
    int j;
    assert(board);
    assert(connected);
    assert(dsf);
    assert(empty);
    assert(learn);
    assert(board[dst] == EMPTY);
    assert(board[src] != EMPTY);
    board[dst] = board[src];
    for (j = 0; j < 4; ++j) {
        const int x = (dst % w) + dx[j];
        const int y = (dst / w) + dy[j];
        const int idx = w*y + x;
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        if (board[idx] != board[dst]) continue;
        merge(dsf, connected, dst, idx);
    }
/*  printf("set board[%d] = board[%d], which is %d; size(%d) = %d\n", dst, src, board[src], src, dsf[dsf_canonify(dsf, src)] >> 2); */
    --*empty;
    *learn = TRUE;
}

static void flood(int *board, int w, int h, int i, int n) {
    const int sz = w * h;
    int k;

    if (board[i] == EMPTY) board[i] = -SENTINEL;
    else if (board[i] == n) board[i] = -board[i];
    else return;

    for (k = 0; k < 4; ++k) {
        const int x = (i % w) + dx[k];
        const int y = (i / w) + dy[k];
        const int idx = w*y + x;
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        flood(board, w, h, idx, n);
    }
}

static int count_and_clear(int *board, int sz) {
    int count = -1;
    int i;
    for (i = 0; i < sz; ++i) {
        if (board[i] >= 0) continue;
        ++count;
        if (board[i] == -SENTINEL) board[i] = EMPTY;
        else board[i] = -board[i];
    }
    return count;
}

static int count(int *board, int w, int h, int i) {
    flood(board, w, h, i, board[i]);
    return count_and_clear(board, w * h);
}

static int expandsize(const int *board, int *dsf, int w, int h, int i, int n) {
    int j;
    int nhits = 0;
    int hits[4];
    int size = 1;
    for (j = 0; j < 4; ++j) {
        const int x = (i % w) + dx[j];
        const int y = (i / w) + dy[j];
        const int idx = w*y + x;
        int root;
        int m;
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        if (board[idx] != n) continue;
        root = dsf_canonify(dsf, idx);
        for (m = 0; m < nhits && root != hits[m]; ++m);
        if (m < nhits) continue;
        /* printf("\t  (%d, %d) contributed %d to size\n", lx, ly, dsf[root] >> 2); */
        size += dsf_size(dsf, root);
        assert(dsf_size(dsf, root) >= 1);
        hits[nhits++] = root;
    }
    return size;
}

/*
 *  +---+---+---+---+---+---+---+
 *  | 6 |   |   | 2 |   |   | 2 |
 *  +---+---+---+---+---+---+---+
 *  |   | 3 |   | 6 |   | 3 |   |
 *  +---+---+---+---+---+---+---+
 *  | 3 |   |   |   |   |   | 1 |
 *  +---+---+---+---+---+---+---+
 *  |   | 2 | 3 |   | 4 | 2 |   |
 *  +---+---+---+---+---+---+---+
 *  | 2 |   |   |   |   |   | 3 |
 *  +---+---+---+---+---+---+---+
 *  |   | 5 |   | 1 |   | 4 |   |
 *  +---+---+---+---+---+---+---+
 *  | 4 |   |   | 3 |   |   | 3 |
 *  +---+---+---+---+---+---+---+
 */

/* Solving techniques:
 *
 * CONNECTED COMPONENT FORCED EXPANSION (too big):
 * When a CC can only be expanded in one direction, because all the
 * other ones would make the CC too big.
 *  +---+---+---+---+---+
 *  | 2 | 2 |   | 2 | _ |
 *  +---+---+---+---+---+
 *
 * CONNECTED COMPONENT FORCED EXPANSION (too small):
 * When a CC must include a particular square, because otherwise there
 * would not be enough room to complete it.
 *  +---+---+
 *  | 2 | _ |
 *  +---+---+
 *
 * DROPPING IN A ONE:
 * When an empty square has no neighbouring empty squares and only a 1
 * will go into the square (or other CCs would be too big).
 *  +---+---+---+
 *  | 2 | 2 | _ |
 *  +---+---+---+
 *
 * TODO: generalise DROPPING IN A ONE: find the size of the CC of
 * empty squares and a list of all adjacent numbers.  See if only one
 * number in {1, ..., size} u {all adjacent numbers} is possible.
 * Probably this is only effective for a CC size < n for some n (4?)
 *
 * TODO: backtracking.
 */
#define EXPAND(a, b)\
expand(board, connected, dsf, w, h, a, b, &nempty, &learn)

static int solver(const int *orig, int w, int h, char **solution) {
    const int sz = w * h;

    int *board = memdup(orig, sz, sizeof (int));
    int *dsf = snew_dsf(sz); /* eqv classes: connected components */
    int *connected = snewn(sz, int); /* connected[n] := n.next; */
    /* cyclic disjoint singly linked lists, same partitioning as dsf.
     * The lists lets you iterate over a partition given any member */

    int nempty = 0;

    int learn;

    int i;
    for (i = 0; i < sz; i++) connected[i] = i;

    for (i = 0; i < sz; ++i) {
        int j;
        if (board[i] == EMPTY) ++nempty;
        else for (j = 0; j < 4; ++j) {
            const int x = (i % w) + dx[j];
            const int y = (i / w) + dy[j];
            const int idx = w*y + x;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            if (board[i] == board[idx]) merge(dsf, connected, i, idx);
        }
    }

/*  puts("trying to solve this:");
    print_board(board, w, h); */

    /* TODO: refactor this code, it's too long */
    do {
        int i;
        learn = FALSE;

        /* for every connected component */
        for (i = 0; i < sz; ++i) {
            int exp = SENTINEL;
            int j;

            /* If the component consists of empty squares */
            if (board[i] == EMPTY) {
                int k;
                int one = TRUE;
                for (k = 0; k < 4; ++k) {
                    const int x = (i % w) + dx[k];
                    const int y = (i / w) + dy[k];
                    const int idx = w*y + x;
                    int n;
                    if (x < 0 || x >= w || y < 0 || y >= h) continue;
                    if (board[idx] == EMPTY) {
                        one = FALSE;
                        continue;
                    }
                    if (one &&
                        (board[idx] == 1 ||
                         (board[idx] >= expandsize(board, dsf, w, h,
                                                   i, board[idx]))))
                        one = FALSE;
                    assert(board[i] == EMPTY);
                    board[i] = -SENTINEL;
                    n = count(board, w, h, idx);
                    assert(board[i] == EMPTY);
                    if (n >= board[idx]) continue;
                    EXPAND(i, idx);
                    break;
                }
                if (k == 4 && one) {
                    assert(board[i] == EMPTY);
                    board[i] = 1;
                    assert(nempty);
                    --nempty;
                    learn = TRUE;
                }
                continue;
            }
            /* printf("expanding blob of (%d, %d)\n", i % w, i / w); */

            j = dsf_canonify(dsf, i);

            /* (but only for each connected component) */
            if (i != j) continue;

            /* (and not if it's already complete) */
            if (dsf_size(dsf, j) == board[j]) continue;

            /* for each square j _in_ the connected component */
            do {
                int k;
                /* printf("  looking at (%d, %d)\n", j % w, j / w); */

                /* for each neighbouring square (idx) */
                for (k = 0; k < 4; ++k) {
                    const int x = (j % w) + dx[k];
                    const int y = (j / w) + dy[k];
                    const int idx = w*y + x;
                    int size;
                    /* int l;
                       int nhits = 0;
                       int hits[4]; */
                    if (x < 0 || x >= w || y < 0 || y >= h) continue;
                    if (board[idx] != EMPTY) continue;
                    if (exp == idx) continue;
                    /* printf("\ttrying to expand onto (%d, %d)\n", x, y); */

                    /* find out the would-be size of the new connected
                     * component if we actually expanded into idx */
                    /*
                    size = 1;
                    for (l = 0; l < 4; ++l) {
                        const int lx = x + dx[l];
                        const int ly = y + dy[l];
                        const int idxl = w*ly + lx;
                        int root;
                        int m;
                        if (lx < 0 || lx >= w || ly < 0 || ly >= h) continue;
                        if (board[idxl] != board[j]) continue;
                        root = dsf_canonify(dsf, idxl);
                        for (m = 0; m < nhits && root != hits[m]; ++m);
                        if (m != nhits) continue;
                        // printf("\t  (%d, %d) contributed %d to size\n", lx, ly, dsf[root] >> 2);
                        size += dsf_size(dsf, root);
                        assert(dsf_size(dsf, root) >= 1);
                        hits[nhits++] = root;
                    }
                    */

                    size = expandsize(board, dsf, w, h, idx, board[j]);

                    /* ... and see if that size is too big, or if we
                     * have other expansion candidates.  Otherwise
                     * remember the (so far) only candidate. */

                    /* printf("\tthat would give a size of %d\n", size); */
                    if (size > board[j]) continue;
                    /* printf("\tnow knowing %d expansions\n", nexpand + 1); */
                    if (exp != SENTINEL) goto next_i;
                    assert(exp != idx);
                    exp = idx;
                }

                j = connected[j]; /* next square in the same CC */
                assert(board[i] == board[j]);
            } while (j != i);
            /* end: for each square j _in_ the connected component */

            if (exp == SENTINEL) continue;
            /* printf("expand b: %d -> %d\n", i, exp); */
            EXPAND(exp, i);

            next_i:
            ;
        }
        /* end: for each connected component */
    } while (learn && nempty);

    /* puts("best guess:");
       print_board(board, w, h); */

    if (solution) {
        int i;
        assert(*solution == NULL);
        *solution = snewn(sz + 2, char);
        **solution = 's';
        for (i = 0; i < sz; ++i) (*solution)[i + 1] = board[i] + '0';
        (*solution)[sz + 1] = '\0';
        /* We don't need the \0 for execute_move (the only user)
         * I'm just being printf-friendly in case I wanna print */
    }

    sfree(dsf);
    sfree(board);
    sfree(connected);

    return !nempty;
}

static int *make_dsf(int *dsf, int *board, const int w, const int h) {
    const int sz = w * h;
    int i;

    if (!dsf)
        dsf = snew_dsf(w * h);
    else
        dsf_init(dsf, w * h);

    for (i = 0; i < sz; ++i) {
        int j;
        for (j = 0; j < 4; ++j) {
            const int x = (i % w) + dx[j];
            const int y = (i / w) + dy[j];
            const int k = w*y + x;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            if (board[i] == board[k]) dsf_merge(dsf, i, k);
        }
    }
    return dsf;
}

/*
static int filled(int *board, int *randomize, int k, int n) {
    int i;
    if (board == NULL) return FALSE;
    if (randomize == NULL) return FALSE;
    if (k > n) return FALSE;
    for (i = 0; i < k; ++i) if (board[randomize[i]] == 0) return FALSE;
    for (; i < n; ++i) if (board[randomize[i]] != 0) return FALSE;
    return TRUE;
}
*/

static int *g_board;
static int compare(const void *pa, const void *pb) {
    if (!g_board) return 0;
    return g_board[*(const int *)pb] - g_board[*(const int *)pa];
}

static char *new_game_desc(game_params *params, random_state *rs,
                           char **aux, int interactive)
{
    const int w = params->w;
    const int h = params->h;
    const int sz = w * h;
    int *board = snewn(sz, int);
    int *randomize = snewn(sz, int);
    int *solver_board = snewn(sz, int);
    char *game_description = snewn(sz + 1, char);
    int i;

    for (i = 0; i < sz; ++i) {
        board[i] = EMPTY;
        randomize[i] = i;
    }

    make_board(board, w, h, rs);
    memcpy(solver_board, board, sz * sizeof (int));

    g_board = board;
    qsort(randomize, sz, sizeof (int), compare);

    /* since more clues only helps and never hurts, one pass will do
     * just fine: if we can remove clue n with k clues of index > n,
     * we could have removed clue n with >= k clues of index > n.
     * So an additional pass wouldn't do anything [use induction]. */
    for (i = 0; i < sz; ++i) {
        solver_board[randomize[i]] = EMPTY;
        if (!solver(solver_board, w, h, NULL))
            solver_board[randomize[i]] = board[randomize[i]];
    }

    for (i = 0; i < sz; ++i) {
        assert(solver_board[i] >= 0);
        assert(solver_board[i] < 10);
        game_description[i] = solver_board[i] + '0';
    }
    game_description[sz] = '\0';

/*
  solver(solver_board, w, h, aux);
  print_board(solver_board, w, h);
*/

    sfree(randomize);
    sfree(solver_board);
    sfree(board);

    return game_description;
}

static char *validate_desc(game_params *params, char *desc)
{
    int i;
    const int sz = params->w * params->h;
    const char m = '0' + max(max(params->w, params->h), 3);

    /* printf("desc = '%s'; sz = %d\n", desc, sz); */

    for (i = 0; desc[i] && i < sz; ++i)
        if (!isdigit((unsigned char) *desc))
	    return "non-digit in string";
	else if (desc[i] > m)
	    return "too large digit in string";
    if (desc[i]) return "string too long";
    else if (i < sz) return "string too short";
    return NULL;
}

static game_state *new_game(midend *me, game_params *params, char *desc)
{
    game_state *state = snew(game_state);
    int sz = params->w * params->h;
    int i;

    state->cheated = state->completed = FALSE;
    state->shared = snew(struct shared_state);
    state->shared->refcnt = 1;
    state->shared->params = *params; /* struct copy */
    state->shared->clues = snewn(sz, int);
    for (i = 0; i < sz; ++i) state->shared->clues[i] = desc[i] - '0';
    state->board = memdup(state->shared->clues, sz, sizeof (int));

    return state;
}

static game_state *dup_game(game_state *state)
{
    const int sz = state->shared->params.w * state->shared->params.h;
    game_state *ret = snew(game_state);

    ret->board = memdup(state->board, sz, sizeof (int));
    ret->shared = state->shared;
    ret->cheated = state->cheated;
    ret->completed = state->completed;
    ++ret->shared->refcnt;

    return ret;
}

static void free_game(game_state *state)
{
    assert(state);
    sfree(state->board);
    if (--state->shared->refcnt == 0) {
        sfree(state->shared->clues);
        sfree(state->shared);
    }
    sfree(state);
}

static char *solve_game(game_state *state, game_state *currstate,
                        char *aux, char **error)
{
    if (aux == NULL) {
        const int w = state->shared->params.w;
        const int h = state->shared->params.h;
        if (!solver(state->board, w, h, &aux))
            *error = "Sorry, I couldn't find a solution";
    }
    return aux;
}

/*****************************************************************************
 * USER INTERFACE STATE AND ACTION                                           *
 *****************************************************************************/

struct game_ui {
    int x, y; /* highlighted square, or (-1, -1) if none */
};

static game_ui *new_ui(game_state *state)
{
    game_ui *ui = snew(game_ui);

    ui->x = ui->y = -1;

    return ui;
}

static void free_ui(game_ui *ui)
{
    sfree(ui);
}

static char *encode_ui(game_ui *ui)
{
    return NULL;
}

static void decode_ui(game_ui *ui, char *encoding)
{
}

static void game_changed_state(game_ui *ui, game_state *oldstate,
                               game_state *newstate)
{
}

#define PREFERRED_TILE_SIZE 32
#define TILE_SIZE (ds->tilesize)
#define BORDER (TILE_SIZE / 2)
#define BORDER_WIDTH (TILE_SIZE / 32)

struct game_drawstate {
    struct game_params params;
    int tilesize;
    int started;
    int *v, *flags;
    int *dsf_scratch, *border_scratch;
};

static char *interpret_move(game_state *state, game_ui *ui, game_drawstate *ds,
                            int x, int y, int button)
{
    const int w = state->shared->params.w;
    const int h = state->shared->params.h;

    const int tx = (x + TILE_SIZE - BORDER) / TILE_SIZE - 1;
    const int ty = (y + TILE_SIZE - BORDER) / TILE_SIZE - 1;

    assert(ui);
    assert(ds);

    button &= ~MOD_MASK;

    if (tx >= 0 && tx < w && ty >= 0 && ty < h) {
        if (button == LEFT_BUTTON) {
            if ((tx == ui->x && ty == ui->y) || state->shared->clues[w*ty+tx])
                ui->x = ui->y = -1;
            else ui->x = tx, ui->y = ty;
            return ""; /* redraw */
        }
    }

    assert((ui->x == -1) == (ui->y == -1));
    if (ui->x == -1) return NULL;
    assert(state->shared->clues[w*ui->y + ui->x] == 0);

    switch (button) {
      case ' ':
      case '\r':
      case '\n':
      case '\b':
      case '\177':
        button = 0;
        break;
      default:
        if (!isdigit(button)) return NULL;
        button -= '0';
        if (button > (w == 2 && h == 2? 3: max(w, h))) return NULL;
    }

    {
        const int i = w*ui->y + ui->x;
        char buf[64];
        ui->x = ui->y = -1;
	if (state->board[i] == button) {
	    return "";		       /* no change - just update ui */
	} else {
	    sprintf(buf, "%d_%d", i, button);
	    return dupstr(buf);
	}
    }
}

static game_state *execute_move(game_state *state, char *move)
{
    game_state *new_state;

    if (*move == 's') {
        const int sz = state->shared->params.w * state->shared->params.h;
        int i = 0;
        new_state = dup_game(state);
        for (++move; i < sz; ++i) new_state->board[i] = move[i] - '0';
        new_state->cheated = TRUE;
    } else {
        char *endptr;
        const int i = strtol(move, &endptr, errno = 0);
        int value;
        if (errno == ERANGE) return NULL;
        if (endptr == move) return NULL;
        if (*endptr != '_') return NULL;
        move = endptr + 1;
        value = strtol(move, &endptr, 0);
        if (endptr == move) return NULL;
        if (*endptr != '\0') return NULL;
        new_state = dup_game(state);
        new_state->board[i] = value;
    }

    /*
     * Check for completion.
     */
    if (!new_state->completed) {
        const int w = new_state->shared->params.w;
        const int h = new_state->shared->params.h;
        const int sz = w * h;
        int *dsf = make_dsf(NULL, new_state->board, w, h);
        int i;
        for (i = 0; i < sz && new_state->board[i] == dsf_size(dsf, i); ++i);
        sfree(dsf);
        if (i == sz)
            new_state->completed = TRUE;
    }

    return new_state;
}

/* ----------------------------------------------------------------------
 * Drawing routines.
 */

#define FLASH_TIME 0.4F

#define COL_CLUE COL_GRID
enum {
    COL_BACKGROUND,
    COL_GRID,
    COL_HIGHLIGHT,
    COL_CORRECT,
    COL_ERROR,
    COL_USER,
    NCOLOURS
};

static void game_compute_size(game_params *params, int tilesize,
                              int *x, int *y)
{
    *x = (params->w + 1) * tilesize;
    *y = (params->h + 1) * tilesize;
}

static void game_set_size(drawing *dr, game_drawstate *ds,
                          game_params *params, int tilesize)
{
    ds->tilesize = tilesize;
}

static float *game_colours(frontend *fe, int *ncolours)
{
    float *ret = snewn(3 * NCOLOURS, float);

    frontend_default_colour(fe, &ret[COL_BACKGROUND * 3]);

    ret[COL_GRID * 3 + 0] = 0.0F;
    ret[COL_GRID * 3 + 1] = 0.0F;
    ret[COL_GRID * 3 + 2] = 0.0F;

    ret[COL_HIGHLIGHT * 3 + 0] = 0.85F * ret[COL_BACKGROUND * 3 + 0];
    ret[COL_HIGHLIGHT * 3 + 1] = 0.85F * ret[COL_BACKGROUND * 3 + 1];
    ret[COL_HIGHLIGHT * 3 + 2] = 0.85F * ret[COL_BACKGROUND * 3 + 2];

    ret[COL_CORRECT * 3 + 0] = 0.9F * ret[COL_BACKGROUND * 3 + 0];
    ret[COL_CORRECT * 3 + 1] = 0.9F * ret[COL_BACKGROUND * 3 + 1];
    ret[COL_CORRECT * 3 + 2] = 0.9F * ret[COL_BACKGROUND * 3 + 2];

    ret[COL_ERROR * 3 + 0] = 1.0F;
    ret[COL_ERROR * 3 + 1] = 0.85F * ret[COL_BACKGROUND * 3 + 1];
    ret[COL_ERROR * 3 + 2] = 0.85F * ret[COL_BACKGROUND * 3 + 2];

    ret[COL_USER * 3 + 0] = 0.0F;
    ret[COL_USER * 3 + 1] = 0.6F * ret[COL_BACKGROUND * 3 + 1];
    ret[COL_USER * 3 + 2] = 0.0F;

    *ncolours = NCOLOURS;
    return ret;
}

static game_drawstate *game_new_drawstate(drawing *dr, game_state *state)
{
    struct game_drawstate *ds = snew(struct game_drawstate);
    int i;

    ds->tilesize = PREFERRED_TILE_SIZE;
    ds->started = 0;
    ds->params = state->shared->params;
    ds->v = snewn(ds->params.w * ds->params.h, int);
    ds->flags = snewn(ds->params.w * ds->params.h, int);
    for (i = 0; i < ds->params.w * ds->params.h; i++)
	ds->v[i] = ds->flags[i] = -1;
    ds->border_scratch = snewn(ds->params.w * ds->params.h, int);
    ds->dsf_scratch = NULL;

    return ds;
}

static void game_free_drawstate(drawing *dr, game_drawstate *ds)
{
    sfree(ds->v);
    sfree(ds->flags);
    sfree(ds->border_scratch);
    sfree(ds->dsf_scratch);
    sfree(ds);
}

#define BORDER_U   0x001
#define BORDER_D   0x002
#define BORDER_L   0x004
#define BORDER_R   0x008
#define BORDER_UR  0x010
#define BORDER_DR  0x020
#define BORDER_UL  0x040
#define BORDER_DL  0x080
#define CURSOR_BG  0x100
#define CORRECT_BG 0x200
#define ERROR_BG   0x400
#define USER_COL   0x800

static void draw_square(drawing *dr, game_drawstate *ds, int x, int y,
                        int n, int flags)
{
    assert(dr);
    assert(ds);

    /*
     * Clear the square.
     */
    draw_rect(dr,
              BORDER + x*TILE_SIZE + 1,
              BORDER + y*TILE_SIZE + 1,
              TILE_SIZE - 1,
              TILE_SIZE - 1,
              (flags & CURSOR_BG ? COL_HIGHLIGHT :
               flags & ERROR_BG ? COL_ERROR :
               flags & CORRECT_BG ? COL_CORRECT : COL_BACKGROUND));

    /*
     * Draw the number.
     */
    if (n) {
        char buf[2];
        buf[0] = n + '0';
        buf[1] = '\0';
        draw_text(dr,
                  (x + 1) * TILE_SIZE,
                  (y + 1) * TILE_SIZE,
                  FONT_VARIABLE,
                  TILE_SIZE / 2,
                  ALIGN_VCENTRE | ALIGN_HCENTRE,
                  flags & USER_COL ? COL_USER : COL_CLUE,
                  buf);
    }

    /*
     * Draw bold lines around the borders.
     */
    if (flags & BORDER_L)
        draw_rect(dr,
                  BORDER + x*TILE_SIZE + 1,
                  BORDER + y*TILE_SIZE + 1,
                  BORDER_WIDTH,
                  TILE_SIZE - 1,
                  COL_GRID);
    if (flags & BORDER_U)
        draw_rect(dr,
                  BORDER + x*TILE_SIZE + 1,
                  BORDER + y*TILE_SIZE + 1,
                  TILE_SIZE - 1,
                  BORDER_WIDTH,
                  COL_GRID);
    if (flags & BORDER_R)
        draw_rect(dr,
                  BORDER + (x+1)*TILE_SIZE - BORDER_WIDTH,
                  BORDER + y*TILE_SIZE + 1,
                  BORDER_WIDTH,
                  TILE_SIZE - 1,
                  COL_GRID);
    if (flags & BORDER_D)
        draw_rect(dr,
                  BORDER + x*TILE_SIZE + 1,
                  BORDER + (y+1)*TILE_SIZE - BORDER_WIDTH,
                  TILE_SIZE - 1,
                  BORDER_WIDTH,
                  COL_GRID);
    if (flags & BORDER_UL)
        draw_rect(dr,
                  BORDER + x*TILE_SIZE + 1,
                  BORDER + y*TILE_SIZE + 1,
                  BORDER_WIDTH,
                  BORDER_WIDTH,
                  COL_GRID);
    if (flags & BORDER_UR)
        draw_rect(dr,
                  BORDER + (x+1)*TILE_SIZE - BORDER_WIDTH,
                  BORDER + y*TILE_SIZE + 1,
                  BORDER_WIDTH,
                  BORDER_WIDTH,
                  COL_GRID);
    if (flags & BORDER_DL)
        draw_rect(dr,
                  BORDER + x*TILE_SIZE + 1,
                  BORDER + (y+1)*TILE_SIZE - BORDER_WIDTH,
                  BORDER_WIDTH,
                  BORDER_WIDTH,
                  COL_GRID);
    if (flags & BORDER_DR)
        draw_rect(dr,
                  BORDER + (x+1)*TILE_SIZE - BORDER_WIDTH,
                  BORDER + (y+1)*TILE_SIZE - BORDER_WIDTH,
                  BORDER_WIDTH,
                  BORDER_WIDTH,
                  COL_GRID);
    
    draw_update(dr,
		BORDER + x*TILE_SIZE - 1,
		BORDER + y*TILE_SIZE - 1,
		TILE_SIZE + 3,
		TILE_SIZE + 3);
}

static void draw_grid(drawing *dr, game_drawstate *ds, game_state *state,
                      game_ui *ui, int flashy, int borders, int shading)
{
    const int w = state->shared->params.w;
    const int h = state->shared->params.h;
    int x;
    int y;

    /*
     * Build a dsf for the board in its current state, to use for
     * highlights and hints.
     */
    ds->dsf_scratch = make_dsf(ds->dsf_scratch, state->board, w, h);

    /*
     * Work out where we're putting borders between the cells.
     */
    for (y = 0; y < w*h; y++)
	ds->border_scratch[y] = 0;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int dx, dy;
            int v1, s1, v2, s2;

            for (dx = 0; dx <= 1; dx++) {
                int border = FALSE;

                dy = 1 - dx;

                if (x+dx >= w || y+dy >= h)
                    continue;

                v1 = state->board[y*w+x];
                v2 = state->board[(y+dy)*w+(x+dx)];
                s1 = dsf_size(ds->dsf_scratch, y*w+x);
                s2 = dsf_size(ds->dsf_scratch, (y+dy)*w+(x+dx));

                /*
                 * We only ever draw a border between two cells if
                 * they don't have the same contents.
                 */
                if (v1 != v2) {
                    /*
                     * But in that situation, we don't always draw
                     * a border. We do if the two cells both
                     * contain actual numbers...
                     */
                    if (v1 && v2)
                        border = TRUE;

                    /*
                     * ... or if at least one of them is a
                     * completed or overfull omino.
                     */
                    if (v1 && s1 >= v1)
                        border = TRUE;
                    if (v2 && s2 >= v2)
                        border = TRUE;
                }

                if (border)
                    ds->border_scratch[y*w+x] |= (dx ? 1 : 2);
            }
        }

    /*
     * Actually do the drawing.
     */
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            /*
             * Determine what we need to draw in this square.
             */
            int v = state->board[y*w+x];
            int flags = 0;

            if (flashy || !shading) {
                /* clear all background flags */
            } else if (x == ui->x && y == ui->y) {
                flags |= CURSOR_BG;
            } else if (v) {
                int size = dsf_size(ds->dsf_scratch, y*w+x);
                if (size == v)
                    flags |= CORRECT_BG;
                else if (size > v)
                    flags |= ERROR_BG;
            }

            /*
             * Borders at the very edges of the grid are
             * independent of the `borders' flag.
             */
            if (x == 0)
                flags |= BORDER_L;
            if (y == 0)
                flags |= BORDER_U;
            if (x == w-1)
                flags |= BORDER_R;
            if (y == h-1)
                flags |= BORDER_D;

            if (borders) {
                if (x == 0 || (ds->border_scratch[y*w+(x-1)] & 1))
                    flags |= BORDER_L;
                if (y == 0 || (ds->border_scratch[(y-1)*w+x] & 2))
                    flags |= BORDER_U;
                if (x == w-1 || (ds->border_scratch[y*w+x] & 1))
                    flags |= BORDER_R;
                if (y == h-1 || (ds->border_scratch[y*w+x] & 2))
                    flags |= BORDER_D;

                if (y > 0 && x > 0 && (ds->border_scratch[(y-1)*w+(x-1)]))
                    flags |= BORDER_UL;
                if (y > 0 && x < w-1 &&
                    ((ds->border_scratch[(y-1)*w+x] & 1) ||
                     (ds->border_scratch[(y-1)*w+(x+1)] & 2)))
                    flags |= BORDER_UR;
                if (y < h-1 && x > 0 &&
                    ((ds->border_scratch[y*w+(x-1)] & 2) ||
                     (ds->border_scratch[(y+1)*w+(x-1)] & 1)))
                    flags |= BORDER_DL;
                if (y < h-1 && x < w-1 &&
                    ((ds->border_scratch[y*w+(x+1)] & 2) ||
                     (ds->border_scratch[(y+1)*w+x] & 1)))
                    flags |= BORDER_DR;
            }

            if (!state->shared->clues[y*w+x])
                flags |= USER_COL;

            if (ds->v[y*w+x] != v || ds->flags[y*w+x] != flags) {
                draw_square(dr, ds, x, y, v, flags);
                ds->v[y*w+x] = v;
                ds->flags[y*w+x] = flags;
            }
        }
}

static void game_redraw(drawing *dr, game_drawstate *ds, game_state *oldstate,
                        game_state *state, int dir, game_ui *ui,
                        float animtime, float flashtime)
{
    const int w = state->shared->params.w;
    const int h = state->shared->params.h;

    const int flashy =
        flashtime > 0 &&
        (flashtime <= FLASH_TIME/3 || flashtime >= FLASH_TIME*2/3);

    if (!ds->started) {
        /*
         * The initial contents of the window are not guaranteed and
         * can vary with front ends. To be on the safe side, all games
         * should start by drawing a big background-colour rectangle
         * covering the whole window.
         */
        draw_rect(dr, 0, 0, 10*ds->tilesize, 10*ds->tilesize, COL_BACKGROUND);

	/*
	 * Smaller black rectangle which is the main grid.
	 */
	draw_rect(dr, BORDER - BORDER_WIDTH, BORDER - BORDER_WIDTH,
		  w*TILE_SIZE + 2*BORDER_WIDTH + 1,
		  h*TILE_SIZE + 2*BORDER_WIDTH + 1,
		  COL_GRID);

        ds->started = TRUE;
    }

    draw_grid(dr, ds, state, ui, flashy, TRUE, TRUE);
}

static float game_anim_length(game_state *oldstate, game_state *newstate,
                              int dir, game_ui *ui)
{
    return 0.0F;
}

static float game_flash_length(game_state *oldstate, game_state *newstate,
                               int dir, game_ui *ui)
{
    assert(oldstate);
    assert(newstate);
    assert(newstate->shared);
    assert(oldstate->shared == newstate->shared);
    if (!oldstate->completed && newstate->completed &&
	!oldstate->cheated && !newstate->cheated)
        return FLASH_TIME;
    return 0.0F;
}

static int game_timing_state(game_state *state, game_ui *ui)
{
    return TRUE;
}

static void game_print_size(game_params *params, float *x, float *y)
{
    int pw, ph;

    /*
     * I'll use 6mm squares by default.
     */
    game_compute_size(params, 600, &pw, &ph);
    *x = pw / 100.0;
    *y = ph / 100.0;
}

static void game_print(drawing *dr, game_state *state, int tilesize)
{
    const int w = state->shared->params.w;
    const int h = state->shared->params.h;
    int c, i, borders;

    /* Ick: fake up `ds->tilesize' for macro expansion purposes */
    game_drawstate *ds = game_new_drawstate(dr, state);
    game_set_size(dr, ds, NULL, tilesize);

    c = print_mono_colour(dr, 1); assert(c == COL_BACKGROUND);
    c = print_mono_colour(dr, 0); assert(c == COL_GRID);
    c = print_mono_colour(dr, 1); assert(c == COL_HIGHLIGHT);
    c = print_mono_colour(dr, 1); assert(c == COL_CORRECT);
    c = print_mono_colour(dr, 1); assert(c == COL_ERROR);
    c = print_mono_colour(dr, 0); assert(c == COL_USER);

    /*
     * Border.
     */
    draw_rect(dr, BORDER - BORDER_WIDTH, BORDER - BORDER_WIDTH,
              w*TILE_SIZE + 2*BORDER_WIDTH + 1,
              h*TILE_SIZE + 2*BORDER_WIDTH + 1,
              COL_GRID);

    /*
     * We'll draw borders between the ominoes iff the grid is not
     * pristine. So scan it to see if it is.
     */
    borders = FALSE;
    for (i = 0; i < w*h; i++)
        if (state->board[i] && !state->shared->clues[i])
            borders = TRUE;

    /*
     * Draw grid.
     */
    draw_grid(dr, ds, state, NULL, FALSE, borders, FALSE);

    /*
     * Clean up.
     */
    game_free_drawstate(dr, ds);
}

#ifdef COMBINED
#define thegame filling
#endif

const struct game thegame = {
    "Filling", "games.filling", "filling",
    default_params,
    game_fetch_preset,
    decode_params,
    encode_params,
    free_params,
    dup_params,
    TRUE, game_configure, custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    dup_game,
    free_game,
    TRUE, solve_game,
    TRUE, game_text_format,
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    game_changed_state,
    interpret_move,
    execute_move,
    PREFERRED_TILE_SIZE, game_compute_size, game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    TRUE, FALSE, game_print_size, game_print,
    FALSE,				   /* wants_statusbar */
    FALSE, game_timing_state,
    0,					   /* flags */
};

#ifdef STANDALONE_SOLVER /* solver? hah! */

int main(int argc, char **argv) {
    while (*++argv) {
        game_params *params;
        game_state *state;
        char *par;
        char *desc;

        for (par = desc = *argv; *desc != '\0' && *desc != ':'; ++desc);
        if (*desc == '\0') {
            fprintf(stderr, "bad puzzle id: %s", par);
            continue;
        }

        *desc++ = '\0';

        params = snew(game_params);
        decode_params(params, par);
        state = new_game(NULL, params, desc);
        if (solver(state->board, params->w, params->h, NULL))
            printf("%s:%s: solvable\n", par, desc);
        else
            printf("%s:%s: not solvable\n", par, desc);
    }
    return 0;
}

#endif