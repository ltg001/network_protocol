#include <cstring>
#include <string>
#include <vector>
#include <set>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 2000
#define WINDOW_NUMBER 8
#define SLIDE_WINDOW_SIZE 2
#define WITH_ACK true

struct FRAME
{
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN];
    unsigned int padding;
};

static unsigned char buffer[PKT_LEN], nbuffered;
static unsigned char frame_send_end = 0, frame_send_head = 0;
static unsigned char frame_expected = 0;
static int phl_ready = 0;
static unsigned char frame_send[WINDOW_NUMBER * PKT_LEN];
static unsigned char send_now = 0;

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(void)
{
    struct FRAME s;
    s.kind = FRAME_DATA;
    s.seq = send_now;
    s.ack = frame_expected == 0 ? WINDOW_NUMBER - 1 : frame_expected - 1;
    memcpy(s.data, frame_send + send_now * PKT_LEN, PKT_LEN);
    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    stop_ack_timer();
    start_timer(send_now, DATA_TIMER);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
    send_now = (send_now + 1) % WINDOW_NUMBER;
}

static void save_data_frame(void)
{
    memcpy(frame_send + frame_send_head * PKT_LEN, buffer, PKT_LEN);

    //dbg_frame("Save DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    send_data_frame();
    frame_send_head = (frame_send_head + 1) % WINDOW_NUMBER;
}

static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = frame_expected == 0 ? WINDOW_NUMBER - 1 : frame_expected - 1;

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

static bool is_expected_ack(unsigned char pos)
{
    auto l = frame_send_end;
    auto r = frame_send_head;
    if (r < l)
    {
        r += WINDOW_NUMBER;
    }
    if (pos < l)
    {
        pos += WINDOW_NUMBER;
    }
    return l <= pos && pos < r;
}

static void process_frame(struct FRAME &f, int len)
{
    if (f.kind == FRAME_ACK)
        dbg_frame("Recv ACK  %d\n", f.ack);
    if (f.kind == FRAME_DATA)
    {
        dbg_frame("Recv DATA %d %d, ID %d. Expect: %d\n", f.seq, f.ack, *(short *)f.data, frame_expected);
        if (f.seq == frame_expected)
        {
            put_packet(f.data, len - 7);
            frame_expected = (frame_expected + 1) % WINDOW_NUMBER;
        }
    }
    if (WITH_ACK)
    {
        start_ack_timer(DATA_TIMER);
    }
    else
    {
        send_ack_frame();
    }
    while (is_expected_ack(f.ack))
    {
        //dbg_frame("AC   ACK %d\n", frame_send_end);
        stop_timer(frame_send_end);
        nbuffered--;
        frame_send_end = (frame_send_end + 1) % WINDOW_NUMBER;
    }
    if (!is_expected_ack(send_now))
    {
        send_now = frame_send_end;
    }
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Designed by Secone, build: " __DATE__ "  " __TIME__ "\n");

    disable_network_layer();

    for (;;)
    {
        event = wait_for_event(&arg);

        switch (event)
        {
        case NETWORK_LAYER_READY:
            get_packet(buffer);
            nbuffered++;
            save_data_frame();
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0)
            {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }
            process_frame(f, len);
            break;

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg);
            send_now = arg;
            send_data_frame();
            break;
        case ACK_TIMEOUT:
            send_ack_frame();
            break;
        }

        if (nbuffered < SLIDE_WINDOW_SIZE)
            enable_network_layer();
        else
            disable_network_layer();
    }
}
