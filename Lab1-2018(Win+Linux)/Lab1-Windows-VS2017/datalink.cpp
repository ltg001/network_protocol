#include <string.h>
#include <stdbool.h>
#include "protocol.h"
#include "datalink.h"

//#define DATA_TIMER 3100
//#define ACK_TIMER 1150
//#define WINDOW_NUMBER 32

#define WITH_ACK true

int SLIDE_WINDOW_SIZE;
int DATA_TIMER = 3100;
int ACK_TIMER = 1150;
int WINDOW_NUMBER = 32;

double output_bps;

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
static unsigned char frame_expected = 0, frame_receive_head = 0;
static int phl_ready = 0;
static unsigned char* frame_send;
static unsigned char* frame_receive;
static bool* receive_cache;
static bool no_nak = false;
static bool* ack_flag;
static unsigned char send_now = 0;

void protocol6_init() {
	frame_send = new unsigned char[WINDOW_NUMBER * PKT_LEN];
	frame_receive = new unsigned char[WINDOW_NUMBER * PKT_LEN];
	receive_cache = new bool[WINDOW_NUMBER];
	ack_flag = new bool[WINDOW_NUMBER];
	SLIDE_WINDOW_SIZE = WINDOW_NUMBER / 2;

	for (int i = 0; i < WINDOW_NUMBER; i++) {
		receive_cache[i] = false;
		ack_flag[i] = false;
	}

	printf("current DATA_TIMER, ACK_TIMER, WINDOW_NUMBER, SLIDE_WINDOW_SIZE = %d, %d, %d, %d\n", DATA_TIMER, ACK_TIMER, WINDOW_NUMBER, SLIDE_WINDOW_SIZE);
}

static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	stop_ack_timer();
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

	dbg_frame("Ask to send ACK  %d\n", s.ack);
	if (!ack_flag[s.ack])
	{
		dbg_frame("Send ACK  %d\n", s.ack);
		ack_flag[s.ack] = true;
		put_frame((unsigned char *)&s, 2);
	}
}

static void send_nak_frame()
{
	if (!no_nak)
	{
		no_nak = true;

		struct FRAME s;
		s.kind = FRAME_NAK;
		s.ack = frame_expected;

		dbg_frame("Send NAK  %d\n", s.ack);
		put_frame((unsigned char *)&s, 2);
	}
	else
	{
		//no_nak = false;
		start_ack_timer(ACK_TIMER);
	}
	return;
}

static bool is_bewteen(unsigned char l, unsigned char pos, unsigned char r)
{
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
	if (f.kind == FRAME_NAK)
	{
		dbg_frame("Recv NAK  %d\n", f.ack);
		if (is_bewteen(frame_send_end, f.ack, frame_send_head))
		{
			auto send_now_temp = send_now;
			send_now = f.ack;
			send_data_frame();
			send_now = send_now_temp;
		}
		f.ack = (f.ack + WINDOW_NUMBER - 1) % WINDOW_NUMBER;
	}
	if (f.kind == FRAME_ACK)
		dbg_frame("Recv ACK  %d\n", f.ack);
	if (f.kind == FRAME_DATA)
	{
		dbg_frame("Recv DATA %d %d, ID %d. Expect: %d\n", f.seq, f.ack, *(short *)f.data, frame_expected);
		if (f.seq != frame_expected)
		{
			send_nak_frame();
		}
		if (is_bewteen(frame_expected, f.seq, frame_expected + SLIDE_WINDOW_SIZE))
		{
			memcpy(&frame_receive[f.seq * PKT_LEN], f.data, PKT_LEN);
			receive_cache[f.seq] = true;
			no_nak = true;
			ack_flag[f.seq] = true;
		}
		auto i = f.seq;
		while (i == frame_expected && receive_cache[i])
		{
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
	while (is_bewteen(frame_send_end, f.ack, frame_send_head + 1))
	{
		dbg_frame("AC   ACK %d\n", frame_send_end);
		stop_timer(frame_send_end);
		--nbuffered;
		frame_send_end = (frame_send_end + 1) % WINDOW_NUMBER;
	}
}

int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	protocol6_init();
	lprintf("Designed by Shell Zhang & Windy Zhou, build: " __DATE__ "  " __TIME__ "\n");
	lprintf("current DATA_TIMER, ACK_TIMER, WINDOW_NUMBER = %d, %d, %d", DATA_TIMER, ACK_TIMER, WINDOW_NUMBER);

	disable_network_layer();

	for (;;)
	{
		event = wait_for_event(&arg);
		auto send_now_temp = send_now;

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
			send_now = send_now_temp;
			break;

		case ACK_TIMEOUT:
			send_ack_frame();
			break;
		}

		if (nbuffered < SLIDE_WINDOW_SIZE && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();

		//printf("[%.4f]\n", output_bps);
	}
}
