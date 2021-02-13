#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <network.h>
#include <errno.h>

#include <wiiuse/wpad.h>

#include "ntp.h"
#include "sysconf.h"

extern u32 __SYS_GetRTC(u32 *gctime);

void *initialise();
void *ntp_client(void *arg);

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static lwp_t ntp_handle = (lwp_t) NULL;

#define NO_RETRIES 20

int main(int argc, char **argv) {
	s32 ret;
	struct in_addr hostip;

	xfb = initialise();

	printf ("\nNTP client demo\n");
	printf("Configuring network. Continously press home key to abort\n");

	net_deinit();

	ret = SYSCONF_Init();
	if (ret < 0) {
		printf("Failed to init sysconf and settings.txt. Err: %d\n", ret);
		exit(1);
	}

	for (int i = 0; i < NO_RETRIES; i++) {
		WPAD_ScanPads();

		int buttonsDown = WPAD_ButtonsDown(0);

		if (buttonsDown & WPAD_BUTTON_HOME) {
			printf("Home button pressed. Exiting...\n");
			exit(0);
		}

		printf("\rTry: %d", i);
		ret = net_init();
		if ((ret == -EAGAIN) || (ret == -ETIMEDOUT)) {
			usleep(50 * 1000);
			continue;
		}
		break;
	}
	printf("\n");

	if (ret < 0) {
		printf ("Network configuration failed: %d:%s. Aborting!\n", ret, strerror(ret));
		exit(0);
	}

	hostip.s_addr = net_gethostip();
	if (hostip.s_addr == 0) {
		printf("Failed to get configured ip address. Aborting!\n");
		exit(0);
	}

	printf("Network configured, ip: %s\n", inet_ntoa(hostip));

	if (LWP_CreateThread(&ntp_handle,	/* thread handle */
					 ntp_client,	/* code */
					 NULL,		    /* arg pointer for thread */
					 NULL,			/* stack base */
					 16*1024,		/* stack size */
					 50				/* thread priority */ ) < 0) {
		printf("Failed to create ntp clint thread. Aborting!\n");
		exit(0);
	}

	while (true) {
		VIDEO_WaitVSync();
		WPAD_ScanPads();

		int buttonsDown = WPAD_ButtonsDown(0);

		if (buttonsDown & WPAD_BUTTON_HOME) {
			printf("Home button pressed. Exiting...\n");
			exit(0);
		}
	}

	return 0;
}

void *ntp_client(void *arg) {
	int sockfd, n;
	ntp_packet packet;
	struct sockaddr_in serv_addr;
	struct hostent *server;
	uint32_t rtc_s;
	uint64_t local_time;
	uint64_t ntp_time_in_gc_epoch;
	u32 bias;

	memset(&packet, 0, sizeof(ntp_packet));

	// Set the first byte's bits to 00,011,011 for li = 0, vn = 3, and mode = 3
	*((char *) &packet + 0) = 0b00011011;

	// Cannot use IPPROTO_UDP, this will return error 121
	sockfd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

	if (sockfd < 0) {
		printf("Failed to create socket %d:%s. Aborting!\n", sockfd, strerror(sockfd));
		return NULL;
	}

	server = net_gethostbyname(NTP_HOST);
	if (server == NULL) {
		printf("Failed to resolve ntp host %s. Errno: %d, %s. Aborting!\n", NTP_HOST, errno, strerror(errno));
		return NULL;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;

	memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

	printf("Resolved %s to %s\n", NTP_HOST, inet_ntoa(serv_addr.sin_addr));

	serv_addr.sin_port = htons(NTP_PORT);

	for (int i = 0; i < 50; i++) {
		n = net_connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
		if (n < 0) {
			printf("Failed to establish connection to NTP server. Err: %d:%s, Aborting!\n", n, strerror(n));
			usleep(50 * 1000);
		} else {
			break;
		}
	}
	if (n < 0) {
		printf("Giving up\n");
		return NULL;
	}

	n = net_write(sockfd, &packet, sizeof(ntp_packet));
	if (n != sizeof(ntp_packet)) {
		printf("Failed to write the full ntp packet. Err: %d:%s. Aborting!\n", n, strerror(n));
		return NULL;
	}

	n = net_read(sockfd, &packet, sizeof(ntp_packet));
	if (n < 0) {
		if (n < 0) {
			printf("Error while receiving ntp packet. Err: %d:%s. Aborting!\n", n, strerror(n));
		} else {
			printf("Did not receive full ntp packet. Got %d bytes. Aborting!\n", n);
		}
		return NULL;
	}

	/* Swap seconds to host byte order */
	packet.txTm_s = ntohl(packet.txTm_s);

	n = __SYS_GetRTC(&rtc_s);

	time_t txTm = (time_t) (packet.txTm_s - NTP_TO_UNIX_EPOCH_DELTA);
	printf("Time: %s", ctime(&txTm));

	ntp_time_in_gc_epoch = packet.txTm_s - NTP_TO_GC_EPOCH_DELTA;

	n = SYSCONF_GetCounterBias(&bias);
	if (n < 0) {
		printf("%s:%d. Failed to get counter bias. Err: %d. Aborting!\n", __FILE__, __LINE__, n);
		return NULL;
	}

	local_time = rtc_s + bias;
	printf("ntp time: %llu, local time: %llu, diff: %lld\n", ntp_time_in_gc_epoch, local_time, ntp_time_in_gc_epoch - local_time);

	printf("Decreasing counter bias by one hour (3600 s)\n");

	n = SYSCONF_SetCounterBias(bias - 3600);
	if (n < 0) {
		printf("Failed to reset counter bias. Err: %d. Aborting!\n", n);
		return NULL;
	}

	printf("Rereading counter bias\n");
	n = SYSCONF_GetCounterBias(&bias);
	if (n < 0) {
		printf("Failed to get counter bias. Err: %d. Aborting!\n", n);
		return NULL;
	}

	local_time = rtc_s + bias;
	printf("ntp time: %llu, local time: %llu, diff: %lld\n", ntp_time_in_gc_epoch, local_time, ntp_time_in_gc_epoch - local_time);

	return NULL;
}

//---------------------------------------------------------------------------------
void *initialise() {
//---------------------------------------------------------------------------------

	void *framebuffer;

	VIDEO_Init();
	WPAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(framebuffer,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) {
		VIDEO_WaitVSync();
	}

	return framebuffer;

}
