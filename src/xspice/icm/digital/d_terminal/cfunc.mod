/* d_terminal — Digital UART terminal with PTY
 * Provides interactive serial terminal via pseudo-terminal.
 *
 * Port order: d_in d_out
 *   d_in  = UART RX (receives from circuit)
 *   d_out = UART TX (sends to circuit)
 *
 * Parameters:
 *   baud      - baud rate (default 9600)
 *   data_bits - data bits 5-9 (default 8)
 *   parity    - "none", "even", "odd" (default "none")
 *   stop_bits - 1 or 2 (default 1)
 *   pty       - PTY symlink path for interactive access
 *               Creates /dev/pts/N and symlinks to given path.
 *
 * Usage (netlist):
 *   .model uart d_terminal(baud=115200 data_bits=8 stop_bits=1 pty="/tmp/tty")
 *   A1 rx tx uart
 *
 * Full-duplex cross-connect (two instances):
 *   A1 rx_a tx_b term1
 *   A2 rx_b tx_a term2
 *
 * Probe via dac_bridge (optional, for analog visualization):
 *   .model bridge dac_bridge(out_low=0 out_high=5)
 *   A_dac [tx] [tx_an 0] bridge
 *
 * NOTE: cm_schedule_output does not propagate to dac_bridge hybrid
 * outputs. This model uses direct OUTPUT_STATE/OUTPUT_CHANGED assertion
 * in STEP_PENDING (polling at ~1us intervals), which works correctly
 * with dac_bridge and other hybrid analog-digital models.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

/* Suppress warn_unused_result for write/symlink (glibc attribute) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"

#define DT_RING_SIZE 512

struct DtState {
    int pty_fd;
    char slave_path[256];
    char link_path[256];

    /* Configuration */
    double bit_time;
    int data_bits;
    int parity_type;  /* 0=none, 1=even, 2=odd */
    double stop_bits;

    /* TX ring buffer (PTY input -> UART output to circuit) */
    unsigned char tx_ring[DT_RING_SIZE];
    int tx_head;
    int tx_tail;

    /* TX state */
    int tx_busy;
    int tx_byte;
    double tx_start_time;

    /* RX state */
    int rx_busy;
    int rx_prev_in;
    int rx_byte;
    int rx_bit_idx;
    double rx_start_time;
};

static int dt_parity(int byte, int data_bits, int ptype)
{
    int p = 0;
    for (int i = 0; i < data_bits; i++)
        p ^= (byte >> i) & 1;
    if (ptype == 2) p = !p;
    return p;
}

static int dt_frame_bits(int data_bits, int has_parity, int stop_bits)
{
    return 1 + data_bits + (has_parity ? 1 : 0) + stop_bits;
}

static void dt_callback(Mif_Private_t *mif_private, Mif_Callback_Reason_t reason)
{
    if (reason == MIF_CB_DESTROY) {
        struct DtState *ts = (struct DtState *)mif_private->inst_var[0]->element[0].pvalue;
        if (ts) {
            if (ts->pty_fd >= 0) {
                close(ts->pty_fd);
                ts->pty_fd = -1;
            }
            if (ts->link_path[0]) {
                unlink(ts->link_path);
                ts->link_path[0] = '\0';
            }
            free(ts);
            mif_private->inst_var[0]->element[0].pvalue = NULL;
        }
    }
}

void cm_d_terminal(ARGS)
{
    struct DtState *ts;

    if (INIT) {
        ts = malloc(sizeof(struct DtState));
        if (!ts) { fprintf(stderr, "d_terminal: malloc failed\n"); return; }
        memset(ts, 0, sizeof(struct DtState));
        STATIC_VAR(term_state) = ts;

        ts->pty_fd = -1;
        ts->bit_time = 1.0 / PARAM(baud);
        ts->data_bits = PARAM_NULL(data_bits) ? 8 : (int)PARAM(data_bits);
        if (ts->data_bits < 5) ts->data_bits = 8;
        ts->stop_bits = PARAM_NULL(stop_bits) ? 1.0 : PARAM(stop_bits);
        if (ts->stop_bits < 1.0) ts->stop_bits = 1.0;
        ts->rx_prev_in = 1;
        ts->tx_busy = 0;
        ts->rx_busy = 0;

        if (!PARAM_NULL(parity) && PARAM(parity)[0]) {
            if (strcmp(PARAM(parity), "even") == 0) ts->parity_type = 1;
            else if (strcmp(PARAM(parity), "odd") == 0) ts->parity_type = 2;
        }

        /* Open PTY if path given */
        if (!PARAM_NULL(pty) && PARAM(pty)[0]) {
            const char *link = PARAM(pty);
            int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
            if (master >= 0) {
                /* Unlock slave (modern devpts handles permissions automatically) */
                int unlock = 0;
                ioctl(master, TIOCSPTLCK, &unlock);

                int pty_num;
                if (ioctl(master, TIOCGPTN, &pty_num) == 0) {
                    snprintf(ts->slave_path, sizeof(ts->slave_path),
                             "/dev/pts/%d", pty_num);
                    strncpy(ts->link_path, link, sizeof(ts->link_path) - 1);
                    ts->link_path[sizeof(ts->link_path) - 1] = '\0';
                    unlink(link);
                    (void)symlink(ts->slave_path, link);
                    int fl = fcntl(master, F_GETFL, 0);
                    fcntl(master, F_SETFL, fl | O_NONBLOCK);
                    ts->pty_fd = master;
                } else {
                    close(master);
                }
            }
        }

        /* Set initial output = HIGH (marking) */
        OUTPUT_STATE(d_out) = ONE;
        OUTPUT_STRENGTH(d_out) = STRONG;
        OUTPUT_CHANGED(d_out) = TRUE;
        OUTPUT_DELAY(d_out) = 0.0;

        /* Auto-transmit a test byte for verification */
        {
            const unsigned char test_byte = 0x55;
            int next = (ts->tx_tail + 1) % DT_RING_SIZE;
            if (next != ts->tx_head) {
                ts->tx_ring[ts->tx_tail] = test_byte;
                ts->tx_tail = next;
            }
        }

        CALLBACK = dt_callback;
        cm_irreversible(1);
        cm_event_queue(1e-12);
        return;
    }

    ts = STATIC_VAR(term_state);
    if (!ts) return;

    if (CALL_TYPE != STEP_PENDING) return;

    double now = TIME;
    double tick = ts->bit_time / 80.0;
    if (tick > 1e-6) tick = 1e-6;
    cm_event_queue(now + tick);

    /* Determine the correct output state for this time point */
    Digital_State_t state = ONE;
    int total = dt_frame_bits(ts->data_bits, ts->parity_type != 0, (int)ts->stop_bits);

    if (ts->tx_busy) {
        double elapsed = now - ts->tx_start_time;
        if (elapsed >= (double)total * ts->bit_time) {
            ts->tx_busy = 0;
            state = ONE;
        } else {
            int bit_idx = (int)(elapsed / ts->bit_time);
            if (bit_idx >= total) {
                state = ONE;
            } else if (bit_idx == 0) {
                state = ZERO;
            } else if (bit_idx <= ts->data_bits) {
                int di = bit_idx - 1;
                state = (ts->tx_byte >> di) & 1 ? ONE : ZERO;
            } else if (ts->parity_type && bit_idx == ts->data_bits + 1) {
                state = dt_parity(ts->tx_byte, ts->data_bits, ts->parity_type) ? ONE : ZERO;
            } else {
                state = ONE;
            }
        }
    }

    /* Start new byte if idle and data available */
    if (!ts->tx_busy && ts->tx_head != ts->tx_tail) {
        ts->tx_byte = ts->tx_ring[ts->tx_head];
        ts->tx_head = (ts->tx_head + 1) % DT_RING_SIZE;
        ts->tx_busy = 1;
        ts->tx_start_time = now;
        state = ZERO;
    }

    /* Assert output */
    OUTPUT_STATE(d_out) = state;
    OUTPUT_STRENGTH(d_out) = STRONG;
    OUTPUT_CHANGED(d_out) = TRUE;
    OUTPUT_DELAY(d_out) = 1e-12;

    /* --- RX: sample input, detect start bit --- */
    int in_state = INPUT_STATE(d_in);

    if (!ts->rx_busy) {
        if (ts->rx_prev_in == 1 && in_state == 0) {
            ts->rx_busy = 1;
            ts->rx_byte = 0;
            ts->rx_bit_idx = 0;
            ts->rx_start_time = now;
        }
    } else {
        double elapsed = now - ts->rx_start_time;

        for (int i = ts->rx_bit_idx; i < total; i++) {
            double sample_time;
            if (i == 0)
                sample_time = 0.5 * ts->bit_time;
            else
                sample_time = ((double)i + 0.5) * ts->bit_time;

            if (elapsed >= sample_time - tick && ts->rx_bit_idx == i) {
                ts->rx_bit_idx = i + 1;

                if (i == 0) {
                    if (in_state != 0)
                        ts->rx_busy = 0;
                } else if (i <= ts->data_bits) {
                    int di = i - 1;
                    if (in_state == 1)
                        ts->rx_byte |= (1 << di);
                } else if (ts->parity_type && i == ts->data_bits + 1) {
                } else {
                    if (in_state == 1 && ts->pty_fd >= 0)
                        (void)write(ts->pty_fd, &ts->rx_byte, 1);
                    ts->rx_busy = 0;
                }
                break;
            }
        }

        if (elapsed > (double)total * ts->bit_time * 2.0)
            ts->rx_busy = 0;
    }

    ts->rx_prev_in = in_state;
}

#pragma GCC diagnostic pop
