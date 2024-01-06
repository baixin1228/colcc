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
#include "server.h"
#include "protocol.h"

/* ------------------ server ---------------------- */
int server_recv_file(FILE *socket_fp)
{
	return recv_remote_file(socket_fp, NULL, NULL, true);
}

int server_compiler(FILE *socket_fp)
{
	int ret = 0;
	char **params;
	uint32_t params_count;
	
	params = malloc(1024);
	if(params == NULL)
	{
		logerr("server_compiler malloc fail\n");
		return -1;
	}

	ret = recv_params(socket_fp, params, &params_count);
	if(ret != 0)
		goto out1;

	ret = mini_exec(params_count, params, true, -1);
	if(ret != 0)
	{
		logerr("exec fail.\n");
		ret = -1;
		goto out2;
	}
out2:
	for (int i = 0; i < params_count; ++i)
	{
		free(params[i]);
	}
out1:
	if(params)
		free(params);

	return ret;
}

int server_send_file(FILE *socket_fp)
{
	int ret;
	char *file_name;
	uint32_t file_name_len;

	file_name = malloc(256);
	if(file_name == NULL)
	{
		logerr("server_send_file malloc fail\n");
		return -1;
	}

	if(fread_all(socket_fp, (char *)&file_name_len, 4) != 0)
	{
		logerr("fread_all file_name_len fail\n");
		ret = -1;
		goto out;
	}
	if(file_name_len == 0)
	{
		logerr("server_send_file file_name_len is 0\n");
		ret = -1;
		goto out;
	}
	if(fread_all(socket_fp, file_name, file_name_len) != 0)
	{
		logerr("fread_all file_name fail\n");
		ret = -1;
		goto out;
	}
	
	loginfo("send file:%s\n", file_name);

	ret = send_remote_file(socket_fp, file_name, NULL);
out:
	if(file_name)
		free(file_name);

	return ret;
}

int server_delete_file(FILE *socket_fp)
{
	int ret;
	char *file_name;
	uint32_t file_name_len;

	file_name = malloc(256);
	if(file_name == NULL)
	{
		logerr("server_delete_file malloc fail\n");
		return -1;
	}

	if(fread_all(socket_fp, (char *)&file_name_len, 4) != 0)
	{
		logerr("delete file fail, fread_all fail\n");
		ret = -1;
		goto out;
	}

	if(file_name_len == 0)
	{
		logerr("delete file fail, file name len is 0.\n");
		ret = -1;
		goto out;
	}

	if(fread_all(socket_fp, file_name, file_name_len) != 0)
	{
		logerr("delete file fail, fread_all fail\n");
		ret = -1;
		goto out;
	}

	ret = delete_file(file_name);

	logfile("delete file:%s %s.\n", file_name, ret == 0 ? "success" : "fail");
out:
	if(file_name)
		free(file_name);

	return ret;
}

atomic_ulong client_fps[256] = {0};
void *server_new_client_run(void *data)
{
	char cmd;
	char cmd_ret;
	uint64_t client_idx = (uint64_t)data;
	FILE *client_fp = (FILE *)atomic_load(&client_fps[client_idx]);

	logdebug("thread:%lu start.\n", client_idx);

	while(1)
	{
		if(wait_start(client_fp) != 0)
		{
			logerr("wait_start fail, thread idx:%lu\n", client_idx);
			goto out;
		}

		if(fread_all(client_fp, &cmd, 1) != 0)
		{
			logerr("fread_all fail, thread idx:%lu\n", client_idx);
			goto out;
		}

		switch(cmd)
		{
			case PUSH_FILE:
				logprotocol("--- thread: %lu  push   ---\n", client_idx);
				if(server_recv_file(client_fp) == 0)
					cmd_ret = CMD_OK;
				else
					cmd_ret = CMD_ERR;
			break;
			case COMPILER:
				logprotocol("--- thread: %lu compiler ---\n", client_idx);
				if(server_compiler(client_fp) == 0)
					cmd_ret = CMD_OK;
				else
					cmd_ret = CMD_ERR;
			break;
			case PULL_FILE:
				logprotocol("--- thread: %lu   pull   ---\n", client_idx);
				if(server_send_file(client_fp) == 0)
					cmd_ret = CMD_OK;
				else
					cmd_ret = CMD_ERR;
			break;
			case DELETE_FILE:
				logprotocol("--- thread: %lu  delete  ---\n", client_idx);
				if(server_delete_file(client_fp) == 0)
					cmd_ret = CMD_OK;
				else
					cmd_ret = CMD_ERR;
			break;
			default:
				logerr("unknow cmd:%d\n", cmd);
				cmd_ret = CMD_ERR;
			break;
		}
		fwrite_all(client_fp, &cmd_ret, 1);
		fflush(client_fp);
		logdebug("<-- response fflush -->\n");
	}

out:
	fclose(client_fp);
	atomic_store(&client_fps[client_idx], 0);
	loginfo("thread: %lu exit.\n", client_idx);
	return NULL;
}

int server(int argc, char **argv)
{
	int server_fd;
	int client_fd;
	char *listen_addr = "0.0.0.0";
	uint16_t listen_port = 3633;
	int addrlen = sizeof(struct sockaddr);
	struct sockaddr_in server_addr = {0};
	struct sockaddr client_addr;
	pthread_t thread;

	for (int i = 2; i < argc; ++i)
	{
		if(strcmp(argv[i], "-l") == 0)
		{
			i++;
			listen_addr = argv[i];
			continue;
		}
		if(strcmp(argv[i], "-p") == 0)
		{
			i++;
			listen_port = atoi(argv[i]);
			continue;
		}
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(listen_port);
	server_addr.sin_addr.s_addr = inet_addr(listen_addr);

	loginfo("addr:%s port:%d\n", listen_addr, listen_port);

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0)
	{
		logerr("create socket error!\n");
		exit(1);
	}

	if (bind(server_fd, (struct sockaddr *)&server_addr, addrlen) < 0)
	{
		logerr("addr:%s port:%d bind error!\n", inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));
		exit(1);
	}

	if (listen(server_fd, 256) < 0)
	{
		logerr("listen error!\n");
		exit(1);
	}

	while(1)
	{
		client_fd = accept(server_fd, &client_addr, &addrlen);
		if (client_fd < 0)
		{
			logerr("accept error!\n");
			exit(1);
		}

		while(1)
		{
			for (uint64_t i = 0; i < 256; ++i)
			{
				if(atomic_load(&client_fps[i]) == 0)
				{
					atomic_store(&client_fps[i], (uint64_t)fdopen(client_fd, "rb+"));
					pthread_create(&thread, NULL, server_new_client_run, (void *)i);
					goto out;
				}
			}
			usleep(50000);
		}
out:
	}

	close(server_fd);
	return 0;
}