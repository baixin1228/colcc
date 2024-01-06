#pragma once

#include <sys/time.h>

extern int debug;
extern int debug_protocol;
extern int debug_file;
extern FILE *log_fp;
extern FILE *logerr_fp;
extern int ori_argc;
extern char **ori_argv;

#define timesub_ms(start, end) \
({\
	end.tv_sec * 1000 + end.tv_usec / 1000 - start.tv_sec * 1000 - start.tv_usec / 1000;\
})

#define logstd(args...) { \
	if(debug)\
	{\
		fprintf(stdout, ##args); \
		fflush(stdout); \
	}\
}

#define logprotocol(args...) { \
	if(debug_protocol)\
	{\
		fprintf(log_fp, ##args); \
		fflush(log_fp); \
	}\
}

#define logfile(args...) { \
	if(debug_file)\
	{\
		fprintf(log_fp, ##args); \
		fflush(log_fp); \
	}\
}

#define logdebug(args...) { \
	if(debug > 2)\
	{\
		fprintf(log_fp, ##args); \
		fflush(log_fp); \
	}\
}

#define loginfo(args...) { \
	if(debug > 1)\
	{\
		fprintf(log_fp, ##args); \
		fflush(log_fp); \
	}\
}

#define logwarning(args...) { \
	if(debug > 0)\
	{\
		fprintf(log_fp, ##args); \
		fflush(log_fp); \
	}\
}

#define logerr(args...) { \
	fprintf(logerr_fp, ##args); \
	fflush(logerr_fp); \
}

int endWith(const char *originString, char *end);
int get_random_str(char* random_str, const int random_len);
char *new_random_file_path(char *dest);
int mk_tmp_dir(char * path);
int delete_file(char * path);
int read_all(int fd, char *buffer, size_t len);
int write_all(int fd, char *buffer, size_t len);

int fread_all(FILE *fp, char *buffer, size_t len);
int fread_cache(FILE *fp, char *buffer, size_t len);
int safe_fread_all(FILE *fp, char *buffer, size_t len);
int fwrite_all(FILE *fp, char *buffer, size_t len);
int mini_exec(int argc, char **argv, bool need_fork, int out_fd);
