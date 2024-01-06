#pragma once

#define START_CODE "\x03\x03\x03\x03\x02\x02\x02\x02"
#define MSG_ADDR "/tmp/colcc/colcc_client"

enum {
	PUSH_FILE = 1,
	COMPILER,
	PULL_FILE,
	DELETE_FILE,
	CMD_OK,
	CMD_ERR
};

enum {
	GET_FD = 1,
	PUT_FD,
	HEART_BEAT,
};

int wait_start(FILE *socket_fp);
int recv_remote_file(FILE *socket_fp, char *file_name, char *rename, bool safe_read);
int send_remote_file(FILE *socket_fp, char *file_name, char *rename);
int delete_remote_file(FILE *socket_fp, char *file_name);
int send_params(FILE *socket_fp, char **params, uint32_t params_count);
int recv_params(FILE *socket_fp, 	char **params, uint32_t *params_count);

int msg_send_fd(int socket_fd, struct sockaddr_un *client_un, int fd, char *data, size_t len);
int msg_send_data(int socket_fd, struct sockaddr_un *client_un, char *data, size_t len);
