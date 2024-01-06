#include <lz4.h>
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
#include "protocol.h"

int wait_start(FILE *socket_fp)
{
	char read_byte;
	char zero_count = 0;

	for (int i = 0; i < 1024 * 1024; ++i)
	{
		if(fread_all(socket_fp, &read_byte, 1) != 0)
		{
			logerr("wait start, read fail.\n");
			return -1;
		}

		if(read_byte == 2)
			zero_count++;
		else
			zero_count = 0;

		if(zero_count == 4)
			return 0;
	}

	logerr("wait start, time out.\n");
	return -1;
}

#define FILE_BUF_SIZE 0x10000
int recv_remote_file(FILE *socket_fp, char *file_name, char *rename)
{
	int ret = 0;
	int fd = -1;
	int read_len;
	int data_len;
	int decom_size;
	char *write_file;
	uint32_t compress;
	uint32_t params_len;
	uint32_t net_compress;
	char *recv_buf = NULL;
	char *decom_buf = NULL;
	int net_compress_size;
	char *remote_output_file = NULL;

	recv_buf = malloc(FILE_BUF_SIZE);
	if(recv_buf == NULL)
	{
		logerr("recv_remote_file malloc fail\n");
		ret = -1;
		goto out;
	}

	remote_output_file = malloc(256);
	if(remote_output_file == NULL)
	{
		logerr("recv_remote_file malloc fail\n");
		ret = -1;
		goto out;
	}

	if(rename)
		write_file = rename;
	else
		write_file = file_name;

	if(fread_all(socket_fp, (char *)&net_compress, 4) != 0)
	{
		logerr("send_remote_file write fail.\n");
		ret = -1;
		goto out;
	}
	compress = ntohl(net_compress);

	if(compress)
	{
		decom_buf = malloc(FILE_BUF_SIZE);
		if(decom_buf == NULL)
		{
			logerr("recv_remote_file malloc fail\n");
			ret = -1;
			goto out;
		}
	}

	if(fread_all(socket_fp, (char *)&params_len, 4) != 0)
	{
		logerr("fread_all filename_len fail\n");
		goto out;
	}

	if(params_len == 0)
	{
		logerr("recv compiler out file fail, file name len is 0.\n");
		ret = -1;
		goto out;
	}

	if(fread_all(socket_fp, remote_output_file, params_len) != 0)
	{
		logerr("fread_all remote_output_file fail\n");
		goto out;
	}

	if(file_name)
	{
		if(strcmp(file_name, remote_output_file) != 0)
		{
			logerr("recv compiler out file fail, file name is not same want:%s recv:%s.\n", file_name, remote_output_file);
			ret = -1;
			goto out;
		}
	} else {
		write_file = remote_output_file;
	}

	fd = open(write_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if(fd == -1)
	{
		logerr("recv compiler out file fail, open file:%s err:%s\n", write_file, strerror(errno));
		ret = -1;
		goto out;
	}

	if(fread_all(socket_fp, (char *)&params_len, 4) != 0)
	{
		logerr("fread_all file len fail\n");
		goto out;
	}

	if(params_len == 0)
	{
		logerr("file len is 0.\n");
		goto out;
	}

	while(params_len)
	{
		read_len = params_len > FILE_BUF_SIZE ? FILE_BUF_SIZE : params_len;
		switch(compress)
		{
			case COMPRESS_NONE:
				
				ret = fread_all(socket_fp, recv_buf, read_len);
				if(ret == 0)
				{
					if(write_all(fd, recv_buf, read_len) != 0)
						logerr("recv_remote_file write fail.\n");

					params_len -= read_len;
				} else {
					ret = -1;
					goto out;
				}
			break;
			case COMPRESS_LZ4:
				ret = fread_all(socket_fp, (char *)&net_compress_size, 4);
				if(ret != 0)
				{
					logerr("recv_remote_file fread net_compress_size all fail.\n");
					ret = -1;
					goto out;
				}

				data_len = ntohl(net_compress_size);
				ret = fread_all(socket_fp, recv_buf, data_len);
				if(ret != 0)
				{
					logerr("recv_remote_file fread recv_buf all fail. data_len:%d\n", data_len);
					ret = -1;
					goto out;
				}

				decom_size = LZ4_decompress_safe(recv_buf, decom_buf, data_len, read_len);
				if(decom_size <= 0)
				{
					logerr("LZ4 decompress fail. decom_size:%d data_len:%d\n", decom_size, data_len);
					ret = -1;
					goto out;
				}

				if(decom_size != read_len)
				{
					logerr("LZ4 decompress decom_size != read_len.\n");
					ret = -1;
					goto out;
				}

				if(write_all(fd, decom_buf, decom_size) != 0)
					logerr("recv_remote_file write fail.\n");

				params_len -= decom_size;
			break;
		}

	}

	logfile("recv   file:%s %s.\n", write_file, ret == 0 ? "success" : "fail");

out:
	if(fd != -1)
		close(fd);

	if(recv_buf)
		free(recv_buf);

	if(remote_output_file)
		free(remote_output_file);

	return ret;
}

int send_remote_file(FILE *socket_fp, char *file_name, char *rename, uint32_t compress)
{
	int ret = 0;
	int read_len;
	int data_len;
	FILE *fp = NULL;
	int net_compress_size;
	uint32_t net_compress;
	uint32_t file_name_len;
	char *read_buffer = NULL;
	char *com_buffer = NULL;

	if(compress)
		com_buffer = malloc(FILE_BUF_SIZE);

	read_buffer = malloc(FILE_BUF_SIZE);
	if(read_buffer == NULL)
	{
		logerr("recv_remote_file malloc fail\n");
		ret = -1;
		goto out;
	}

	net_compress = htonl(compress);
	if(fwrite_all(socket_fp, (char *)&net_compress, 4) != 0)
	{
		logerr("send_remote_file write fail.\n");
		ret = -1;
		goto out;
	}

	if(rename)
	{
		file_name_len = strlen(rename) + 1;
		if(fwrite_all(socket_fp, (char *)&file_name_len, 4) != 0)
		{
			logerr("send_remote_file write fail.\n");
			ret = -1;
			goto out;
		}
		if(fwrite_all(socket_fp, rename, file_name_len) != 0)
		{
			logerr("send_remote_file write fail.\n");
			ret = -1;
			goto out;
		}
	} else {
		file_name_len = strlen(file_name) + 1;
		if(fwrite_all(socket_fp, (char *)&file_name_len, 4) != 0)
		{
			logerr("send_remote_file write fail.\n");
			ret = -1;
			goto out;
		}
		if(fwrite_all(socket_fp, file_name, file_name_len) != 0)
		{
			logerr("send_remote_file write fail.\n");
			ret = -1;
			goto out;
		}
	}

	fp = fopen(file_name, "r");
	if(!fp)
	{
		logerr("send file, open file:%s fail.\n%s\n", file_name, strerror(errno));
		ret = -1;
		goto out;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		logerr("fseek failed: %s\n", strerror(errno));
		ret = -1;
		goto out;
	}

	uint32_t file_size = ftell(fp);
	if (file_size == -1) {
		printf("ftell failed :%s\n", strerror(errno));
		ret = -1;
		goto out;
	}

	if(fwrite_all(socket_fp, (char *)&file_size, 4) != 0)
	{
		logerr("send_remote_file write fail.\n");
		ret = -1;
		goto out;
	}

	if (fseek(fp, 0, SEEK_SET) != 0) {
		logerr("fseek failed: %s\n", strerror(errno));
		ret = -1;
		goto out;
	}
	while(!feof(fp)){
		read_len = file_size > FILE_BUF_SIZE ? FILE_BUF_SIZE : file_size;
		if(read_len == 0)
			break;

		ret = fread_all(fp, read_buffer, read_len);
		if(ret == 0)
		{
			file_size -= read_len;

			switch(compress)
			{
				case COMPRESS_NONE:
					if(fwrite_all(socket_fp, read_buffer, read_len) != 0)
					{
						logerr("send_remote_file write fail.\n");
						ret = -1;
						goto out;
					}
				break;
				case COMPRESS_LZ4:
					data_len = LZ4_compress_fast(read_buffer, com_buffer, read_len, read_len, 1);
					if(data_len <= 0)
					{
						logwarning("LZ4 compress fail. read_len:%d data_len:%d\n", read_len, data_len);
						ret = -1;
						goto out;
					} else {
						net_compress_size = htonl(data_len);

						logdebug("send compress size:%d read_len:%d compress:%d%%\n", data_len, read_len, data_len * 100 / read_len);
						
						compress_record(data_len * 100 / read_len);

						if(fwrite_all(socket_fp, (char *)&net_compress_size, 4) != 0)
						{
							logerr("send_remote_file write fail.\n");
							ret = -1;
							goto out;
						}

						if(fwrite_all(socket_fp, com_buffer, data_len) != 0)
						{
							logerr("send_remote_file write fail.\n");
							ret = -1;
							goto out;
						}
					}
				break;
			}

		} else {
			logerr("file:%s read fail. read_len:%d ret:%d\n", file_name, read_len, ret);
			ret = -1;
			goto out;
		}
	}

out:
	if(fp)
		fclose(fp);

	if(read_buffer)
		free(read_buffer);

	return ret;
}

int delete_remote_file(FILE *socket_fp, char *file_name)
{
	char cmd;
	uint32_t file_name_len;

	/* ----------------------- cmd start ---------------------- */
	if(fwrite_all(socket_fp, START_CODE, 8) != 0)
	{
		logerr("delete_remote_file write all fail.\n");
		return -1;
	}
	cmd = DELETE_FILE;
	if(fwrite_all(socket_fp, &cmd, 1) != 0)
	{
		logerr("delete_remote_file write all fail.\n");
		return -1;
	}

	file_name_len = strlen(file_name) + 1;
	if(fwrite_all(socket_fp, (char *)&file_name_len, 4) != 0)
	{
		logerr("delete_remote_file write fail.\n");
		return -1;
	}
	if(fwrite_all(socket_fp, file_name, file_name_len) != 0)
	{
		logerr("delete_remote_file write fail.\n");
		return -1;
	}
	fflush(socket_fp);

	if(fread_all(socket_fp, &cmd, 1) != 0)
	{
		logerr("delete_remote_file read all fail.\n");
		return -1;
	}
	if(cmd != CMD_OK)
	{
		logerr("delete file fail.\n");
		return -1;
	}
	/* ----------------------- cmd end ---------------------- */

	return 0;
}

int send_params(FILE *socket_fp, char **params, uint32_t params_count)
{
	uint32_t params_len;

	if(fwrite_all(socket_fp, (char *)&params_count, 4) != 0)
	{
		logerr("send_params write fail.\n");
		return -1;
	}

	for (int i = 0; i < params_count; ++i)
	{
		/* 算上结束符 */
		params_len = strlen(params[i]) + 1;
		if(fwrite_all(socket_fp, (char *)&params_len, 4) != 0)
		{
			logerr("send_params write fail.\n");
			return -1;
		}
		if(fwrite_all(socket_fp, params[i], params_len) != 0)
		{
			logerr("send_params write fail.\n");
			return -1;
		}
	}
	return 0;
}

int recv_params(FILE *socket_fp, 	char **params, uint32_t *params_count)
{
	uint32_t params_len;
	char *params_item;
	
	if(fread_all(socket_fp, (char *)params_count, 4) != 0)
	{
		logerr("read_all recv_params params_count fail\n");
		*params_count = 0;
		return -1;
	}

	loginfo("params count:%u\n", *params_count);
	for (int i = 0; i < *params_count; ++i)
	{
		if(fread_all(socket_fp, (char *)&params_len, 4) != 0)
		{
			logerr("read_all recv_params params_len fail\n");
			*params_count = 0;
			return -1;
		}
		params_item = malloc(params_len + 1);
		if(params_item == NULL)
		{
			logerr("recv_params malloc fail\n");
			*params_count = 0;
			return -1;
		}
		if(fread_all(socket_fp, params_item, params_len) != 0)
		{
			logerr("read_all recv_params params_item fail\n");
			*params_count = 0;
			return -1;
		}
		params[i] = params_item;
	}
	params[*params_count] = NULL;

	return 0;
}

int msg_send_fd(int socket_fd, struct sockaddr_un *client_un, int fd, char *data, size_t len)
{
	struct msghdr send_msg = {0};
	send_msg.msg_name = client_un;
	send_msg.msg_namelen = sizeof(*client_un);
	struct iovec data_iov = {data, len};
	send_msg.msg_iov = &data_iov;
	send_msg.msg_iovlen = 1;
	char cmsg_send[CMSG_SPACE(sizeof(int))];
	send_msg.msg_control = &cmsg_send;
	send_msg.msg_controllen = sizeof(cmsg_send);
	struct cmsghdr *control = CMSG_FIRSTHDR(&send_msg);
	control->cmsg_level = SOL_SOCKET;
	control->cmsg_type = SCM_RIGHTS;
	control->cmsg_len = CMSG_LEN(sizeof(int));
	*(int*)CMSG_DATA(control) = fd;

	if(sendmsg(socket_fd, &send_msg, 0) <= 0)
	{
		logerr("sendmsg fail, %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int msg_send_data(int socket_fd, struct sockaddr_un *client_un, char *data, size_t len)
{
	struct msghdr send_msg = {0};
	send_msg.msg_name = client_un;
	send_msg.msg_namelen = sizeof(*client_un);
	struct iovec data_iov = {data, len};
	send_msg.msg_iov = &data_iov;
	send_msg.msg_iovlen = 1;

	if(sendmsg(socket_fd, &send_msg, 0) <= 0)
	{
		logerr("sendmsg fail, %s\n", strerror(errno));
		return -1;
	}

	return 0;
}