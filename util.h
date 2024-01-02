#pragma once

extern int debug;
extern int debug_protocol;
extern int debug_file;
extern FILE *log_fp;
extern FILE *logerr_fp;
extern int ori_argc;
extern char **ori_argv;

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
int mk_tmp_dir(char * path);
int delete_file(char * path);
int read_all(int fd, char *buffer, size_t len);
int write_all(int fd, char *buffer, size_t len);

int fread_all(FILE *fp, char *buffer, size_t len);
int safe_fread_all(FILE *fp, char *buffer, size_t len);
int fwrite_all(FILE *fp, char *buffer, size_t len);
int mini_exec(int argc, char **argv, bool need_fork, int out_fd);