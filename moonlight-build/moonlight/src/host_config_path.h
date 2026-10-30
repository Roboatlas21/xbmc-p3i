/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* The caller owns the returned allocation and must free it. */
char *moonlight_host_config_path(const char *address);
