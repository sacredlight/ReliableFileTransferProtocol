#include "../global.h"
#include "send.h"
#include "semaphore.h"

#define WINDOW_SIZE 1000
#define MAX_PAYLOAD 2048

static int sock;
static sem_t mutex;
static sockaddr *sin_send;
static sockaddr *sin_recv;
static bool stop;

struct sendQueue;
struct sendQueue {
	unsigned int seq;
	char* pck;
	int pckLen;
	bool hasACK;
	struct sendQueue *next;
};
struct sendQueue *head;

/* get the first packet that contains file information */
char* getFirstPacket(char* filename, unsigned long long fLen) {
	int length = 12 + sizeof(char) * strlen(filename);
	unsigned int high = fLen >> 32;
	unsigned int low = fLen & 0x0ffffffff;
	char* packet = (char*)malloc(length + 20);

	*(unsigned int*)(packet+16) = 0;			// sequence number and flags
	*(unsigned int*)(packet+20) = htonl(strlen(filename));	// length of file name
	*(unsigned int*)(packet+24) = htonl(high);		// high 32-bit of file size
	*(unsigned int*)(packet+28) = htonl(low);		// low 32-bit of file size
	memcpy(packet+32, filename, strlen(filename));

	//printf("first packet:\nfilename: %s, high-bit of size: %d, low-bit of size: %d\n", filename, high, low);

	md5((uint8_t *) (packet+16), length+4, (uint8_t *) packet);
    
	return packet;
}

/* get a packet from specific position in a file */
char* getBigPacket(FILE *fp, unsigned int seq, unsigned short length, int status){
    char* packet;
    unsigned int header;
    
    packet = (char*)malloc(sizeof(char) * (length + 20));
    
    fseek(fp, (seq-1) * MAX_PAYLOAD, SEEK_SET);
    fread(packet + 20, 1, length, fp);
    
    if(status == 1)
        header = (seq << 2)| 0x2;
    else
        header = seq << 2;
    
    *(unsigned int*)(packet+16) = htonl(header);
    
    md5((uint8_t *) (packet+16), length+4, (uint8_t *) packet);
    
    return packet;
}

void *send_thread_big(void *argv) {
	while (head != NULL) {
		if (stop)
			return NULL;
		// sending packets
		sem_wait(&mutex);
		struct sendQueue *cur = head;
		while (cur != NULL) {
			if (!cur->hasACK)
				sendto(sock, cur->pck, cur->pckLen, 0, sin_send, sizeof(struct sockaddr));
			cur = cur->next;
		}
		sem_post(&mutex);
	}
}

/* check the received acknowledgement for big file */
bool parseACK_big(char *recvACK)
{
	uint8_t r[16];
	md5((uint8_t *)(recvACK+16), 4, r);
	return packetCorrect((uint8_t *) recvACK, r);
}

/* free the memory resource used by sending queue */
void freeQueue() {
	while (head != NULL) {
		struct sendQueue *cur = head->next;
		free(head->pck);
		free(head);
		head = cur;
	}
}

/* start to send big file, return true if succeed */
bool engage_big(int sock_num, struct sockaddr *sock_send, struct sockaddr *sock_recv, FILE* fp, char* fileName, unsigned long long fLen) {
	sock = sock_num;
	sin_send = sock_send;
	sin_recv = sock_recv;
	stop = false;

	unsigned int minWait = 0, maxWait = (unsigned int)(fLen / MAX_PAYLOAD) + 1;
	unsigned short lastpckLength = (unsigned short)(fLen % MAX_PAYLOAD);
	if (lastpckLength == 0) {
		lastpckLength = MAX_PAYLOAD;
		maxWait--;
	}

	/* construct the sending queue */
	// the first packet indicates information for the file
	head = (struct sendQueue*)malloc(sizeof(struct sendQueue));
	head->seq = 0;
	head->pck = getFirstPacket(fileName, fLen);
	head->pckLen = strlen(fileName) + 32;
	head->hasACK = false;
	head->next = NULL;
	struct sendQueue* tail = head;

	// following packets contain data in the file
	int i = 1;	// sequence number of packet
	while (i < WINDOW_SIZE && i <= maxWait) {
		struct sendQueue *cur = (struct sendQueue*)malloc(sizeof(struct sendQueue));
		cur->seq = i;
		if (i != maxWait) {
			cur->pck = getBigPacket(fp, i, MAX_PAYLOAD, 0);
			cur->pckLen = MAX_PAYLOAD + 20;
		}
		else {
			cur->pck = getBigPacket(fp, i, lastpckLength, 1);
			cur->pckLen = lastpckLength + 20;
		}
		cur->hasACK = false;
		cur->next = NULL;

		tail->next = cur;
		tail = cur;
		i++;
	}

	/* semaphore to prevent race between sending thread and main */
	sem_init(&mutex, 0, 1);

	pthread_t send_tid;
	int rc = pthread_create(&send_tid, NULL, send_thread_big, NULL);
	if (rc != 0) {
		fprintf(stderr, "*Error* cannot create a new thread to send packets.\n");
		return false;
	}

	bool complete = false;
	int timeoutCount = 1;
	int recvCount;
	char *recvBuf = (char*)malloc(sizeof(char) * 20);
	unsigned int sockaddr_len = sizeof(struct sockaddr);
	while (!complete) {
		recvCount = recvfrom(sock, recvBuf, 20, 0, sin_recv, &sockaddr_len);
		if (recvCount < 0) {
			fprintf(stderr, "Time out occurs... (no ACK in %d secondes)\n", (timeoutCount++)*5);
			if (timeoutCount > 10) {
				stop = true;
				break;
			}
			else continue;
		}
		else if (recvCount == 20 && parseACK_big(recvBuf)) {
			unsigned int seq = ntohl(*(unsigned int*)(recvBuf+16));
			if (seq == maxWait + 1)
				complete = true;
			else if (seq == minWait) {
				head->hasACK = true;
				// find next minWait and update sending queue
				sem_wait(&mutex);
				while (head != NULL && head->hasACK) {
					if (tail->seq < maxWait) {
						// add a new packet to be sent
						struct sendQueue *toAdd = (struct sendQueue*)malloc(sizeof(struct sendQueue));
						unsigned int newSeq = tail->seq + 1;
						toAdd->seq = newSeq;
						if (newSeq != maxWait) {
							toAdd->pck = getBigPacket(fp, newSeq, MAX_PAYLOAD, 0);
							toAdd->pckLen = MAX_PAYLOAD + 20;
						}
						else {
							toAdd->pck = getBigPacket(fp, newSeq, lastpckLength, 1);
							toAdd->pckLen = lastpckLength + 20;
						}
						toAdd->hasACK = false;
						toAdd->next = NULL;

						tail->next = toAdd;
						tail = toAdd;
					}

					// remove the head
					struct sendQueue *newHead = head->next;
					free(head->pck);
					free(head);
					head = newHead;
					minWait++;
				}
				sem_post(&mutex);
			}
			else if (seq > minWait && seq < minWait + WINDOW_SIZE) {
				struct sendQueue *cur = head;
				while (cur->seq != seq)
					cur = cur->next;
				cur->hasACK = true;
			}
		} // end if

		timeoutCount = 0;
	} // end while

	// clean up
	delete recvBuf;
	freeQueue();
	pthread_join(send_tid, NULL);

	return complete;
}
