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
#include "server.h"
#include "protocol.h"

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

	char compression;

	FILE *socket_fp;
	bool local;
	/* 编译到那一步结束 */
	bool e;
	bool s;
	bool c;
};

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
			strcpy(new_random_file_path(c_output_file), ".o");
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
		strcpy(new_random_file_path(c_output_file), ".o");
		rel_output_file = c_output_file;

		strcpy(new_random_file_path(c_remote_file), ".o");

		remote_arg->c_params[remote_arg->c_params_count] = "-o";
		remote_arg->c_params_count++;
		remote_arg->c_params[remote_arg->c_params_count] = c_remote_file;
		remote_arg->c_params_count++;

		/* ----------------------- cmd start ---------------------- */
		if(fwrite_all(remote_arg->socket_fp, START_CODE, 8) != 0)
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

		if(fflush(remote_arg->socket_fp) != 0)
		{
			logerr("to_gcc_compiler fflush fail.\n");
			ret = -1;
			goto out;
		}
		
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
		if(fwrite_all(remote_arg->socket_fp, START_CODE, 8) != 0)
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
		if(fflush(remote_arg->socket_fp) != 0)
		{
			logerr("to_gcc_compiler fflush fail.\n");
			ret = -1;
			goto out;
		}

		if(remote_arg->c)
			ret = recv_remote_file(remote_arg->socket_fp, c_remote_file, output_file);
		else
			ret = recv_remote_file(remote_arg->socket_fp, c_remote_file, c_output_file);
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
		strcpy(new_random_file_path(e_remote_file), ".i");
		/* ----------------------- cmd start ---------------------- */
		if(fwrite_all(remote_arg->socket_fp, START_CODE, 8) != 0)
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

		if(send_remote_file(remote_arg->socket_fp, input_file, e_remote_file, remote_arg->compression) != 0)
		{
			logerr("send remote file fail.\n");
			return -1;
		}
		if(fflush(remote_arg->socket_fp) != 0)
		{
			logerr("to_gcc_compiler fflush fail.\n");
			ret = -1;
			goto out;
		}

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
		strcpy(new_random_file_path(e_output_file), ".i");

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
			strcpy(new_random_file_path(e_remote_file), ".i");
			compiler_input_file = e_remote_file;
			/* ----------------------- cmd start ---------------------- */
			if(fwrite_all(remote_arg->socket_fp, START_CODE, 8) != 0)
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
			
			ret = send_remote_file(remote_arg->socket_fp, e_output_file, e_remote_file, remote_arg->compression);
			if(ret != 0)
			{
				ret = -1;
				goto out;
			}

			if(fflush(remote_arg->socket_fp) != 0)
			{
				logerr("to_gcc_compiler fflush fail.\n");
				ret = -1;
				goto out;
			}

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

bool stop = false;
void *heart_thread(void *data)
{
	int socket_fd;
	uint32_t msg_data[2];
	int fd_handle = *(int *)data;
	struct sockaddr_un un_server;

	socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (socket_fd < 0)
	{
		logerr("create socket error!\n");
		return NULL;
	}

	un_server.sun_family = AF_UNIX;
	strcpy(un_server.sun_path, MSG_ADDR);

	while(!stop)
	{
		msg_data[0] = HEART_BEAT;
		msg_data[1] = fd_handle;
		if(msg_send_data(socket_fd, &un_server, (char *)&msg_data, sizeof(msg_data)) != 0)
		{
			break;
		}
		usleep(50000);
	}
	close(socket_fd);
	return NULL;
}

int to_gcc_remote(int argc, char **argv)
{
	int ret;
	int c_fd = -1;
	int socket_fd;
	char *local_env;
	uint32_t msg_data[2];
	uint32_t fd_handle;
	pthread_t thr = {0};
	char recv_name[32] = {0};
	int input_file_count = 0;
	struct msghdr recv_msg = {0};
	struct sockaddr_un un_client;
	struct sockaddr_un un_server;
	static char *input_files[128] = {NULL};
	char *output_file = NULL;

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
	for (int i = 0; i < input_file_count; ++i)
	{
		if(endWith(input_files[i], ".o") != 0)
		{
			goto single_input;
		}
	}

	ret = to_gcc_link(&remote_arg, input_files, output_file);
	goto out;

single_input:
	if(!remote_arg.local)
	{
		un_client.sun_family = AF_UNIX;
		strcpy(new_random_file_path(un_client.sun_path), ".i");
		unlink(un_client.sun_path);
		
		socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (socket_fd < 0)
		{
			logerr("create socket error!\n");
			socket_fd = -1;
			ret = -1;
			goto out;
		}

		if (bind(socket_fd, (struct sockaddr*)&un_client, sizeof(un_client)) < 0) {
			logerr("bind failed\n");
			ret = -1;
			goto out;
		}

		msg_data[0] = GET_FD;
		un_server.sun_family = AF_UNIX;
		strcpy(un_server.sun_path, MSG_ADDR);

		ret = msg_send_data(socket_fd, &un_server, (char *)&msg_data, sizeof(msg_data));
		if (ret < 0)
		{
			logerr("sendmsg error! %s %s\n", strerror(errno), MSG_ADDR);
			ret = -1;
			goto out;
		}

		struct iovec data_iov = {&msg_data, sizeof(msg_data)};
		recv_msg.msg_name = (void*)recv_name;
		recv_msg.msg_namelen = sizeof(recv_name);
		recv_msg.msg_iov = &data_iov;
		recv_msg.msg_iovlen = 1;
		char cmsg[CMSG_SPACE(sizeof(int))];
		recv_msg.msg_control = &cmsg;
		recv_msg.msg_controllen = sizeof(cmsg);

		ret = recvmsg(socket_fd, &recv_msg, 0);
		if (ret < 0)
		{
			logerr("recvmsg error!\n");
			ret = -1;
			goto out;
		}

		if(msg_data[0] != CMD_OK)
		{
			logerr("response err!\n");
			ret = -1;
			goto out;
		}
		
		fd_handle = msg_data[1] & 0xffff;
		remote_arg.compression = msg_data[1] >> 16;
		struct cmsghdr *ctrl = CMSG_FIRSTHDR(&recv_msg);
		c_fd = *(int*)CMSG_DATA(ctrl);

		remote_arg.socket_fp = fdopen(c_fd, "rb+");
		if(remote_arg.socket_fp == NULL)
		{
			logerr("fdopen fail!\n");
			ret = -1;
			goto out;
		}

		pthread_create(&thr, NULL, heart_thread, &fd_handle);
#if 0
		struct sockaddr_in serv_addr = {0};
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
		}

		if((ret = connect(socket_fd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr))) < 0)
		{
			logerr("connect:%s error!\n", host_addr);
			goto out;
		}

		remote_arg.socket_fp = fdopen(socket_fd, "rb+");
		socket_fd = -1;
#endif
	}

	if(endWith(input_files[0], ".i") == 0)
	{
		ret = to_gcc_direct_compiler(&remote_arg, input_files[0], output_file);
		goto out;
	}

	ret = to_gcc_pretreatment(&remote_arg, input_files[0], output_file);

out:
	if(socket_fd)
	{
		if(c_fd != -1)
		{
			msg_data[0] = PUT_FD;
			msg_data[1] = get_compress_record() << 16 | (fd_handle & 0xffff);
			ret = msg_send_fd(socket_fd, &un_server, c_fd, (char *)&msg_data, sizeof(msg_data));
			if (ret < 0)
			{
				logerr("sendmsg error! %s %s\n", strerror(errno), MSG_ADDR);
			}
			ret = 0;
		}
		close(socket_fd);
		unlink(un_client.sun_path);

		stop = true;
		if(thr != 0)
			pthread_join(thr, NULL);
	}

	if(remote_arg.socket_fp)
		fclose(remote_arg.socket_fp);

	return ret;
}

int to_gcc(int argc, char **argv)
{
	int ret = 0;
	char *input_file = NULL;
	char *output_file = NULL;

	signal(SIGPIPE, SIG_IGN);

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
		goto out2;
	}

	if(strcmp("client", argv[1]) == 0)
	{
		ret = client(argc, argv);
		goto out1;
	}

	if(strcmp("server", argv[1]) == 0)
	{
		ret = server(argc, argv);
		goto out1;
	}

	if(strcmp("--help", argv[1]) == 0)
	{
		printf("usage:\n");
		printf("    colcc gcc     : use gcc compiler.\n");
		printf("    colcc client  : run as client.\n");
		printf("    colcc server  : run as server.\n");
		printf("                  [-l addr] [-p port]\n");
	}

out2:
	if(ret != 0)
	{
		logerr("---> ori:\n");
		for (int i = 0; i < argc; ++i)
			logerr("%s ", argv[i]);
		logerr("\n");
	}
out1:
	return ret;
}