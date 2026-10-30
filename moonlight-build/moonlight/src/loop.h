/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>

#define LOOP_RETURN 1
#define LOOP_OK 0
#define LOOP_ERROR -1

typedef int(*FdHandler)(int fd);

void loop_add_fd(int fd, FdHandler handler, int events);
void loop_remove_fd(int fd);

/* Called before evdev/libCEC workers are spawned so they inherit SIG_BLOCK. */
void loop_block_termination_signals(void);
void loop_init();
int loop_main();
void loop_request_stop(void);
/* Asynchronous backend/transport failure must not be mistaken for Back/Exit. */
void loop_request_error_stop(void);
/* Shared result for SDL and embedded backends (SDL has no signalfd loop). */
bool loop_stream_failed(void);
