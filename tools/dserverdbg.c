#define _GNU_SOURCE
#include <darlingserver/rpc.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>

typedef uint32_t mach_port_type_t;
typedef uint32_t mach_port_right_t;

#define MACH_PORT_RIGHT_SEND            ((mach_port_right_t) 0)
#define MACH_PORT_RIGHT_RECEIVE         ((mach_port_right_t) 1)
#define MACH_PORT_RIGHT_SEND_ONCE       ((mach_port_right_t) 2)
#define MACH_PORT_RIGHT_PORT_SET        ((mach_port_right_t) 3)
#define MACH_PORT_RIGHT_DEAD_NAME       ((mach_port_right_t) 4)
#define MACH_PORT_RIGHT_LABELH          ((mach_port_right_t) 5) /* obsolete right */
#define MACH_PORT_RIGHT_NUMBER          ((mach_port_right_t) 6) /* right not implemented */

#define MACH_PORT_TYPE(right)                                           \
	        ((mach_port_type_t)(((mach_port_type_t) 1)              \
	        << ((right) + ((mach_port_right_t) 16))))
#define MACH_PORT_TYPE_NONE         ((mach_port_type_t) 0L)
#define MACH_PORT_TYPE_SEND         MACH_PORT_TYPE(MACH_PORT_RIGHT_SEND)
#define MACH_PORT_TYPE_RECEIVE      MACH_PORT_TYPE(MACH_PORT_RIGHT_RECEIVE)
#define MACH_PORT_TYPE_SEND_ONCE    MACH_PORT_TYPE(MACH_PORT_RIGHT_SEND_ONCE)
#define MACH_PORT_TYPE_PORT_SET     MACH_PORT_TYPE(MACH_PORT_RIGHT_PORT_SET)
#define MACH_PORT_TYPE_DEAD_NAME    MACH_PORT_TYPE(MACH_PORT_RIGHT_DEAD_NAME)
#define MACH_PORT_TYPE_LABELH       MACH_PORT_TYPE(MACH_PORT_RIGHT_LABELH) /* obsolete */

// borrowed from `src/startup/darling.c`
// ---
// Between Linux 4.9 and 4.11, a strange bug has been introduced
// which prevents connecting to Unix sockets if the socket was
// created in a different mount namespace or under overlayfs
// (dunno which one is really responsible for this).
#define USE_LINUX_4_11_HACK 1

typedef enum dserverdbg_command {
	dserverdbg_command_ps,
	dserverdbg_command_lsport,
	dserverdbg_command_lspset,
	dserverdbg_command_lsmsg,
	dserverdbg_command_uidgid_fault,
	dserverdbg_command_fork_wait_sticky,
} dserverdbg_command_t;

struct sockaddr_un __dserver_socket_address_data = {0};
int __dserver_main_thread_socket_fd = -1;

static char* default_prefix_path(uid_t original_uid) {
	struct passwd* info = getpwuid(original_uid);
	char* result = NULL;

	if (asprintf(&result, "%s/.darling", info->pw_dir) < 0) {
		return NULL;
	}

	return result;
};

static char* get_prefix_path(uid_t original_uid) {
	char* env = getenv("DPREFIX");

	if (env) {
		return strdup(env);
	}

	return default_prefix_path(original_uid);
};

// borrowed from `src/startup/darling.c`
static void joinNamespace(pid_t pid, int type, const char* typeName)
{
	int fdNS;
	char pathNS[4096];

	snprintf(pathNS, sizeof(pathNS), "/proc/%d/ns/%s", pid, typeName);

	fdNS = open(pathNS, O_RDONLY);

	if (fdNS < 0)
	{
		fprintf(stderr, "Cannot open %s namespace file: %s\n", typeName, strerror(errno));
		exit(1);
	}

	// Calling setns() with a PID namespace doesn't move our process into it,
	// but our child process will be spawned inside the namespace
	if (setns(fdNS, type) != 0)
	{
		fprintf(stderr, "Cannot join %s namespace: %s\n", typeName, strerror(errno));
		exit(1);
	}
	close(fdNS);
}

// borrowed from `src/startup/darling.c`, with the UID/GID check removed
static pid_t getInitProcess(const char* prefix)
{
	const char pidFile[] = "/.init.pid";
	char* pidPath;
	pid_t pid;
	int pid_i;
	FILE *fp;
	char procBuf[100];
	char *exeBuf;

	pidPath = (char*) alloca(strlen(prefix) + sizeof(pidFile));
	strcpy(pidPath, prefix);
	strcat(pidPath, pidFile);

	fp = fopen(pidPath, "r");
	if (fp == NULL)
		return 0;

	if (fscanf(fp, "%d", &pid_i) != 1)
	{
		fclose(fp);
		unlink(pidPath);
		return 0;
	}
	fclose(fp);
	pid = (pid_t) pid_i;

	// Does the process exist?
	if (kill(pid, 0) == -1)
	{
		unlink(pidPath);
		return 0;
	}

	// Is it actually an init process?
	snprintf(procBuf, sizeof(procBuf), "/proc/%d/comm", pid);
	fp = fopen(procBuf, "r");
	if (fp == NULL)
	{
		unlink(pidPath);
		return 0;
	}

	if (fscanf(fp, "%ms", &exeBuf) != 1)
	{
		fclose(fp);
		unlink(pidPath);
		return 0;
	}
	fclose(fp);

	if (strcmp(exeBuf, "darlingserver") != 0)
	{
		unlink(pidPath);
		return 0;
	}
	free(exeBuf);

	return pid;
}

static int setup_socket(void) {
	int fd = -1;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		goto err_out;
	}

	int fd_flags = fcntl(fd, F_GETFD);
	if (fd_flags < 0) {
		goto err_out;
	}
	if (fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
		goto err_out;
	}

	sa_family_t family = AF_UNIX;
	if (bind(fd, (const struct sockaddr*)&family, sizeof(family)) < 0) {
		goto err_out;
	}

out:
	return fd;

err_out:
	if (fd >= 0) {
		close(fd);
	}

	return -1;
};

static int set_receive_timeout(int fd, int seconds) {
	struct timeval timeout = {
		.tv_sec = seconds,
		.tv_usec = 0,
	};

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		return errno;
	}

	return 0;
}

static int run_uidgid_fault_oracle(void) {
	int old_uid = -1;
	int old_gid = -1;
	int status = 0;

	status = set_receive_timeout(__dserver_main_thread_socket_fd, 3);
	if (status != 0) {
		fprintf(stderr, "DSERVERDBG_RECV_TIMEOUT_SETUP_RC=%d (%s)\n", status, strerror(status));
		return 1;
	}

	status = dserver_rpc_checkin(false, NULL, -1);
	printf("DSERVERDBG_CHECKIN_RC=%d\n", status);
	if (status != 0) {
		return 1;
	}

	status = dserver_rpc_uidgid(-1, -1, &old_uid, &old_gid);
	printf("DSERVERDBG_UIDGID_RC=%d\n", status);
	printf("DSERVERDBG_UIDGID_OLD_UID=%d\n", old_uid);
	printf("DSERVERDBG_UIDGID_OLD_GID=%d\n", old_gid);

	if (status == -EINVAL) {
		printf("DSERVERDBG_UIDGID_FAULT_OK\n");
		return 0;
	}

	return 1;
}

static int write_fault_line(const char* line) {
	const char* path = getenv("DSERVER_TEST_FAULT_FILE");
	if (!path || path[0] == '\0') {
		fprintf(stderr, "DSERVERDBG_FAULT_FILE=missing\n");
		return 1;
	}

	FILE* file = fopen(path, "w");
	if (!file) {
		int status = errno;
		fprintf(stderr, "DSERVERDBG_FAULT_WRITE_RC=%d (%s)\n", status, strerror(status));
		return status;
	}

	if (fputs(line, file) < 0 || fputc('\n', file) < 0) {
		int status = errno;
		fclose(file);
		fprintf(stderr, "DSERVERDBG_FAULT_WRITE_RC=%d (%s)\n", status, strerror(status));
		return status;
	}
	if (fclose(file) != 0) {
		int status = errno;
		fprintf(stderr, "DSERVERDBG_FAULT_CLOSE_RC=%d (%s)\n", status, strerror(status));
		return status;
	}

	printf("DSERVERDBG_FAULT_WRITTEN=%s\n", line);
	return 0;
}

static int run_fork_wait_sticky_oracle(void) {
	int status = 0;
	pid_t parent_pid = getpid();

	status = set_receive_timeout(__dserver_main_thread_socket_fd, 3);
	if (status != 0) {
		fprintf(stderr, "DSERVERDBG_RECV_TIMEOUT_SETUP_RC=%d (%s)\n", status, strerror(status));
		return 1;
	}

	status = dserver_rpc_checkin(false, NULL, -1);
	printf("DSERVERDBG_PARENT_CHECKIN_RC=%d\n", status);
	printf("DSERVERDBG_PARENT_PID=%d\n", parent_pid);
	if (status != 0) {
		return 1;
	}

	char fault[128];
	snprintf(fault, sizeof(fault), "fork.interrupt_wait parent=%d", parent_pid);
	status = write_fault_line(fault);
	if (status != 0) {
		return 1;
	}

	status = dserver_rpc_fork_wait_for_child();
	printf("DSERVERDBG_FORK_FIRST_WAIT_RC=%d\n", status);
	if (status != -ETIMEDOUT) {
		return 1;
	}

	snprintf(fault, sizeof(fault), "fork.skip_checkin_semaphore parent=%d", parent_pid);
	status = write_fault_line(fault);
	if (status != 0) {
		return 1;
	}
	fflush(stdout);

	pid_t child = fork();
	if (child < 0) {
		status = errno;
		fprintf(stderr, "DSERVERDBG_FORK_RC=%d (%s)\n", status, strerror(status));
		return 1;
	}
	if (child == 0) {
		int child_status = dserver_rpc_checkin(true, NULL, -1);
		printf("DSERVERDBG_CHILD_CHECKIN_RC=%d\n", child_status);
		fflush(stdout);
		_exit(child_status == 0 ? 0 : 1);
	}

	int child_status = 0;
	if (waitpid(child, &child_status, 0) != child) {
		status = errno;
		fprintf(stderr, "DSERVERDBG_WAITPID_RC=%d (%s)\n", status, strerror(status));
		return 1;
	}
	printf("DSERVERDBG_CHILD_EXIT_STATUS=%d\n", child_status);
	if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
		return 1;
	}

	status = dserver_rpc_fork_wait_for_child();
	printf("DSERVERDBG_FORK_SECOND_WAIT_RC=%d\n", status);
	if (status == 0) {
		printf("DSERVERDBG_FORK_WAIT_STICKY_OK\n");
		return 0;
	}

	return 1;
}

// borrowed from `src/startup/darling.c`
static void missingSetuidRoot(void)
{
	char path[4096];
	int len;

	len = readlink("/proc/self/exe", path, sizeof(path)-1);
	if (len < 0)
		strcpy(path, "darling");
	else
		path[len] = '\0';

	fprintf(stderr, "Sorry, the `%s' binary is not setuid root, which is mandatory.\n", path);
	fprintf(stderr, "Darling needs this in order to create mount and PID namespaces and to perform mounts.\n");
}

static int run_direct_uidgid_fault_command(uid_t original_uid) {
	char* prefix_path = get_prefix_path(original_uid);
	int status = 1;

	if (!prefix_path) {
		fprintf(stderr, "Failed to determine prefix path\n");
		return 1;
	}

	__dserver_socket_address_data.sun_family = AF_UNIX;
	snprintf(__dserver_socket_address_data.sun_path, sizeof(__dserver_socket_address_data.sun_path), "%s/.darlingserver.sock", prefix_path);

	__dserver_main_thread_socket_fd = setup_socket();
	if (__dserver_main_thread_socket_fd < 0) {
		fprintf(stderr, "Failed to set up darlingserver client socket\n");
		goto out;
	}

	status = run_uidgid_fault_oracle();

out:
	if (__dserver_main_thread_socket_fd >= 0) {
		close(__dserver_main_thread_socket_fd);
		__dserver_main_thread_socket_fd = -1;
	}
	free(prefix_path);
	return status;
}

static int run_direct_fork_wait_sticky_command(uid_t original_uid) {
	char* prefix_path = get_prefix_path(original_uid);
	int status = 1;

	if (!prefix_path) {
		fprintf(stderr, "Failed to determine prefix path\n");
		return 1;
	}

	__dserver_socket_address_data.sun_family = AF_UNIX;
	snprintf(__dserver_socket_address_data.sun_path, sizeof(__dserver_socket_address_data.sun_path), "%s/.darlingserver.sock", prefix_path);

	__dserver_main_thread_socket_fd = setup_socket();
	if (__dserver_main_thread_socket_fd < 0) {
		fprintf(stderr, "Failed to set up darlingserver client socket\n");
		goto out;
	}

	status = run_fork_wait_sticky_oracle();

out:
	if (__dserver_main_thread_socket_fd >= 0) {
		close(__dserver_main_thread_socket_fd);
		__dserver_main_thread_socket_fd = -1;
	}
	free(prefix_path);
	return status;
}

int main(int argc, char** argv) {
	char* prefix_path = NULL;
	dserverdbg_command_t command = dserverdbg_command_ps;
	pid_t command_pid = 0;
	uint32_t command_port = 0;
	int output_fd = -1;
	int status = 0;
	uint64_t count = 0;
	uint64_t elmsize = 0;
	char* data = 0;
	uid_t original_uid = -1;
	gid_t original_gid = -1;
#if USE_LINUX_4_11_HACK
	pid_t pidInit = 0;
#endif

	if (argc > 1 && strcmp(argv[1], "uidgid-fault") == 0) {
		if (argc > 2) {
			fprintf(stderr, "Expected 1 argument (subcommand); got %d arguments\n", argc);
			return 1;
		}
		return run_direct_uidgid_fault_command(getuid());
	}
	if (argc > 1 && strcmp(argv[1], "fork-wait-sticky") == 0) {
		if (argc > 2) {
			fprintf(stderr, "Expected 1 argument (subcommand); got %d arguments\n", argc);
			return 1;
		}
		return run_direct_fork_wait_sticky_command(getuid());
	}

	if (geteuid() != 0) {
		missingSetuidRoot();
		return 1;
	}

	original_uid = getuid();
	original_gid = getgid();

	setuid(0);
	setgid(0);

	prefix_path = get_prefix_path(original_uid);
	if (!prefix_path) {
		fprintf(stderr, "Failed to determine prefix path\n");
		return 1;
	}

	__dserver_socket_address_data.sun_family = AF_UNIX;
	snprintf(__dserver_socket_address_data.sun_path, sizeof(__dserver_socket_address_data.sun_path), "%s/.darlingserver.sock", prefix_path);

#if USE_LINUX_4_11_HACK
	pidInit = getInitProcess(prefix_path);
	joinNamespace(pidInit, CLONE_NEWNS, "mnt");
#endif

	__dserver_main_thread_socket_fd = setup_socket();

	if (__dserver_main_thread_socket_fd < 0) {
		fprintf(stderr, "Failed to set up darlingserver client socket\n");
		return 1;
	}

	if (argc > 1) {
		if (strcmp(argv[1], "ps") == 0 || strcmp(argv[1], "lsproc") == 0) {
			command = dserverdbg_command_ps;
		} else if (strcmp(argv[1], "lsport") == 0) {
			command = dserverdbg_command_lsport;
		} else if (strcmp(argv[1], "lspset") == 0) {
			command = dserverdbg_command_lspset;
		} else if (strcmp(argv[1], "lsmsg") == 0) {
			command = dserverdbg_command_lsmsg;
		} else if (strcmp(argv[1], "uidgid-fault") == 0) {
			command = dserverdbg_command_uidgid_fault;
		} else if (strcmp(argv[1], "fork-wait-sticky") == 0) {
			command = dserverdbg_command_fork_wait_sticky;
		} else {
			fprintf(stderr, "Unknown subcommand: %s\n", argv[1]);
			return 1;
		}
	}

	switch (command) {
		case dserverdbg_command_ps:
			if (argc > 2) {
				fprintf(stderr, "Expected 1 argument (subcommand); got %d arguments\n", argc);
				return 1;
			}
			break;
		case dserverdbg_command_lsport:
			if (argc != 3) {
				fprintf(stderr, "Expected 2 arguments (subcommand and PID); got %d arguments\n", argc);
				return 1;
			}
			command_pid = atoi(argv[2]);
			break;
		case dserverdbg_command_uidgid_fault:
		case dserverdbg_command_fork_wait_sticky:
			if (argc > 2) {
				fprintf(stderr, "Expected 1 argument (subcommand); got %d arguments\n", argc);
				return 1;
			}
			break;
		case dserverdbg_command_lspset:
		case dserverdbg_command_lsmsg:
			if (argc != 4) {
				fprintf(stderr, "Expected 3 arguments (subcommand, PID, and port name); got %d arguments\n", argc);
				return 1;
			}
			command_pid = atoi(argv[2]);
			command_port = atoi(argv[3]);
			break;
	}

	switch (command) {
		case dserverdbg_command_ps:
			status = dserver_rpc_debug_list_processes(&count, &output_fd);
			elmsize = sizeof(dserver_debug_process_t);
			break;
		case dserverdbg_command_lsport:
			status = dserver_rpc_debug_list_ports(command_pid, &count, &output_fd);
			elmsize = sizeof(dserver_debug_port_t);
			break;
		case dserverdbg_command_lspset:
			status = dserver_rpc_debug_list_members(command_pid, command_port, &count, &output_fd);
			elmsize = sizeof(dserver_debug_port_t);
			break;
		case dserverdbg_command_lsmsg:
			status = dserver_rpc_debug_list_messages(command_pid, command_port, &count, &output_fd);
			elmsize = sizeof(dserver_debug_message_t);
			break;
		case dserverdbg_command_uidgid_fault:
			status = run_uidgid_fault_oracle();
			goto out;
		case dserverdbg_command_fork_wait_sticky:
			status = run_fork_wait_sticky_oracle();
			goto out;
	}

	if (status != 0) {
		fprintf(stderr, "Subcommand failed: server replied with error: %d (%s)\n", status, strerror(status));
		return 1;
	}

	for (uint64_t i = 0; i < count; ++i) {
		char buffer[elmsize];

		if (read(output_fd, buffer, elmsize) != elmsize) {
			status = errno;
			fprintf(stderr, "Failed to read from output pipe: %d (%s)\n", status, strerror(status));
			return 1;
		}

		switch (command) {
			case dserverdbg_command_ps: {
				dserver_debug_process_t* data = (void*)buffer;
				printf("pid %u - %lu ports\n", data->pid, data->port_count);
			} break;

			case dserverdbg_command_lsport:
			case dserverdbg_command_lspset: {
				dserver_debug_port_t* data = (void*)buffer;
				const char* right_name = "<unknown>";

				if (data->rights == MACH_PORT_TYPE_SEND) {
					right_name = "send";
				} else if (data->rights == MACH_PORT_TYPE_RECEIVE) {
					right_name = "receive";
				} else if (data->rights == MACH_PORT_TYPE_SEND_ONCE) {
					right_name = "send-once";
				} else if (data->rights == MACH_PORT_TYPE_PORT_SET) {
					right_name = "port set";
				} else if (data->rights == MACH_PORT_TYPE_DEAD_NAME) {
					right_name = "dead name";
				} else if (data->rights == MACH_PORT_TYPE_LABELH) {
					right_name = "labelh";
				}

				printf("port %d (%s), %lu refs - %lu messages\n", data->port_name, right_name, data->refs, data->messages);
			} break;

			case dserverdbg_command_lsmsg: {
				dserver_debug_message_t* data = (void*)buffer;

				printf("message %lu (from %u); %lu bytes\n", i, data->sender, data->size);
			} break;
		}
	}

	if (output_fd >= 0) {
		close(output_fd);
	}

	if (prefix_path) {
		free(prefix_path);
	}

	return 0;

out:
	if (prefix_path) {
		free(prefix_path);
	}

	return status;
};
