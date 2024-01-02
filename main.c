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
#include <sys/time.h>
#include <sys/random.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>
#include <sys/socket.h>
#include <stdatomic.h>

#include "util.h"

int debug = 0;
int debug_protocol = 0;
int debug_file = 0;
FILE *log_fp = NULL;
FILE *logerr_fp = NULL;
char *work_dir = NULL;
char *host_addr = NULL;
uint16_t host_port = 3633;
const char *gcc_spec_params[] = 
	{
		"-Xassembler",
		"-Xpreprocessor",
		"-Xlinker",
		"-B",
		"-x"
	};

int to_gcc_local(int argc, char **argv)
{
	uint32_t params_count = 0;
	static char *params[256] = {NULL};

	for (int i = 1; i < argc; ++i)
	{
		params[params_count] = argv[i];
		params_count++;
	}

	return mini_exec(params_count, params, false, -1);
}

struct remote_arg
{
	char *e_params[1024];
	uint32_t e_params_count;
	char *c_params[1024];
	uint32_t c_params_count;
	char *l_params[1024];
	uint32_t l_params_count;
	char *link_lib_params[512];
	uint32_t link_lib_count;

	FILE *socket_fp;
	bool local;
	/* 编译到那一步结束 */
	bool e;
	bool s;
	bool c;
};

enum {
	PUSH_FILE = 1,
	COMPILER,
	PULL_FILE,
	DELETE_FILE,
	CMD_OK,
	CMD_ERR
};

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

int recv_remote_file(FILE *socket_fp, char *file_name, char *rename, bool safe_read)
{
	int ret = 0;
	int fd = -1;
	char *write_file;
	uint32_t params_len;
	char *recv_buf = NULL;
	char *remote_output_file = NULL;

	recv_buf = malloc(4096);
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

	if(safe_fread_all(socket_fp, (char *)&params_len, 4) != 0)
	{
		logerr("safe_fread_all fail\n");
		goto out;
	}

	if(params_len == 0)
	{
		logerr("recv compiler out file fail, file name len is 0.\n");
		ret = -1;
		goto out;
	}

	if(safe_fread_all(socket_fp, remote_output_file, params_len) != 0)
	{
		logerr("safe_fread_all fail\n");
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

	if(safe_fread_all(socket_fp, (char *)&params_len, 4) != 0)
	{
		logerr("safe_fread_all fail\n");
		goto out;
	}

	if(params_len == 0)
	{
		logerr("file len is 0.\n");
		goto out;
	}

	while(params_len)
	{
		int read_len = params_len > 4096 ? 4096 : params_len;
		if(safe_read)
			ret = safe_fread_all(socket_fp, recv_buf, read_len);
		else
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

int send_remote_file(FILE *socket_fp, char *file_name, char *rename)
{
	int ret = 0;
	int read_len;
	FILE *fp = NULL;
	uint32_t file_name_len;
	char *read_buffer = NULL;

	read_buffer = malloc(4096);
	if(read_buffer == NULL)
	{
		logerr("recv_remote_file malloc fail\n");
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
        read_len = fread(read_buffer, 1, 4096, fp);
        if(read_len > 0)
        {
        	if(fwrite_all(socket_fp, read_buffer, read_len) != 0)
			{
				logerr("send_remote_file write fail.\n");
				ret = -1;
				goto out;
			}
        } else {
			logerr("file:%s read fail.\n", file_name);
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
	if(fwrite_all(socket_fp, "\x03\x03\x03\x03\x02\x02\x02\x02", 8) != 0)
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
	
	if(safe_fread_all(socket_fp, (char *)params_count, 4) != 0)
	{
		logerr("read_all recv_params params_count fail\n");
		*params_count = 0;
		return -1;
	}

	loginfo("params count:%u\n", *params_count);
	for (int i = 0; i < *params_count; ++i)
	{
		if(safe_fread_all(socket_fp, (char *)&params_len, 4) != 0)
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
		if(safe_fread_all(socket_fp, params_item, params_len) != 0)
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

int to_gcc_link(struct remote_arg *remote_arg, char **input_files, char *output_file)
{
	for (int i = 0; i < 128; ++i)
	{
		if(!input_files[i])
			break;

		remote_arg->l_params[remote_arg->l_params_count] = input_files[i];
		remote_arg->l_params_count++;
	}

	remote_arg->l_params[remote_arg->l_params_count] = "-o";
	remote_arg->l_params_count++;
	remote_arg->l_params[remote_arg->l_params_count] = output_file;
	remote_arg->l_params_count++;

	for (int i = 0; i < remote_arg->link_lib_count; ++i)
	{

		remote_arg->l_params[remote_arg->l_params_count] = remote_arg->link_lib_params[i];
		remote_arg->l_params_count++;
	}
	return mini_exec(remote_arg->l_params_count, remote_arg->l_params, true, -1);
}

int to_gcc_compiler(struct remote_arg *remote_arg, char *input_file, char *output_file)
{
	int ret;
	char cmd;
	static char c_output_file[256];
	static char c_remote_file[256];

	c_output_file[0] = 0;
	c_remote_file[0] = 0;

	char *rel_output_file;
	uint32_t params_len;

	remote_arg->c_params[remote_arg->c_params_count] = "-c";
	remote_arg->c_params_count++;
	remote_arg->c_params[remote_arg->c_params_count] = input_file;
	remote_arg->c_params_count++;

	if(remote_arg->local)
	{
		if(remote_arg->c) {
			rel_output_file = output_file;
		} else {
			strcpy(c_output_file, "/tmp/colcc/");
			get_random_str(c_output_file + 11, 16);
			strcpy(c_output_file + 27, ".o");
			rel_output_file = c_output_file;
		}

		remote_arg->c_params[remote_arg->c_params_count] = "-o";
		remote_arg->c_params_count++;
		remote_arg->c_params[remote_arg->c_params_count] = rel_output_file;
		remote_arg->c_params_count++;

		ret = mini_exec(remote_arg->c_params_count, remote_arg->c_params, true, -1);
		if(ret != 0)
			goto out;
	} else {
		strcpy(c_output_file, "/tmp/colcc/");
		get_random_str(c_output_file + 11, 16);
		strcpy(c_output_file + 27, ".o");
		rel_output_file = c_output_file;

		strcpy(c_remote_file, "/tmp/colcc/");
		get_random_str(c_remote_file + 11, 16);
		strcpy(c_remote_file + 27, ".o");

		remote_arg->c_params[remote_arg->c_params_count] = "-o";
		remote_arg->c_params_count++;
		remote_arg->c_params[remote_arg->c_params_count] = c_remote_file;
		remote_arg->c_params_count++;

		/* ----------------------- cmd start ---------------------- */
		if(fwrite_all(remote_arg->socket_fp, "\x03\x03\x03\x03\x02\x02\x02\x02", 8) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		cmd = COMPILER;
		if(fwrite_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		if(send_params(remote_arg->socket_fp, remote_arg->c_params, remote_arg->c_params_count) != 0)
		{
			logerr("to_gcc_compiler send_params fail.\n");
			ret = -1;
			goto out;
		}
		fflush(remote_arg->socket_fp);
		if(fread_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		if(cmd != CMD_OK)
		{
			logerr("remote compiler fail.\n");
			ret = -1;
			goto out;
		}
		/* ----------------------- cmd end ---------------------- */

		/* ----------------------- cmd start ---------------------- */
		if(fwrite_all(remote_arg->socket_fp, "\x03\x03\x03\x03\x02\x02\x02\x02", 8) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		cmd = PULL_FILE;
		if(fwrite_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		/* 算上结束符 */
		params_len = strlen(c_remote_file) + 1;
		if(fwrite_all(remote_arg->socket_fp, (char *)&params_len, 4) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		if(fwrite_all(remote_arg->socket_fp, c_remote_file, params_len) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		fflush(remote_arg->socket_fp);

		if(remote_arg->c)
			ret = recv_remote_file(remote_arg->socket_fp, c_remote_file, output_file, false);
		else
			ret = recv_remote_file(remote_arg->socket_fp, c_remote_file, c_output_file, false);
		if(ret != 0)
			goto out;

		if(fread_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		if(cmd != CMD_OK)
		{
			logerr("pull file fail.\n");
			ret = -1;
			goto out;
		}
		/* ----------------------- cmd end ---------------------- */
	}

	if(remote_arg->c)
		goto out;

	char *input_files[2] = {NULL};
	input_files[0] = c_output_file;

	ret = to_gcc_link(remote_arg, input_files, output_file);

out:
	if(remote_arg->local && c_output_file[0])
	{
		int del_ret = delete_file(c_output_file);
		logfile("delete file:%s %s.\n", c_output_file, del_ret == 0 ? "success" : "fail");
	}

	if(!remote_arg->local && c_remote_file[0])
		delete_remote_file(remote_arg->socket_fp, c_remote_file);

	return ret;
}

int to_gcc_direct_compiler(struct remote_arg *remote_arg, char *input_file, char *output_file)
{
	int ret;
	char cmd;
	static char e_remote_file[256];

	if(!remote_arg->local)
	{
		strcpy(e_remote_file, "/tmp/colcc/");
		get_random_str(e_remote_file + 11, 16);
		strcpy(e_remote_file + 27, ".i");
		/* ----------------------- cmd start ---------------------- */
		if(fwrite_all(remote_arg->socket_fp, "\x03\x03\x03\x03\x02\x02\x02\x02", 8) != 0)
		{
			logerr("to_gcc_direct_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		cmd = PUSH_FILE;
		if(fwrite_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_direct_compiler write all fail.\n");
			ret = -1;
			goto out;
		}

		if(send_remote_file(remote_arg->socket_fp, input_file, e_remote_file) != 0)
		{
			logerr("send remote file fail.\n");
			return -1;
		}
		fflush(remote_arg->socket_fp);

		if(fread_all(remote_arg->socket_fp, &cmd, 1) != 0)
		{
			logerr("to_gcc_compiler write all fail.\n");
			ret = -1;
			goto out;
		}
		if(cmd != CMD_OK)
		{
			logerr("push file fail.\n");
			ret = -1;
			goto out;
		}
		/* ----------------------- cmd end ---------------------- */
		ret = to_gcc_compiler(remote_arg, e_remote_file, output_file);

		delete_remote_file(remote_arg->socket_fp, e_remote_file);
	} else {
		ret = to_gcc_compiler(remote_arg, input_file, output_file);
	}

out:
	return ret;
}

int to_gcc_pretreatment(struct remote_arg *remote_arg, char *input_file, char *output_file)
{
	int ret;
	char cmd;
	char *compiler_input_file;
	static char e_output_file[256];
	static char e_remote_file[256];
	
	e_output_file[0] = 0;
	e_remote_file[0] = 0;

	if(remote_arg->s)
	{
		remote_arg->e_params[remote_arg->e_params_count] = "-S";
		remote_arg->e_params_count++;
	} else {
		remote_arg->e_params[remote_arg->e_params_count] = "-E";
		remote_arg->e_params_count++;
		// remote_arg->e_params[remote_arg->e_params_count] = "-fdirectives-only";
		// remote_arg->e_params_count++;
	}

	remote_arg->e_params[remote_arg->e_params_count] = input_file;
	remote_arg->e_params_count++;



	if(remote_arg->e || remote_arg->s) {
		remote_arg->e_params[remote_arg->e_params_count] = "-o";
		remote_arg->e_params_count++;
		remote_arg->e_params[remote_arg->e_params_count] = output_file;
		remote_arg->e_params_count++;
	} else {
		strcpy(e_output_file, "/tmp/colcc/");
		get_random_str(e_output_file + 11, 16);
		strcpy(e_output_file + 27, ".i");

		remote_arg->e_params[remote_arg->e_params_count] = "-o";
		remote_arg->e_params_count++;
		remote_arg->e_params[remote_arg->e_params_count] = e_output_file;
		remote_arg->e_params_count++;
		compiler_input_file = e_output_file;
	}

	ret = mini_exec(remote_arg->e_params_count, remote_arg->e_params, true, -1);
	if(ret != 0)
		goto out;

	if(remote_arg->e || remote_arg->s)
		goto out;
	else {
		if(!remote_arg->local)
		{
			strcpy(e_remote_file, "/tmp/colcc/");
			get_random_str(e_remote_file + 11, 16);
			strcpy(e_remote_file + 27, ".i");
			compiler_input_file = e_remote_file;
			/* ----------------------- cmd start ---------------------- */
			if(fwrite_all(remote_arg->socket_fp, "\x03\x03\x03\x03\x02\x02\x02\x02", 8) != 0)
			{
				logerr("to_gcc_pretreatment write all fail.\n");
				ret = -1;
				goto out;
			}
			cmd = PUSH_FILE;
			if(fwrite_all(remote_arg->socket_fp, &cmd, 1) != 0)
			{
				logerr("to_gcc_pretreatment write all fail.\n");
				ret = -1;
				goto out;
			}
			
			ret = send_remote_file(remote_arg->socket_fp, e_output_file, e_remote_file);
			if(ret != 0)
			{
				ret = -1;
				goto out;
			}
			fflush(remote_arg->socket_fp);

			if(fread_all(remote_arg->socket_fp, &cmd, 1) != 0)
			{
				logerr("to_gcc_pretreatment write all fail.\n");
				ret = -1;
				goto out;
			}
			if(cmd != CMD_OK)
			{
				logerr("push file fail.\n");
				ret = -1;
				goto out;
			}
			/* ----------------------- cmd end ---------------------- */
		}
	}

	ret = to_gcc_compiler(remote_arg, compiler_input_file, output_file);

out:
	if(e_output_file[0])
	{
		int del_ret = delete_file(e_output_file);
		logfile("delete file:%s %s.\n", e_output_file, del_ret == 0 ? "success" : "fail");
	}

	if(!remote_arg->local && e_remote_file[0])
		delete_remote_file(remote_arg->socket_fp, e_remote_file);

	return ret;
}

int to_gcc_remote(int argc, char **argv)
{
	int ret;
	int socket_fd;
	char *local_env;
	int input_file_count = 0;
	static char *input_files[128] = {NULL};
	char *output_file = NULL;
	struct sockaddr_in serv_addr = {0};

	static struct remote_arg remote_arg = {0};

	remote_arg.e_params[remote_arg.e_params_count] = "gcc";
	remote_arg.e_params_count++;
	remote_arg.c_params[remote_arg.c_params_count] = "gcc";
	remote_arg.c_params_count++;
	remote_arg.l_params[remote_arg.l_params_count] = "gcc";
	remote_arg.l_params_count++;

	for (int i = 2; i < argc; ++i)
	{

		if(strcmp("-E", argv[i]) == 0)
		{
			remote_arg.e = true;
			continue;
		}

		if(strcmp("-include", argv[i]) == 0)
		{
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			i++;
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			continue;
		}

		if(strcmp("-I", argv[i]) == 0)
		{
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			i++;
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			continue;
		}

		if(strncmp("-I", argv[i], 2) == 0)
		{
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			continue;
		}

		if(strncmp("-l", argv[i], 2) == 0)
		{
			remote_arg.link_lib_params[remote_arg.link_lib_count] = argv[i];
			remote_arg.link_lib_count++;
			continue;
		}

		if(strcmp("-S", argv[i]) == 0)
		{
			remote_arg.s = true;
			continue;
		}

		if(strcmp("-c", argv[i]) == 0)
		{
			remote_arg.c = true;
			continue;
		}

		if(strcmp("-o", argv[i]) == 0)
		{
			output_file = argv[++i];
			continue;
		}

		for (int j = 0; j < sizeof(gcc_spec_params) / sizeof(gcc_spec_params[0]); ++j)
		{
			if(strcmp(gcc_spec_params[j], argv[i]) == 0)
			{
				remote_arg.e_params[remote_arg.e_params_count] = argv[i];
				remote_arg.e_params_count++;
				remote_arg.c_params[remote_arg.c_params_count] = argv[i];
				remote_arg.c_params_count++;
				remote_arg.l_params[remote_arg.l_params_count] = argv[i];
				remote_arg.l_params_count++;
				i++;
				remote_arg.e_params[remote_arg.e_params_count] = argv[i];
				remote_arg.e_params_count++;
				remote_arg.c_params[remote_arg.c_params_count] = argv[i];
				remote_arg.c_params_count++;
				remote_arg.l_params[remote_arg.l_params_count] = argv[i];
				remote_arg.l_params_count++;
				goto cc;
			}
		}

		if('-' == argv[i][0])
		{
			/* 从管道输入，不知道从那一步开始 */
			remote_arg.e_params[remote_arg.e_params_count] = argv[i];
			remote_arg.e_params_count++;
			remote_arg.c_params[remote_arg.c_params_count] = argv[i];
			remote_arg.c_params_count++;
			remote_arg.l_params[remote_arg.l_params_count] = argv[i];
			remote_arg.l_params_count++;
			continue;
		}

		input_files[input_file_count] = argv[i];
		input_file_count++;
cc:
	}
	local_env = getenv("LOCAL");
	if(local_env)
		remote_arg.local = strcmp(local_env, "1") == 0;

	/* 从那一步开始编译 */
	if(input_file_count > 1)
	{
		for (int i = 0; i < input_file_count; ++i)
		{
			if(endWith(input_files[i], ".o") != 0)
			{
				goto single_input;
			}
		}

		ret = to_gcc_link(&remote_arg, input_files, output_file);
		goto out;
	}

single_input:
	if(!remote_arg.local)
	{
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_port = ntohs(host_port);
		serv_addr.sin_addr.s_addr = inet_addr(host_addr);

		socket_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (socket_fd < 0)
		{
			logerr("create socket error!\n");
			socket_fd = -1;
			ret = -1;
			goto out;
		} else {
			if((ret = connect(socket_fd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr))) < 0)
			{
				logerr("connect error!\n");
				goto out;
			}
		}
		remote_arg.socket_fp = fdopen(socket_fd, "rb+");
	}

	if(endWith(input_files[0], ".i") == 0)
	{
		ret = to_gcc_direct_compiler(&remote_arg, input_files[0], output_file);
		goto out;
	}

	ret = to_gcc_pretreatment(&remote_arg, input_files[0], output_file);

out:
	if(remote_arg.socket_fp)
		fclose(remote_arg.socket_fp);

	return ret;
}

int to_gcc(int argc, char **argv)
{
	int ret = 0;
	char *input_file = NULL;
	char *output_file = NULL;

	for (int i = 1; i < argc; ++i)
	{
		loginfo("%s ", argv[i]);
	}
	loginfo("\n");

	for (int i = 2; i < argc; ++i)
	{
		if(strcmp("-o", argv[i]) == 0)
		{
			output_file = argv[++i];
			continue;
		}

		for (int j = 0; j < sizeof(gcc_spec_params) / sizeof(gcc_spec_params[0]); ++j)
		{
			if(strcmp(gcc_spec_params[j], argv[i]) == 0)
			{
				i++;
				goto cc;
			}
		}

		if('-' == argv[i][0])
		{
			continue;
		}

		input_file = argv[i];
cc:
	}

	if(input_file)
		loginfo("---> input_file:%s\n", input_file);
	if(output_file)
		loginfo("---> output_file:%s\n", output_file);

	if(!input_file)
		return to_gcc_local(argc, argv);

	if(!output_file)
		return to_gcc_local(argc, argv);

	if(strcmp(output_file, "-") == 0)
		return to_gcc_local(argc, argv);

	if(strcmp(output_file, "/dev/null") == 0)
		return to_gcc_local(argc, argv);

	if(strcmp(input_file, "/dev/null") == 0)
		return to_gcc_local(argc, argv);

	if(endWith(input_file, ".s") == 0)
		return to_gcc_local(argc, argv);

	if(endWith(input_file, ".S") == 0)
		return to_gcc_local(argc, argv);

	if(to_gcc_remote(argc, argv) != 0)
	{
		logerr("remote exec fail, fallback to local.\n");
		return to_gcc_local(argc, argv);
	}

	return 0;
}

/* ---------------------------------------- */
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

	if(safe_fread_all(socket_fp, (char *)&file_name_len, 4) != 0)
	{
		logerr("safe_fread_all file_name_len fail\n");
		ret = -1;
		goto out;
	}
	if(file_name_len == 0)
	{
		logerr("server_send_file file_name_len is 0\n");
		ret = -1;
		goto out;
	}
	if(safe_fread_all(socket_fp, file_name, file_name_len) != 0)
	{
		logerr("safe_fread_all file_name fail\n");
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

	if(safe_fread_all(socket_fp, (char *)&file_name_len, 4) != 0)
	{
		logerr("delete file fail, safe_fread_all fail\n");
		ret = -1;
		goto out;
	}

	if(file_name_len == 0)
	{
		logerr("delete file fail, file name len is 0.\n");
		ret = -1;
		goto out;
	}

	if(safe_fread_all(socket_fp, file_name, file_name_len) != 0)
	{
		logerr("delete file fail, safe_fread_all fail\n");
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

		if(safe_fread_all(client_fp, &cmd, 1) != 0)
		{
			logerr("safe_fread_all fail, thread idx:%lu\n", client_idx);
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
				logprotocol("--- thread: %lu   delete   ---\n", client_idx);
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

	for (int i = 1; i < argc; ++i)
	{
		if(strcmp(argv[i], "-l"))
		{
			i++;
			listen_addr = argv[i];
			continue;
		}
		if(strcmp(argv[i], "-p"))
		{
			i++;
			listen_port = atoi(argv[i]);
			continue;
		}
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(listen_port);
	server_addr.sin_addr.s_addr = inet_addr(listen_addr);

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

int main(int argc, char **argv)
{
	int ret = 0;
	char *log_dir;
	static char log_path[128];

	if(argc < 2)
		return -1;

	ori_argc = argc;
	ori_argv = argv;

	if(getenv("COLCC_HOSTS"))
	{
		host_addr = getenv("COLCC_HOSTS");
	} else {
		host_addr = "127.0.0.1";
	}

	if(getenv("HOSTS_PORT"))
	{
		host_port = atoi(getenv("HOSTS_PORT"));
	} else {
		host_port = 3633;
	}

	if(getenv("DEBUG"))
	{
		debug = atoi(getenv("DEBUG"));
	} else {
		debug = 0;
	}

	if(getenv("DEBUG_PROTOCOL"))
	{
		debug_protocol = atoi(getenv("DEBUG_PROTOCOL"));
	} else {
		debug_protocol = 0;
	}

	if(getenv("DEBUG_FILE"))
	{
		debug_file = atoi(getenv("DEBUG_FILE"));
	} else {
		debug_file = 0;
	}

	log_dir = getenv("LOG_DIR");
	if(getenv("WORK_DIR"))
	{
		work_dir = getenv("WORK_DIR");
	} else {
		work_dir = "/tmp/colcc";
		mk_tmp_dir(work_dir);
	}

	if(log_dir)
	{
		sprintf(log_path, "%s%s", log_dir, "/colcc_server.log");
		log_fp = fopen(log_path, "a+");
		sprintf(log_path, "%s%s", log_dir, "/colcc_server_err.log");
		logerr_fp = fopen(log_path, "a+");
	} else {
		log_fp = stdout;
		logerr_fp = stderr;
	}

	loginfo("\n");
	if(strcmp("gcc", argv[1]) == 0)
	{
		ret = to_gcc(argc, argv);
		goto out;
	}

	if(strcmp("server", argv[1]) == 0)
	{
		ret = server(argc, argv);
		goto out;
	}

	if(strcmp("--help", argv[1]) == 0)
	{
		printf("usage:\n");
		printf("    colcc gcc     : use gcc compiler.\n");
		printf("    colcc client  : run as client.\n");
		printf("    colcc server  : run as server.\n");
		printf("                  [-l addr] [-p port]\n");
	}

out:
	if(ret != 0)
	{
		logerr("---> ori:\n");
		for (int i = 0; i < argc; ++i)
			logerr("%s ", argv[i]);
		logerr("\n");
	}
	return ret;
}