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

#include "util.h"

int endWith(const char *originString, char *end)
{
	// 参数校验
	if (originString == NULL || end == NULL || strlen(end) > strlen(originString)) {
		return -1;
	}
	
	int n = strlen(end);
	int m = strlen(originString);
	int i;
	for (i = 0; i < n; i++) {
		if (originString[m-i-1] != end[n-i-1]) {
			return 1;
		}
	}
	return 0;
}

int get_random_str(char* random_str, const int random_len)
{
	int i;
	uuid_t uu;

	uuid_generate_random(uu);

	for(i = 0; i < random_len &&i < 16; i++)
	{
		sprintf(&random_str[i], "%x", uu[i]);
	}

	return 0;
}

char *new_random_file_path(char *dest)
{
	strcpy(dest, "/tmp/colcc/");
	get_random_str(dest + 11, 16);
	return dest + 27;
}

int mk_tmp_dir(char * path)
{
	if((access(path, 0)))
		return mkdir(path, S_IRWXU);

	return 0;
}

int delete_file(char * path)
{
	if((access(path, F_OK)) == 0)
		return remove(path);
	else
		logwarning("delete, not find file:%s\n", path);

	return 0;
}

int read_all(int fd, char *buffer, size_t len)
{
	int read_sum = 0;
	int read_con;
	while(len)
	{
		read_con = read(fd, buffer + read_sum, len);
		if(read_con > 0)
		{
			read_sum += read_con;
			len -= read_con;
		} else
			return -1;
	}
	return 0;
}

int write_all(int fd, char *buffer, size_t len)
{
	int write_sum = 0;
	int write_con;
	while(len)
	{
		write_con = write(fd, buffer + write_sum, len);
		if(write_con > 0)
		{
			write_sum += write_con;
			len -= write_con;
		} else
			return -1;
	}
	return 0;
}

struct fread_cache {
	char *cache;
	int head;
	int tail;
};

struct fread_cache caches[1024] = {0};
int fread_cache(FILE *fp, char *buffer, size_t len)
{
	int ret;
	int fd = fileno(fp);
	if(!caches[fd].cache)
	{
		caches[fd].cache = malloc(10240);
	}

	if(caches[fd].head == caches[fd].tail)
	{
		ret = fread(caches[fd].cache, 1, 10240, fp);
		if(ret <= 0)
			return ret;

		caches[fd].tail = ret;
		caches[fd].head = 0;
	}

	ret = len < caches[fd].tail - caches[fd].head ? len : caches[fd].tail - caches[fd].head;
	memcpy(buffer, caches[fd].cache + caches[fd].head, ret);
	caches[fd].head += ret;

	return ret;
}

int fread_all(FILE *fp, char *buffer, size_t len)
{
	int read_sum = 0;
	int read_con;
	while(len)
	{
		read_con = fread(buffer + read_sum, 1, len, fp);
		if(read_con > 0)
		{
			read_sum += read_con;
			len -= read_con;
		} else
			return -1;
	}
	return 0;
}

int fwrite_all(FILE *fp, char *buffer, size_t len)
{
	int write_sum = 0;
	int write_con;
	while(len)
	{
		write_con = fwrite(buffer + write_sum, 1, len, fp);
		if(write_con > 0)
		{
			write_sum += write_con;
			len -= write_con;
		} else
			return -1;
	}
	return 0;
}

int ori_argc;
char **ori_argv;
int mini_exec(int argc, char **argv, bool need_fork, int out_fd)
{
	int ret = 0;
	char *out_buffer;
	bool core_dump = false;
	bool exit_signel = false;
	bool exit_code = false;
	pid_t pid;
	int piple_fd[2];

	argv[argc] = NULL;

	out_buffer = malloc(4096);
	if(out_buffer == NULL)
	{
		logerr("mini_exec malloc fail\n");
		return -1;
	}

	logdebug("---> ");
	for (int i = 0; i < argc; ++i)
		logdebug("%s ", argv[i]);
	logdebug("\n");

	if(need_fork)
	{
		if(out_fd != -1)
		{
			if (pipe(piple_fd) == -1) 
				fprintf(stderr, "piple create fail!\n");
		}

		pid = fork();
		if (pid < 0)
		{
			fprintf(stderr, "error in fork!\n");
			ret = -1;
			goto out;
		}
		if (pid == 0)
		{
			if(out_fd != -1)
			{
				close(piple_fd[0]);
				close(1);
				dup2(piple_fd[1], 1);
				close(piple_fd[1]);
			}
			exit(execv("/usr/bin/gcc", argv));
		} else {
			if(out_fd != -1)
			{
				close(piple_fd[1]);
				while((ret = read(piple_fd[0], out_buffer, 4096)) != 0)
					if(write_all(out_fd, out_buffer, ret) != 0)
						logerr("mini_exec write_all fail.\n");

				
				char stop = 4;
				if(write_all(out_fd, &stop, 1) != 0)
					logerr("mini_exec write_all fail.\n");
				close(piple_fd[0]);
			}
			waitpid(pid, &ret, 0);
			exit_code = ret >> 8;
			exit_signel = (ret >> 1) & 0x7f;
			core_dump = ret & 0x1;
		}
	} else {
		exit(execv("/usr/bin/gcc", argv));
	}

	if(ret != 0)
	{
		loginfo("---> colcc:  ret:%d exit_code:%d exit_signel:%d core_dump:%d\n",
			ret, exit_code, exit_signel, core_dump);
		for (int i = 0; i < argc; ++i)
			loginfo("%s ", argv[i]);
		loginfo("\n");
		loginfo("---> diff:\n");
		for (int i = 1; i < ori_argc; ++i)
		{
			bool find = false;
			for (int j = 0; j < argc; ++j)
			{
				if(ori_argv[i] && argv[j] && strcmp(ori_argv[i], argv[j]) == 0)
				{
					find = true;
					break;
				}
			}
			if(!find)
				loginfo("%s ", ori_argv[i]);
		}
		loginfo("\n");
	}

out:
	if(out_buffer)
		free(out_buffer);

	return ret;
}