#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2000

#define MAX_SEQ 15
#define MAX_WINDOW_SIZE 16

int phl_ready = 0;

struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
};

struct PACKET
{
	unsigned char content[PKT_LEN];
};

static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_data_frame(int frame_number, int frame_expected, unsigned char buffer[])
{
	struct FRAME s;
	s.kind = FRAME_DATA;
	s.seq = frame_number;
	s.ack = (frame_expected + MAX_SEQ) % MAX_WINDOW_SIZE;
	memcpy(s.data, buffer, PKT_LEN);

	dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

	put_frame((unsigned char*)&s, PKT_LEN + 3);
	start_timer(frame_number, DATA_TIMER);
}

static void send_ack_frame(int frame_expected)
{
	struct FRAME s;
	s.kind = FRAME_ACK;
	s.ack = (frame_expected + MAX_SEQ) % MAX_WINDOW_SIZE;

	dbg_frame("Send ACK %d\n", s.ack);

	put_frame((unsigned char*)&s, 2);
}

static bool between(int a, int b, int c)
{
	if (((a <= b) && (b < c)) || (c < a) && (a <= b) || ((b < c) && (c < a)))
		return true;
	else
		return false;
}

inline int inc(int next_frame_to_send)
{
	return (next_frame_to_send + 1) % MAX_WINDOW_SIZE;
}

int main(int argc, char** argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	int next_frame_to_send = 0, ack_expected = 0, frame_expected = 0, nbuffered = 0, i = 0;
	struct PACKET buffer[MAX_WINDOW_SIZE];

	disable_network_layer();

	protocol_init(argc, argv);
	lprintf("Designed by Jiang Yanjun, build:" __DATE__" "__TIME__ "\n");

	while (true)
	{
		event = wait_for_event(&arg);
		// lprintf("Get event %d\n", event);
		switch (event)
		{
		case NETWORK_LAYER_READY:
			get_packet(buffer[next_frame_to_send].content);
			nbuffered += 1;
			send_data_frame(next_frame_to_send, frame_expected, buffer[next_frame_to_send].content);
			next_frame_to_send = inc(next_frame_to_send);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char *)&f, sizeof(f));
			if(len < 5 || crc32((unsigned char *)&f, len) != 0)
			{
				dbg_event("**** Receive Error, Bad CRC Checksum ID %d****\n", *(short*)f.data);
				break;
			}
			//dbg_frame("Get packet %d %d ID %d\n", f.seq, f.ack, *(short*)f.data);
			if (f.kind == FRAME_ACK) 
			{
				dbg_frame("Recv ACK %d", f.ack);
			}
			if(f.kind == FRAME_DATA)
			{
				dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)f.data);
				if(f.seq == frame_expected)
				{
					//dbg_event("deal with ID %d\n", *(short*)f.data);
					put_packet(f.data, len - 7);
					dbg_event("!!!To network layer successfully ID %d\n", *(short*)f.data);
					frame_expected = inc(frame_expected);
				}
			}
			while (between(ack_expected, f.ack, next_frame_to_send))
			{
				nbuffered -= 1;
				stop_timer(ack_expected);
				ack_expected = inc(ack_expected);
			}
			break;

		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			dbg_event("start piggy backing from %d to %d\n", ack_expected, next_frame_to_send);
			next_frame_to_send = ack_expected;
			for(i = 1; i <= nbuffered; i++)
			{
				send_data_frame(next_frame_to_send, frame_expected, buffer[next_frame_to_send].content);
				next_frame_to_send = inc(next_frame_to_send);
			}
			break;
		}

		if (nbuffered < MAX_SEQ && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();

		dbg_event("current frame_expected:%d next_frame_to_send: %d\n", frame_expected, next_frame_to_send);
	}
}

