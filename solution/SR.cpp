#include <string.h>


#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 3100
#define ACK_TIMER 1150
#define WINDOW_NUMBER 32
#define SLIDE_WINDOW_SIZE 16
#define WITH_ACK true

struct FRAME {
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN];
    unsigned int  padding;
};

static unsigned char buffer[PKT_LEN], nbuffered; // 发送缓冲区 仅一帧
static unsigned char frame_send_end = 0, frame_send_head = 0; // 标识队列的头尾
static unsigned char frame_expected = 0, frame_receive_head = 0;
static int phl_ready = 0; // 物理层准备
static unsigned char frame_send[WINDOW_NUMBER * PKT_LEN]; // 发送的缓冲区
static unsigned char frame_receive[WINDOW_NUMBER * PKT_LEN]; // 接收的缓冲区
static bool receive_cache[WINDOW_NUMBER] = { false }; // 标志收集到什么帧
static bool no_nak = false;
static bool ack_flag[WINDOW_NUMBER] = { false }; // 标志发送了谁的 ack
static unsigned char send_now = 0; // 发送帧的序号

static void put_frame(unsigned char *frame, int len) {
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    stop_ack_timer();
    phl_ready = 0;
}

static void send_data_frame(void) {
    struct FRAME s;
    s.kind = FRAME_DATA;
    s.seq = send_now;
    // 通过判断代替 (x + window_number - 1) % window_number
    s.ack = frame_expected == 0 ? WINDOW_NUMBER - 1 : frame_expected - 1; 
    // 从发送缓冲区内拷出发送内容
    memcpy(s.data, frame_send + send_now * PKT_LEN, PKT_LEN);
    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    // 开始当前的数据帧计时器
    start_timer(send_now, DATA_TIMER);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
    // 发送序号+1
    send_now = (send_now + 1) % WINDOW_NUMBER;
}

static void save_data_frame(void) { // 先保存数据帧再进行发送
    memcpy(frame_send + frame_send_head * PKT_LEN, buffer, PKT_LEN);
    //dbg_frame("Save DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
    send_data_frame();
    frame_send_head = (frame_send_head + 1) % WINDOW_NUMBER;
}

static void send_ack_frame(void) {
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = frame_expected == 0 ? WINDOW_NUMBER - 1 : frame_expected - 1;

    dbg_frame("Ask to send ACK  %d\n", s.ack);
    if (!ack_flag[s.ack]) {
        dbg_frame("Send ACK  %d\n", s.ack);
        ack_flag[s.ack] = true;
        put_frame((unsigned char *)&s, 2);
    }

}

static void send_nak_frame() {
    if (!no_nak) {
        no_nak = true;

        struct FRAME s;
        s.kind = FRAME_NAK;
        s.ack = frame_expected;

        dbg_frame("Send NAK  %d\n", s.ack);
        put_frame((unsigned char *)&s, 2);
    }
    else {
        //no_nak = false;
        start_ack_timer(ACK_TIMER);
    }
    return;
}

static bool is_bewteen(unsigned char l, unsigned char pos, unsigned char r) {
    if (r < l) { // 判断窗口 动态调整判断范围
        r += WINDOW_NUMBER;
    }
    if (pos < l) {
        pos += WINDOW_NUMBER;
    }
    return l <= pos && pos < r; // 是一个左闭右开区间
}

static void process_frame(struct FRAME &f, int len) {
    if (f.kind == FRAME_NAK) {
        dbg_frame("Recv NAK  %d\n", f.ack);
        if (is_bewteen(frame_send_end, f.ack, frame_send_head)) {
            unsigned char send_now_temp = send_now;
            send_now = f.ack; // 重新发送对应帧
            send_data_frame();
            send_now = send_now_temp;
        }
        f.ack = (f.ack + WINDOW_NUMBER - 1) % WINDOW_NUMBER; // 对ack编号+1
    }
    if (f.kind == FRAME_ACK)
        dbg_frame("Recv ACK  %d\n", f.ack);
    if (f.kind == FRAME_DATA) {
        dbg_frame("Recv DATA %d %d, ID %d. Expect: %d\n", f.seq, f.ack, *(short *)f.data, frame_expected);
        if (f.seq != frame_expected) {
            send_nak_frame();
        }
        if (is_bewteen(frame_expected, f.seq, frame_expected + SLIDE_WINDOW_SIZE)) { // 上限固定为 frame_expected + 滑动窗口大小
            memcpy(&frame_receive[f.seq * PKT_LEN], f.data, PKT_LEN); // 保存在缓冲区
            receive_cache[f.seq] = true;
            no_nak = true;
            ack_flag[f.seq] = true;
        }
        unsigned char i = f.seq;
        while (i == frame_expected && receive_cache[i]) { // 呈递之前缓存的帧
            //dbg_frame("Use saved frame %d\n", i);
            put_packet(&frame_receive[i * PKT_LEN], PKT_LEN);
            no_nak = false;
            ack_flag[i] = false;
            receive_cache[i] = false;
            frame_expected = (frame_expected + 1) % WINDOW_NUMBER;
            i = (i + 1) % WINDOW_NUMBER;
        }
        start_ack_timer(ACK_TIMER);
    }
    while (is_bewteen(frame_send_end, f.ack, frame_send_head + 1)) {
        dbg_frame("AC   ACK %d\n", frame_send_end); // 通过ack记录移动接收区间
        stop_timer(frame_send_end);
        --nbuffered;
        frame_send_end = (frame_send_end + 1) % WINDOW_NUMBER;
    }
}

int main(int argc, char **argv) {
    int event, arg;
    struct FRAME f;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Designed by Secone, build: " __DATE__"  " __TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);
        unsigned char send_now_temp = send_now;

        switch (event) {
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
                if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                    dbg_event("**** Receiver Error, Bad CRC Checksum.\n");
                    send_nak_frame();
                    break;
                }
                process_frame(f, len);
                break;

            case DATA_TIMEOUT:
                dbg_event("---- DATA %d timeout\n", arg);
                send_now = arg;
                send_data_frame();
                send_now = send_now_temp; // 在发送一个帧后恢复之前的帧序号
                break;

            case ACK_TIMEOUT:
                send_ack_frame();
                break;
        }

        if (nbuffered < SLIDE_WINDOW_SIZE  && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
    }
}
