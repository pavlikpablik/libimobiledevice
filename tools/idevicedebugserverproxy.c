/*
 * idevicedebugserverproxy.c
 * Proxy a debugserver connection from device for remote debugging
 *
 * Copyright (c) 2012 Martin Szulecki All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/debugserver.h>

#include "common/socket.h"
#include "common/thread.h"

#define info(...) fprintf(stdout, __VA_ARGS__); fflush(stdout)
#define debug(...) if(debug_mode) fprintf(stdout, __VA_ARGS__)

static int debug_mode = 0;
static int quit_flag = 0;

typedef struct {
	int server_fd;
	int client_fd;
	uint16_t local_port;
	idevice_t device;
	debugserver_client_t debugserver_client;
	volatile int stop_ctod;
	volatile int stop_dtoc;
} socket_info_t;

static socket_info_t global_socket_info;

static void clean_exit(int sig)
{
	if (quit_flag == 0)
		fprintf(stderr, "Setting quit status. Cancel again to quit server.\n");
	else
		fprintf(stderr, "Exiting...\n");

	quit_flag++;

	/* shutdown server socket if we have to terminate to unblock the server loop */
	if (quit_flag > 1) {
		if (global_socket_info.server_fd > 0) {
			socket_shutdown(global_socket_info.server_fd, SHUT_RDWR);
			socket_close(global_socket_info.server_fd);
		}
	}
}

static void print_usage(int argc, char **argv)
{
	char *name = NULL;

	name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS] <PORT>\n", (name ? name + 1: argv[0]));
	printf("Proxy debugserver connection from device to a local socket at PORT.\n\n");
	printf("  -d, --debug\t\tenable communication debugging\n");
	printf("  -u, --udid UDID\ttarget specific device by its 40-digit device UDID\n");
	printf("  -h, --help\t\tprints usage information\n");
	printf("\n");
}

static void *thread_device_to_client(void *data)
{
	socket_info_t* socket_info = (socket_info_t*)data;
	idevice_error_t res = IDEVICE_E_UNKNOWN_ERROR;

	int recv_len;
	int sent;
	char buffer[131072];

	debug("%s: started thread...\n", __func__);

	debug("%s: client_fd = %d\n", __func__, socket_info->client_fd);
	debug("%s: server fd = %d\n", __func__, socket_info->server_fd);

	while (!quit_flag && !socket_info->stop_dtoc && socket_info->client_fd > 0 && socket_info->server_fd > 0) {
		debug("%s: receiving data from device...\n", __func__);

		res = debugserver_client_receive_with_timeout(socket_info->debugserver_client, buffer, sizeof(buffer), (uint32_t*)&recv_len, 5000);

		if (recv_len <= 0) {
			if (recv_len == 0 && res == IDEVICE_E_SUCCESS) {
				// try again
				continue;
			} else {
				fprintf(stderr, "recv failed: %s\n", strerror(errno));
				break;
			}
		} else {
			/* send to device */
			debug("%s: sending data to client...\n", __func__);
			sent = socket_send(socket_info->client_fd, buffer, recv_len);
			if (sent < recv_len) {
				if (sent <= 0) {
					fprintf(stderr, "send failed: %s\n", strerror(errno));
					break;
				} else {
					fprintf(stderr, "only sent %d from %d bytes\n", sent, recv_len);
				}
			} else {
				// sending succeeded, receive from device
				debug("%s: pushed %d bytes to client\n", __func__, sent);
			}
		}
	}

	debug("%s: shutting down...\n", __func__);

	socket_shutdown(socket_info->client_fd, SHUT_RDWR);
	socket_close(socket_info->client_fd);

	socket_info->client_fd = -1;
	socket_info->stop_ctod = 1;

	return NULL;
}

static void *thread_client_to_device(void *data)
{
	socket_info_t* socket_info = (socket_info_t*)data;
	idevice_error_t res = IDEVICE_E_UNKNOWN_ERROR;

	int recv_len;
	int sent;
	char buffer[131072];
	thread_t dtoc;

	debug("%s: started thread...\n", __func__);

	debug("%s: client_fd = %d\n", __func__, socket_info->client_fd);
	debug("%s: server_fd = %d\n", __func__, socket_info->server_fd);

	/* spawn server to client thread */
	socket_info->stop_dtoc = 0;
	if (thread_new(&dtoc, thread_device_to_client, data) != 0) {
		fprintf(stderr, "Failed to start device to client thread...\n");
	}

	while (!quit_flag && !socket_info->stop_ctod && socket_info->client_fd > 0 && socket_info->server_fd > 0) {
		debug("%s: receiving data from client...\n", __func__);

		/* attempt to read incoming data from client */
		recv_len = socket_receive_timeout(socket_info->client_fd, buffer, sizeof(buffer), 0, 5000);

		/* any data received? */
		if (recv_len <= 0) {
			if (recv_len == 0) {
				/* try again */
				continue;
			} else {
				fprintf(stderr, "Receive failed: %s\n", strerror(errno));
				break;
			}
		} else {
			/* forward data to device */
			debug("%s: sending data to device...\n", __func__);
			res = debugserver_client_send(socket_info->debugserver_client, buffer, recv_len, (uint32_t*)&sent);

			if (sent < recv_len || res != IDEVICE_E_SUCCESS) {
				if (sent <= 0) {
					fprintf(stderr, "send failed: %s\n", strerror(errno));
					break;
				} else {
					fprintf(stderr, "only sent %d from %d bytes\n", sent, recv_len);
				}
			} else {
				// sending succeeded, receive from device
				debug("%s: sent %d bytes to device\n", __func__, sent);
			}
		}
	}

	debug("%s: shutting down...\n", __func__);

	socket_shutdown(socket_info->client_fd, SHUT_RDWR);
	socket_close(socket_info->client_fd);

	socket_info->client_fd = -1;
	socket_info->stop_dtoc = 1;

	/* join other thread to allow it to stop */
	thread_join(dtoc);
	thread_free(dtoc);

	return NULL;
}

static void* connection_handler(void* data)
{
	debugserver_error_t derr = DEBUGSERVER_E_SUCCESS;
	socket_info_t* socket_info = (socket_info_t*)data;
	thread_t ctod;

	debug("%s: client_fd = %d\n", __func__, socket_info->client_fd);

	derr = debugserver_client_start_service(socket_info->device, &socket_info->debugserver_client, "idevicedebugserverproxy");
	if (derr != DEBUGSERVER_E_SUCCESS) {
		fprintf(stderr, "Could not start debugserver on device!\nPlease make sure to mount a developer disk image first.\n");
		return NULL;
	}

	/* spawn client to device thread */
	socket_info->stop_ctod = 0;
	if (thread_new(&ctod, thread_client_to_device, data) != 0) {
		fprintf(stderr, "Failed to start client to device thread...\n");
	}

	/* join the fun */
	thread_join(ctod);
	thread_free(ctod);

	debug("%s: shutting down...\n", __func__);

	debugserver_client_free(socket_info->debugserver_client);
	socket_info->debugserver_client = NULL;

	/* shutdown client socket */
	socket_shutdown(socket_info->client_fd, SHUT_RDWR);
	socket_close(socket_info->client_fd);

	return NULL;
}

int main(int argc, char *argv[])
{
	idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;
	thread_t th;
	const char* udid = NULL;
	uint16_t local_port = 0;
	int result = EXIT_SUCCESS;
	int i;

	/* bind signals */
	signal(SIGINT, clean_exit);
	signal(SIGTERM, clean_exit);
#ifndef WIN32
	signal(SIGQUIT, clean_exit);
	signal(SIGPIPE, SIG_IGN);
#endif

	/* parse cmdline arguments */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
			debug_mode = 1;
			idevice_set_debug_level(1);
			socket_set_verbose(3);
			continue;
		}
		else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--udid")) {
			i++;
			if (!argv[i] || (strlen(argv[i]) != 40)) {
				print_usage(argc, argv);
				return 0;
			}
			udid = argv[i];
			continue;
		}
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argc, argv);
			return EXIT_SUCCESS;
		}
		else if (atoi(argv[i]) > 0) {
			local_port = atoi(argv[i]);
			continue;
		}
		else {
			print_usage(argc, argv);
			return EXIT_SUCCESS;
		}
	}

	/* a PORT is mandatory */
	if (!local_port) {
		fprintf(stderr, "Please specify a PORT.\n");
		print_usage(argc, argv);
		goto leave_cleanup;
	}

	/* setup and create socket endpoint */
	global_socket_info.local_port = local_port;

	/* start services and connect to device */
	ret = idevice_new(&global_socket_info.device, udid);
	if (ret != IDEVICE_E_SUCCESS) {
		if (udid) {
			fprintf(stderr, "No device found with udid %s, is it plugged in?\n", udid);
		} else {
			fprintf(stderr, "No device found, is it plugged in?\n");
		}
		result = EXIT_FAILURE;
		goto leave_cleanup;
	}

	/* create local socket */
	global_socket_info.server_fd = socket_create(global_socket_info.local_port);
	if (global_socket_info.server_fd < 0) {
		fprintf(stderr, "Could not create socket\n");
		result = EXIT_FAILURE;
		goto leave_cleanup;
	}

	while (!quit_flag) {
		debug("%s: Waiting for connection on local port %d\n", __func__, global_socket_info.local_port);

		/* wait for client */
		global_socket_info.client_fd = socket_accept(global_socket_info.server_fd, global_socket_info.local_port);
		if (global_socket_info.client_fd < 0) {
			continue;
		}

		debug("%s: Handling new client connection...\n", __func__);

		if (thread_new(&th, connection_handler, (void*)&global_socket_info) != 0) {
			fprintf(stderr, "Could not start connection handler.\n");
			socket_shutdown(global_socket_info.server_fd, SHUT_RDWR);
			socket_close(global_socket_info.server_fd);
			continue;
		}

		/* we do not need it anymore */
		thread_free(th);
	}

	debug("%s: Shutting down debugserver proxy...\n", __func__);

leave_cleanup:
	if (global_socket_info.device) {
		idevice_free(global_socket_info.device);
	}

	return result;
}
