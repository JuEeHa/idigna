#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <poll.h>
#include <netinet/in.h>
#include <unistd.h>

const char *program_name;
const char *remote;
long int remote_port = 70;
char remote_port_string[6];

struct pollfd *sockets = NULL;
size_t number_sockets = 0;
size_t number_interfaces = 0;

enum connection_state { START, PATH, REQUEST_END, CONNECT, REQUEST_WRITE, READ, WRITE };
struct connection {
	enum connection_state state;
	int sock;
	char *path;
	size_t path_size;
	char *buffer;
	size_t buffer_size;
	int sock_other;
	size_t written;
	size_t read;
};

struct connection **connections = NULL;
size_t number_connections = 0;

void usage(FILE *stream) {
	fprintf(stream, "%s [--port|-p server_port] remote [remote_port]\n", program_name);
}

void help(FILE *stream) {
	usage(stream);
}

long int parse_port(const char *string) {
	char *endptr;
	long int port = strtol(string, &endptr, 10);

	if(endptr == string || *endptr != '\0') { // String did not fully scan as number
		return -1;
	} else if(port < 0 || port > 1<<16) { // Port value out of range
		return -1;
	} else { // All ok
		return port;
	}
}

bool stringify_port(long int port, char *buffer, size_t buffer_length) {
	int size = snprintf(buffer, buffer_length, "%li", port);

	// If snprintf returns either an error signal or size larger than buffer, signal error
	if(size < 0 || (size_t)size > buffer_length) {
		return false;
	} else {
		return true;
	}
}

void add_socket(int sock, short events) {
	// Grow the table of sockets
	size_t index = number_sockets++;
	sockets = realloc(sockets, number_sockets * sizeof(struct pollfd));

	if(sockets == NULL) {
		perror("realloc");
		exit(1);
	}

	// Add socket to the table
	struct pollfd new_socket = {.fd = sock, .events = events};
	sockets[index] = new_socket;
}

void remove_socket(size_t index) {
	// Clean the socket up
	close(sockets[index].fd);

	if(index != number_sockets - 1) {
		// The socket entry was not at the end of the table -> we need to rearrange to allow shrinking of allocation
		memmove(&sockets[index], &sockets[number_sockets - 1], sizeof(*sockets));
	}

	// Now the last entry in the table is either the socket to remove or a duplicate -> safe to remove either way
	// Shrink the allocation and update number_sockets
	sockets = realloc(sockets, --number_sockets * sizeof(struct pollfd));

	if(sockets == NULL && number_sockets != 0) {
		perror("realloc");
		exit(1);
	}
}

size_t get_socket_index(int sock) {
	for(size_t i = 0; i < number_sockets; i++) {
		if(sockets[i].fd == sock) {
			return i;
		}
	}

	// None found, return index of last element + 1
	return number_sockets;
}

void add_connection(int sock) {
	// Grow the table of connections
	size_t index = number_connections++;
	connections = realloc(connections, number_connections * sizeof(struct connection));

	if(connections == NULL) {
		perror("realloc");
		exit(1);
	}

	// Add socket to table of sockets
	add_socket(sock, POLLIN);

	// Initialise and add connection to the table
	struct connection *connection = calloc(1, sizeof(struct connection));

	if(connection == NULL && sizeof(struct connection) != 0) {
		perror("calloc");
		exit(1);
	}

	connection->state = START;
	connection->sock = sock;
	connection->sock_other = -1;

	connections[index] = connection;
}

void remove_connection(size_t index) {
	// Clean the connection up
	size_t socket_index = get_socket_index(connections[index]->sock);
	if(socket_index == number_sockets) {
		fprintf(stderr, "%s: socket to remove not in table of sockets\n", program_name);
		exit(1);
	}
	remove_socket(socket_index);

	if(connections[index]->sock_other != -1) {
		close(connections[index]->sock_other);
	}

	if(connections[index]->path != NULL) {
		free(connections[index]->path);
	}

	if(connections[index]->buffer != NULL) {
		free(connections[index]->buffer);
	}

	if(index != number_connections - 1) {
		// The connection was not at the end of the table -> we need to rearrange to allow shrinking of allocation
		memmove(&connections[index], &connections[number_connections - 1], sizeof(*connections));
	}

	// Now the last entry in the table is either the connection to remove or a duplicate -> safe to remove either way
	// Shrink the allocation and update number_connections
	connections = realloc(connections, --number_connections * sizeof(struct connection));

	if(connections == NULL && number_connections != 0) {
		perror("realloc");
		exit(1);
	}
}

size_t get_connection_index(int sock) {
	for(size_t i = 0; i < number_connections; i++) {
		if(connections[i]->sock == sock) {
			return i;
		}
	}

	// None found, return index of last element + 1
	return number_connections;
}

void add_listen(struct addrinfo *res) {
	const int yes = 1;

	// Create socket
	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(sock == -1) {
		perror("socket");
		exit(1);
	}

	// Disable the IPv4 over IPv6, as that results in IPv4 and IPv6 sockets conflicting and one of them not being able to be set up
	if(res->ai_family == AF_INET6) {
		if(setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
			perror("setsockopt");
			exit(1);
		}
	}

	// Set reuseaddr
	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
		perror("setsockopt");
		exit(1);
	}

	// Bind onto given address
	if(bind(sock, res->ai_addr, res->ai_addrlen) == -1) {
		perror("bind");
		exit(1);
	}

	// Listen for incoming connections
	if(listen(sock, 1) == -1) {
		perror("listen");
		exit(1);
	}

	add_socket(sock, POLLIN);
}

void setup_listen(unsigned long port) {
	char port_string[6];

	// getaddrinfo wants port as a string, so stringify it
	if(!stringify_port(port, port_string, sizeof(port_string))) {
		fprintf(stderr, "%s: Could not convert %li to string\n", program_name, port);
		exit(1);
	}

	struct addrinfo hints;
	struct addrinfo *getaddrinfo_result;

	// AF_UNSPEC: either IPv4 or IPv6
	// SOCK_STREAM: TCP
	// AI_PASSIVE: fill out my IP for me
	// AI_ADDRCONFIG: only return addresses I have a configured interface for
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

	int status = getaddrinfo(NULL, port_string, &hints, &getaddrinfo_result);

	if(status != 0) {
		fprintf(stderr, "%s: getaddrinfo failed: %s\n", program_name, gai_strerror(status));
		exit(1);
	}

	for(struct addrinfo *res = getaddrinfo_result; res != NULL; res = res->ai_next) {
		// Add corresponding interface to table of sockets
		add_listen(res);
	}

	freeaddrinfo(getaddrinfo_result);

	// Store the number of sockets corresponding to interfaces, as they won't be the only sockets in the table
	number_interfaces = number_sockets;
}

int connect_to_remote(void) {
	struct addrinfo hints;
	struct addrinfo *getaddrinfo_result;

	// AF_UNSPEC: either IPv4 or IPv6
	// SOCK_STREAM: TCP
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	int status = getaddrinfo(remote, remote_port_string, &hints, &getaddrinfo_result);

	if(status != 0) {
		fprintf(stderr, "%s: getaddrinfo failed: %s\n", program_name, gai_strerror(status));
		exit(1);
	}

	for(struct addrinfo *res = getaddrinfo_result; res != NULL; res = res->ai_next) {
		// Create socket
		int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if(sock == -1) {
			perror("socket");
			exit(1);
		}

		// Connect to remote
		if(connect(sock, res->ai_addr, res->ai_addrlen) == -1) {
			close(sock);
			continue;
		}

		return sock;
	}

	return -1;
}

void buffer_append(char **buffer, size_t *buffer_length, char *appended, size_t appended_length) {
	*buffer = realloc(*buffer, *buffer_length + appended_length);

	if (*buffer == NULL && *buffer_length + appended_length != 0) {
		perror("realloc");
		exit(1);
	}

	memmove(*buffer + *buffer_length, appended, appended_length);
	*buffer_length = *buffer_length + appended_length;
}

void *memdup(void *mem, size_t size) {
	void *dup = malloc(size);
	if(dup == NULL && size != 0) {
		perror("malloc");
		exit(1);
	}
	memmove(dup, mem, size);
	return dup;
}

void socket_change(int old, int new, short events) {
	size_t socket_index = get_socket_index(old);
	if(socket_index == number_sockets) {
		fprintf(stderr, "%s: socket requested is not in list of sockets\n", program_name);
		exit(1);
	}
	sockets[socket_index].fd = new;
	sockets[socket_index].events = events;
}

void handle_connection(size_t index) {
	struct connection *conn = connections[index];

	if(conn->state == START || conn->state == PATH) {
		// Read data (that's what we're here for) and append to buffer
		char buffer[1024];
		ssize_t amount = recv(conn->sock, &buffer, sizeof(buffer), 0);

		if(amount <= 0) {
			// EOF or error
			remove_connection(index);
			return;
		}

		buffer_append(&conn->buffer, &conn->buffer_size, buffer, amount);
	} else if(conn->state == REQUEST_END) {
		// Read data and keep the last 4 bytes (only interested in \r\n\r\n)
		char buffer[1024];
		size_t buffer_fill = conn->buffer_size;
		memmove(buffer, conn->buffer, buffer_fill);
		ssize_t amount = recv(conn->sock, &buffer + buffer_fill, sizeof(buffer) - buffer_fill, 0);

		if(amount <= 0) {
			// EOF or error
			remove_connection(index);
			return;
		}

		buffer_fill += amount;

		if(buffer_fill < 4) {
			memmove(conn->buffer, buffer, buffer_fill);
			conn->buffer_size = buffer_fill;
		} else {
			memmove(conn->buffer, &buffer[buffer_fill - 4], 4);
			conn->buffer_size = 4;
		}
	}

	if(conn->state == START) {
		// Check buffer's contents to see if we can move to next state
		if(conn->buffer_size >= 4 && memcmp(conn->buffer, "GET ", 4) == 0) {
			// Remove the first 4 bytes (not needed by us) from the buffer
			conn->buffer_size = conn->buffer_size - 4;
			memmove(conn->buffer, conn->buffer + 4, conn->buffer_size);

			conn->state = PATH;
		}
	}

	if(conn->state == PATH) {
		char *path_end = memchr(conn->buffer, ' ', conn->buffer_size);
		if(path_end != NULL) {
			// Copy the path from buffer into separate path buffer
			conn->path_size = path_end - conn->buffer;
			conn->path = memdup(conn->buffer, conn->path_size);

			// Copy max. 4 bytes off the end, in case it has \r\n\r\n
			size_t left_over = conn->buffer_size - conn->path_size;
			char tmpbuf[4];
			size_t tmpbuf_size;
			if(left_over < 4) {
				memmove(&tmpbuf, path_end, left_over);
				tmpbuf_size = left_over;
			} else {
				memmove(&tmpbuf, &conn->buffer[conn->buffer_size - 4], 4);
				tmpbuf_size = 4;
			}

			// Free the buffer and replace it with a tiny one, store the copied bytes
			free(conn->buffer);
			conn->buffer = malloc(4);
			if(conn->buffer == NULL) {
				perror("malloc");
				exit(1);
			}
			memmove(conn->buffer, &tmpbuf, tmpbuf_size);
			conn->buffer_size = tmpbuf_size;

			conn->state = REQUEST_END;
		}
	}

	if(conn->state == REQUEST_END) {
		if(conn->buffer_size >= 4 && memcmp(conn->buffer, "\r\n\r\n", 4) == 0) {
			// Completely remove the buffer
			free(conn->buffer);
			conn->buffer = NULL;
			conn->buffer_size = 0;

			conn->state = CONNECT;
		}
	}

	if(conn->state == CONNECT) {
			// Allocate a fixed buffer for data copying
			conn->buffer = malloc(1024);
			if(conn->buffer == NULL) {
				perror("malloc");
			}
			conn->buffer_size = 1024;

			// Connect to remote and change it to our main socket
			size_t remote_socket = connect_to_remote();
			conn->sock_other = conn->sock;
			conn->sock = remote_socket;

			// Change socket in the table of sockets
			socket_change(conn->sock_other, conn->sock, POLLOUT);

			// Append \r\n to conn->path to make it a valid selector
			buffer_append(&conn->path, &conn->path_size, "\r\n", 2);

			conn->state = REQUEST_WRITE;
			// Do not continue onwards to REQUEST_WRITE's code, because we changed the socket mid-function and REQUEST_WRITE's code assumes function got called with POLLOUT revent
			return;
	}

	if(conn->state == REQUEST_WRITE) {
		char *start = conn->path + conn->written;
		size_t left = conn->path_size - conn->written;
		ssize_t amount = send(conn->sock, start, left, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		conn->written += amount;

		if(conn->written >= conn->path_size) {
			// Completely remove the path
			free(conn->path);
			conn->path = NULL;
			conn->path_size = 0;

			// Change socket to read mode
			socket_change(conn->sock, conn->sock, POLLIN);

			conn->state = READ;
			// Return because socket was changed
			return;
		}
	}

	if(conn->state == READ) {
		ssize_t amount = recv(conn->sock, conn->buffer, conn->buffer_size, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		if(amount == 0) {
			// EOF reached
			remove_connection(index);
			return;
		}

		// Store the amount of data that's been read into the buffer and reset the amount written
		conn->read = amount;
		conn->written = 0;

		// Swap sockets
		int tmp = conn->sock_other;
		conn->sock_other = conn->sock;
		conn->sock = tmp;

		// Change socket in the table of sockets to the other
		socket_change(conn->sock_other, conn->sock, POLLOUT);

		conn->state = WRITE;
		// Return because socket was changed
		return;
	}
	
	if(conn->state == WRITE) {
		char *start = conn->buffer + conn->written;
		size_t left = conn->read - conn->written;
		ssize_t amount = send(conn->sock, start, left, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		conn->written += amount;

		if(conn->written >= conn->read) {
			// Swap sockets
			int tmp = conn->sock_other;
			conn->sock_other = conn->sock;
			conn->sock = tmp;

			// Change socket in the table of sockets to the other
			socket_change(conn->sock_other, conn->sock, POLLIN);

			conn->state = READ;
			// Return because socket was changed
			return;
		}
	}
}

int main(int argc, char **argv) {
	long int server_port = 1234;

	// Store proram name for later use
	if(argc < 1) {
		fprintf(stderr, "Missing program name\n");
		exit(1);
	} else {
		char *argv0 = strdup(argv[0]);
		if(argv0 == NULL) {
			perror("strdup");
			exit(1);
		}
		program_name = strdup(basename(argv0));
		if(program_name == NULL) {
			perror("strdup");
			exit(1);
		}
		free(argv0);
	}

	// Do option handling
	struct option long_options[] = {
		{"help", no_argument, 0, 0},
		{"port", required_argument, 0, 'p'},
		{0, 0, 0, 0}
	};

	for(;;) {
		int long_option_index;
		int opt = getopt_long(argc, argv, "p:", long_options, &long_option_index);

		if(opt == -1) {
			break;
		}

		switch(opt) {
			case 0: // Long option with no short equivalent
				if(strcmp(long_options[long_option_index].name, "help") == 0) {
					help(stdout);
					exit(0);
				}
				break;;

			case 'p':
				server_port = parse_port(optarg);
				if(server_port < 0) {
					usage(stderr);
					exit(1);
				}
				break;;

			default:
				usage(stderr);
				exit(1);
		}
	}

	if(optind == argc - 2) {
		// 2 arguments left -> remote and remote_port
		remote = argv[optind];
		remote_port = parse_port(argv[optind + 1]);
		if(remote_port < 0) {
			usage(stderr);
			exit(1);
		}
	} else if(optind == argc - 1) {
		// 1 argument left -> only remote
		remote = argv[optind];
	} else {
		usage(stderr);
		exit(1);
	}

	// getaddrinfo wants port as a string, so stringify it
	if(!stringify_port(remote_port, remote_port_string, sizeof(remote_port_string))) {
		fprintf(stderr, "%s: Could not convert %li to string\n", program_name, remote_port);
		exit(1);
	}

	printf("%s %s\n", remote, remote_port_string);

	// Populate the table of sockets with all possible sockets to listen on
	setup_listen(server_port);

	// Poll
	while(1) {
		int amount_ready = poll(sockets, number_sockets, -1);
		if(amount_ready < 0) {
			perror("poll");
			exit(1);
		}

		for(size_t i = 0; i < number_sockets && amount_ready > 0; i++) {
			// While the order of sockets in the table gets rearranged if one is removed, the rearrangement only affects sockets created after the removed one
			// Thus, as long as an interface is not removed from the table, all sockets < number_interfaces are interfaces and other data sockets
			if(i < number_interfaces) {
				// Interface socket
				if(sockets[i].revents & POLLIN) {
					struct sockaddr_storage client_addr;
					socklen_t addr_size = sizeof(client_addr);

					int sock = accept(sockets[i].fd, (struct sockaddr *)&client_addr, &addr_size);

					add_connection(sock);

					amount_ready--;
				}
			} else {
				if(sockets[i].revents & (POLLHUP | POLLIN | POLLOUT)) {
					// Data socket
					size_t connection_index = get_connection_index(sockets[i].fd);

					if(connection_index == number_connections) {
						fprintf(stderr, "%s: socket does not correspond to any connection\n", program_name);
					}

					if(sockets[i].revents & POLLHUP) {
						remove_connection(connection_index);
					} else {
						handle_connection(connection_index);
					}

					amount_ready--;
				}
			}
		}
	}
}
