#include <wait.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/random.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>
#include <sys/socket.h>
#include <stdatomic.h>

#include "util.h"
#include "client.h"
#include "protocol.h"

#define TIME_OUT_MS 100
#define TASK_COUNT 512
/* ------------------ client ---------------------- */
enum TASK_STATE{
	NONE = 0,
	WAITING_FD,
	WORKING,
	TIMEOUT,
};

struct server_fd_obj;

struct gcc_task
{
	char state;
	struct sockaddr_un client_un;
	struct timeval time;
	uint64_t heart_times;
	struct server_fd_obj *fd_obj;
} tasks[TASK_COUNT] = {0};

struct server_fd_obj
{
	int fd;
	struct gcc_task *task;
};

struct server_info
{
	char addr[64];
	uint16_t port;
	uint32_t ts_count;
	struct server_fd_obj fd_objs[512];
	uint8_t compression;
} server_info[128] = {0};

int connect_new(struct sockaddr_in *server_addr)
{
	int socket_fd = -1;

	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0)
	{
		logerr("create socket error!\n");
		return -1;
	}

	if(connect(socket_fd, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0)
	{
		logerr("connect:%s error!\n", inet_ntoa(server_addr->sin_addr));
		close(socket_fd);
		return -1;
	}

	printf("connected %s:%d fd:%d\n", inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port), socket_fd);

	return socket_fd;
}

void disable_task(struct gcc_task *task)
{
	unlink(task->client_un.sun_path);
	if(task->fd_obj)
	{
		task->fd_obj->task = NULL;
		task->fd_obj = NULL;
	}

	task->state = NONE;
}

void *task_daemon(void *data)
{
	int i, j;
	uint32_t task_id;
	struct timeval now;
	struct gcc_task *task;
	uint32_t send_data[2] = {0};
	int socket_fd = *(int *)data;
	struct server_fd_obj *send_fd_obj;
	struct sockaddr_in server_addr = {0};

	while(1)
	{
		task = NULL;
		gettimeofday(&now, NULL);
		for (i = 0; i < TASK_COUNT; ++i)
		{
			if(tasks[i].state != NONE)
			{
				if(timesub_ms(tasks[i].time, now) > TIME_OUT_MS)
				{
					loginfo("time out:%d\n", i);
					disable_task(&tasks[i]);
				}
			}

			if(!task && tasks[i].state == WAITING_FD)
			{
				task_id = i;
				task = &tasks[i];
			}
		}

		if(task == NULL)
		{
			loginfo("idle\n");
			usleep(20000);
			continue;
		}

		send_fd_obj = NULL;
		for (i = 0; i < 128; ++i)
		{
			for (j = 0; j < 512; ++j)
			{
				if(!server_info[i].fd_objs[j].fd &&
					!server_info[i].fd_objs[j].task)
				{
					server_addr.sin_family = AF_INET;
					server_addr.sin_port = htons(server_info[i].port);
					server_addr.sin_addr.s_addr = inet_addr(server_info[i].addr);
					server_info[i].fd_objs[j].fd = connect_new(&server_addr);
					if(server_info[i].fd_objs[j].fd == -1)
						server_info[i].fd_objs[j].fd = 0;
				}

				if(server_info[i].fd_objs[j].fd &&
					!server_info[i].fd_objs[j].task)
				{
					send_fd_obj = &server_info[i].fd_objs[j];
					goto find;
				}
			}
		}

		loginfo("not fd\n");
		usleep(20000);
		continue;
find:
		send_data[0] = CMD_OK;
		send_data[1] = server_info[i].compression << 16 | (task_id & 0xffff);
		if(msg_send_fd(socket_fd, &task->client_un, send_fd_obj->fd, (char *)&send_data, sizeof(send_data)) == 0)
			printf("send fd:%d task_id:%d\n", send_fd_obj->fd, task_id);
		else {
			logerr("send fail fd:%d task_id:%d socket:%s\n", send_fd_obj->fd, task_id, task->client_un.sun_path);
			disable_task(task);
			continue;
		}

		/* change controller */
		close(send_fd_obj->fd);
		send_fd_obj->fd = 0;
		send_fd_obj->task = task;
		task->fd_obj = send_fd_obj;
		task->state = WORKING;
	}
	return NULL;
}

int client(int argc, char **argv)
{
	int i, j;
	int ret = 0;
	char *tmp;
	pthread_t thr;
	uint32_t com_ps;
	uint32_t task_id;
	int ser_idx = 0;
	int socket_fd = -1;
	char compression[32];
	uint32_t recv_data[2] = {0};
	struct sockaddr_un server_un = {0};
	struct sockaddr_un client_un = {0};


	for (i = 2; i < argc; ++i)
	{
		if(strcmp(argv[i], "-h") == 0)
		{
			i++;

			tmp = strchr(argv[i], ',');
			if(tmp != NULL)
			{
				strcpy(compression, tmp + 1);
				if(strcmp(compression, "lz4") == 0)
					server_info[ser_idx].compression = COMPRESS_LZ4;

				*tmp = '\0';
			}

			tmp = strchr(argv[i], '/');
			if(tmp != NULL)
			{
				server_info[ser_idx].ts_count = atoi(tmp + 1);
				*tmp = '\0';
			} else {
				server_info[ser_idx].ts_count = 1;
			}

			tmp = strchr(argv[i], ':');
			if(tmp != NULL)
			{
				server_info[ser_idx].port = atoi(tmp + 1);
				*tmp = '\0';
			} else {
				server_info[ser_idx].port = 3633;
			}

			strcpy(server_info[ser_idx].addr, argv[i]);
			ser_idx++;
			continue;
		}
	}

	if(ser_idx == 0)
	{
		logerr("no host given.\n");
		ret = -1;
		goto out;
	}

	socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);

	server_un.sun_family = AF_UNIX;
	strcpy(server_un.sun_path, MSG_ADDR);
	unlink(server_un.sun_path);

	if (bind(socket_fd, (struct sockaddr*)&server_un, sizeof(server_un)) < 0) {
		printf("bind failed\n");
		ret = -1;
		goto out;
	}

	pthread_create(&thr, NULL, task_daemon, &socket_fd);

	struct msghdr recv_msg = {0};
	struct iovec data_iov = {&recv_data, sizeof(recv_data)};
	char cmsg[CMSG_SPACE(sizeof(int))];

	while(1)
	{
		recv_msg.msg_name = &client_un;
		recv_msg.msg_namelen = sizeof(client_un);
		recv_msg.msg_iov = &data_iov;
		recv_msg.msg_iovlen = 1;
		recv_msg.msg_control = &cmsg;
		recv_msg.msg_controllen = sizeof(cmsg);

		ret = recvmsg(socket_fd, &recv_msg, 0);
		if(ret <= 0)
		{
			logerr("recv err ret:%d %s\n", ret, strerror(errno));
			ret = -1;
			goto out;
		}

		switch(recv_data[0])
		{
			case GET_FD:
			{
				for (i = 0; i < TASK_COUNT; ++i)
				{
					if(tasks[i].state == NONE)
					{
						gettimeofday(&tasks[i].time, NULL);
						memcpy(&tasks[i].client_un, &client_un, sizeof(client_un));
						tasks[i].state = WAITING_FD;
						tasks[i].heart_times = 0;
						logdebug("new task_id:%d socket:%s\n", i, client_un.sun_path);
						break;
					}
				}
			}
			break;
			case HEART_BEAT:
				task_id = recv_data[1];
				if(tasks[task_id].state != NONE)
				{
					gettimeofday(&tasks[task_id].time, NULL);
					tasks[task_id].heart_times++;
				}
			break;
			case PUT_FD:
				task_id = recv_data[1] & 0xffff;
				com_ps = recv_data[1] >> 16;
				if(tasks[task_id].state != NONE)
				{
					struct cmsghdr *ctrl = CMSG_FIRSTHDR(&recv_msg);
					printf("put  fd:%d task_id:%-3d comp perc:%d%%\n", *(int*)CMSG_DATA(ctrl), task_id , com_ps);
					tasks[task_id].fd_obj->fd = *(int*)CMSG_DATA(ctrl);
					tasks[task_id].fd_obj->task = NULL;
					tasks[task_id].fd_obj = NULL;
					tasks[task_id].state = NONE;
				}
			break;
			default:
				logerr("unknow cmd:%d\n", recv_data[0]);
			break;
		}
	}

out:
	return ret;
}