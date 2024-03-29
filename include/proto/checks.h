/*
  include/proto/checks.h
  Functions prototypes for the checks.

  Copyright (C) 2000-2009 Willy Tarreau - w@1wt.eu
  
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, version 2.1
  exclusively.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef _PROTO_CHECKS_H
#define _PROTO_CHECKS_H

#include <types/task.h>
#include <common/config.h>

const char *get_check_status_description(short check_status);
const char *get_check_status_info(short check_status);
void set_server_down(struct check *check);
void set_server_up(struct check *check);
int start_checks();
void health_adjust(struct server *s, short status);

extern struct data_cb check_conn_cb;

#endif /* _PROTO_CHECKS_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
