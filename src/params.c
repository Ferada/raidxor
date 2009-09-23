/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

static int number_of_cache_lines = 10;
module_param(number_of_cache_lines, int, S_IRUGO);
