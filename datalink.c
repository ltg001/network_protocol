#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2000
#define MAX_SEQ 15
#define MAX_WINDOW_SIZE 16
#define NR_BUFS (MAX_WINDOW_SIZE) /2


bool no_nak = true;
bool arrive[NR_BUFS];

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

static void send_all_frame(char frame_kind, int frame_number, int frame_expected, unsigned char buffer[]) {
	struct FRAME s;
	s.kind = frame_kind;
	if (frame_kind == FRAME_DATA) {
		s.seq = frame_number;
		s.ack = (frame_expected + MAX_SEQ) % MAX_WINDOW_SIZE;
		memcpy(s.data, buffer[frame_number%NR_BUFS], PKT_LEN);  //change

		dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
		put_frame((unsigned char*)&s, PKT_LEN + 3);
		start_timer(frame_number%NR_BUFS, DATA_TIMER);  //change
	}
	if (frame_kind == FRAME_ACK) {
		s.ack = (frame_expected + MAX_SEQ) % MAX_WINDOW_SIZE;
		dbg_frame("Send ACK  %d\n", s.ack);
		put_frame((unsigned char*)&s, 2);
	}
	if (frame_kind = FRAME_NAK) {
		no_nak = false;
		s.ack = (frame_expected + MAX_SEQ) % MAX_WINDOW_SIZE;

		dbg_frame("Send NAK  %d\n", s.ack);
		put_frame((unsigned char*)&s, 2);
	}
	stop_ack_timer();
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

void protocol_6(void) {
	int ack_expected = 0;
	int next_frame_to_send = 0;
	int frame_expected = 0;
	int too_far = NR_BUFS;    //change 
	int nbuffer = 0;
	int oldest_frame = MAX_SEQ + 1;

	int i;
	struct FRAME r;

	struct PACKET in_buf[NR_BUFS];
	struct PACKET out_buf[NR_BUFS];

	int event, arg;
	int len = 0;

	for (i = 0; i < NR_BUFS; i++)
		arrive[i] = false;

	disable_network_layer();

	while (true)
	{
		event = wait_for_event(&arg);
		switch (event)
		{
		case NETWORK_LAYER_READY:
			nbuffer += 1;
			get_packet(out_buf[next_frame_to_send % NR_BUFS].content);
			send_all_frame(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);//change
			next_frame_to_send = inc(next_frame_to_send);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&r, sizeof(r));

			if (len < 5 || crc32((unsigned char *)&r, len) != 0)
			{
				dbg_event("**** Receive Error, Bad CRC Checksum ID %d****\n", *(short*)r.data);
				if (no_nak)
					send_all_frame(FRAME_NAK, 0, frame_expected, out_buf);
				break;
			}

			if (r.kind == FRAME_DATA) {
				dbg_frame("Recv DATA %d %d, ID %d\n", r.seq, r.ack, *(short*)r.data);
				if (r.seq != frame_expected && no_nak) {
					send_all_frame(FRAME_NAK, 0, frame_expected, out_buf);
				}
				else {
					start_ack_timer(DATA_TIMER);
				}

				if (between(frame_expected, r.seq, too_far) && arrive[r.seq % NR_BUFS] == false) {
					arrive[r.seq % NR_BUFS] = true;//change
					in_buf[r.seq % NR_BUFS] = r.content;
					while (arrive[frame_expected %NR_BUFS]) {
						put_packet(&in_buf[r.seq%NR_BUFS], len - 7);
						dbg_event("!!!To network layer successfully ID %d\n", *(short*)r.data);
						no_nak = true;
						arrive[frame_expected % NR_BUFS] = false;
						frame_expected = inc(frame_expected);
						too_far = inc(too_far);
						start_ack_timer(DATA_TIMER);
					}
				}
			}
			if (r.kind == FRAME_NAK) {
				if (between(ack_expected, (r.ack + 1) % MAX_WINDOW_SIZE, next_frame_to_send))
					dbg_event("response NAK to %d", r.ack + 1);
					send_all_frame(FRAME_DATA, (r.ack + 1) % MAX_WINDOW_SIZE, frame_expected, out_buf);
			}
			while (between(ack_expected, r.ack, next_frame_to_send))
			{
				nbuffer -= 1;
				stop_timer(ack_expected % NR_BUFS);
				ack_expected = inc(ack_expected);
			}
			break;

		case ACK_TIMEOUT:
			send_all_frame(FRAME_ACK, 0, frame_expected, out_buf[frame_expected].content);
			break;
		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			/*next_frame_to_send = ack_expected;
			for (int i = 0; i < nbuffer; i++) {
				send_all_frame(FRAME_DATA, next_frame_to_send, frame_expected, out_buf[next_frame_to_send % NR_BUFS].content);
				next_frame_to_send = inc(next_frame_to_send);
			}*/
			send_all_frame(FRAME_DATA, oldest_frame, frame_expected, out_buf);
			break;
		default:
			break;
		}
		if (nbuffer < NR_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();

		dbg_event("current frame_expected:%d next_frame_to_send: %d\n", frame_expected, next_frame_to_send);
	}
}
// phl_sq_len
int main(int argc, char** argv)
{
	//int event, arg;
	//struct FRAME f;
	//int len = 0;

	//int next_frame_to_send = 0, ack_expected = 0, frame_expected = 0, nbuffered = 0, i = 0;
	//struct PACKET buffer[MAX_WINDOW_SIZE];

	//disable_network_layer();

	protocol_init(argc, argv);
	lprintf("Designed by Jiang Yanjun, build:" __DATE__" "__TIME__ "\n");

	protocol_6();

	//while (true)
	//{
	//	event = wait_for_event(&arg);
	//	// lprintf("Get event %d\n", event);
	//	switch (event)
	//	{
	//	case NETWORK_LAYER_READY:
	//		get_packet(buffer[next_frame_to_send].content);
	//		nbuffered += 1;
	//		send_data_frame(next_frame_to_send, frame_expected, buffer[next_frame_to_send].content);
	//		next_frame_to_send = inc(next_frame_to_send);
	//		break;

	//	case PHYSICAL_LAYER_READY:
	//		phl_ready = 1;
	//		break;

	//	case FRAME_RECEIVED:
	//		len = recv_frame((unsigned char *)&f, sizeof(f));
	//		if(len < 5 || crc32((unsigned char *)&f, len) != 0)
	//		{
	//			dbg_event("**** Receive Error, Bad CRC Checksum ID %d****\n", *(short*)f.data);
	//			break;
	//		}
	//		//dbg_frame("Get packet %d %d ID %d\n", f.seq, f.ack, *(short*)f.data);
	//		if (f.kind == FRAME_ACK) 
	//		{
	//			dbg_frame("Recv ACK %d", f.ack);
	//		}
	//		if(f.kind == FRAME_DATA)
	//		{
	//			dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)f.data);
	//			if(f.seq == frame_expected)
	//			{
	//				//dbg_event("deal with ID %d\n", *(short*)f.data);
	//				put_packet(f.data, len - 7);
	//				dbg_event("!!!To network layer successfully ID %d\n", *(short*)f.data);
	//				frame_expected = inc(frame_expected);
	//			}
	//		}
	//		while (between(ack_expected, f.ack, next_frame_to_send))
	//		{
	//			nbuffered -= 1;
	//			stop_timer(ack_expected);
	//			ack_expected = inc(ack_expected);
	//		}
	//		break;

	//	case DATA_TIMEOUT:
	//		dbg_event("---- DATA %d timeout\n", arg);
	//		dbg_event("start piggy backing from %d to %d\n", ack_expected, next_frame_to_send);
	//		next_frame_to_send = ack_expected;
	//		for(i = 1; i <= nbuffered; i++)
	//		{
	//			send_data_frame(next_frame_to_send, frame_expected, buffer[next_frame_to_send].content);
	//			next_frame_to_send = inc(next_frame_to_send);
	//		}
	//		break;
	//	}

	//	if (nbuffered < MAX_SEQ && phl_ready)
	//		enable_network_layer();
	//	else
	//		disable_network_layer();

	//	dbg_event("current frame_expected:%d next_frame_to_send: %d\n", frame_expected, next_frame_to_send);
	//}
}

