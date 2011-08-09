
/*
 * Copyright (C) 2011 Mail.RU
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include <tnt_queue.h>
#include <tnt_error.h>
#include <tnt_mem.h>
#include <tnt_leb128.h>
#include <tnt_tuple.h>

void
tnt_tuple_init(struct tnt_tuple *tuple)
{
	tuple->count    = 0;
	tuple->size_enc = 4; /* cardinality */
	STAILQ_INIT(&tuple->list);
}

void
tnt_tuple_free(struct tnt_tuple *tuple)
{
	struct tnt_tuple_field *f, *fnext;
	STAILQ_FOREACH_SAFE(f, &tuple->list, next, fnext) {
		if (f->data)
			tnt_mem_free(f->data);
		tnt_mem_free(f);
	}
}

struct tnt_tuple_field* 
tnt_tuple_add(struct tnt_tuple *tuple, char *data, unsigned int size)
{
	struct tnt_tuple_field *f =
		tnt_mem_alloc(sizeof(struct tnt_tuple_field));
	if (f == NULL)
		return NULL;
	f->size = size;
	f->size_leb = tnt_leb128_size(size);
	f->data = NULL;
	if (data) {
		f->data = tnt_mem_alloc(size);
		if (f->data == NULL) {
			tnt_mem_free(f);
			return NULL;
		}
		memcpy(f->data, data, f->size);
	}
	tuple->count++;
	tuple->size_enc += f->size_leb;
	tuple->size_enc += size;
	STAILQ_INSERT_TAIL(&tuple->list, f, next);
	return f;
}

struct tnt_tuple_field*
tnt_tuple_get(struct tnt_tuple *tuple, unsigned int field)
{
	int c = 0;
	struct tnt_tuple_field *iter;
	if (field >= tuple->count)
		return NULL;
	TNT_TUPLE_FOREACH(tuple, iter) {
		if (field == c++)
			return iter;
	}
	return NULL;
}

int
tnt_tuplef(struct tnt_tuple *tuple, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char *p = fmt;
	while (*p) {
		if (isspace(*p)) {
			p++;
			continue;
		} else
		if (*p != '%')
			return -1;
		p++;
		switch (*p) {
		case '*': {
			if (*(p + 1) == 's') {
				int len = va_arg(args, int);
				char *s = va_arg(args, char*);
				tnt_tuple_add(tuple, s, len);
				p += 2;
			} else
				return -1;
			break;
		}
		case 's': {
			char *s = va_arg(args, char*);
			tnt_tuple_add(tuple, s, strlen(s));
			p++;
			break;
		}
		case 'd': {
			int i = va_arg(args, int);
			tnt_tuple_add(tuple, (char*)&i, sizeof(int));
			p++;
			break;
		}	
		case 'u':
			if (*(p + 1) == 'l') {
				if (*(p + 2) == 'l') {
					unsigned long long int ull = va_arg(args, unsigned long long);
					tnt_tuple_add(tuple, (char*)&ull, sizeof(unsigned long long int));
					p += 3;
				} else {
					unsigned long int ul = va_arg(args, unsigned long int);
					tnt_tuple_add(tuple, (char*)&ul, sizeof(unsigned long int));
					p += 2;
				}
			} else
				return -1;
			break;
		case 'l':
			if (*(p + 1) == 'l') {
				long long int ll = va_arg(args, int);
				tnt_tuple_add(tuple, (char*)&ll, sizeof(long long int));
				p += 2;
			} else {
				long int l = va_arg(args, int);
				tnt_tuple_add(tuple, (char*)&l, sizeof(long int));
				p++;
			}
			break;
		default:
			return -1;
		}
	}
	va_end(args);
	return 0;
}

enum tnt_error
tnt_tuple_pack(struct tnt_tuple *tuple, char **data, unsigned int *size)
{
	*size = tuple->size_enc;
	*data = tnt_mem_alloc(tuple->size_enc);
	if (*data == NULL)
		return TNT_EMEMORY;
	char *p = *data;
	memcpy(p, &tuple->count, 4);
	p += 4;

	struct tnt_tuple_field *f;
	TNT_TUPLE_FOREACH(tuple, f) {
		tnt_leb128_write(p, f->size);
		p += f->size_leb;
		if (f->data) {
			memcpy(p, f->data, f->size); 
			p += f->size;
		}
	}
	return TNT_EOK;
}

enum tnt_error
tnt_tuple_pack_to(struct tnt_tuple *tuple, char *dest)
{
	memcpy(dest, &tuple->count, 4);
	dest += 4;
	struct tnt_tuple_field *f;
	TNT_TUPLE_FOREACH(tuple, f) {
		tnt_leb128_write(dest, f->size);
		dest += f->size_leb;
		if (f->data) {
			memcpy(dest, f->data, f->size); 
			dest += f->size;
		}
	}
	return TNT_EOK;
}

void
tnt_tuples_init(struct tnt_tuples *tuples)
{
	tuples->count = 0;
	STAILQ_INIT(&tuples->list);
}

void
tnt_tuples_free(struct tnt_tuples *tuples)
{
	struct tnt_tuple *t, *tnext;
	STAILQ_FOREACH_SAFE(t, &tuples->list, next, tnext) {
		tnt_tuple_free(t);
		tnt_mem_free(t);
	}
}

struct tnt_tuple*
tnt_tuples_add(struct tnt_tuples *tuples)
{
	struct tnt_tuple *t =
		tnt_mem_alloc(sizeof(struct tnt_tuple));
	if (t == NULL)
		return NULL;
	tnt_tuple_init(t);
	tuples->count++;
	STAILQ_INSERT_TAIL(&tuples->list, t, next);
	return t;
}

enum tnt_error
tnt_tuples_pack(struct tnt_tuples *tuples, char **data, unsigned int *size)
{
	if (tuples->count == 0)
		return TNT_EEMPTY;
	*size = 4; /* count */

	struct tnt_tuple *t;
	STAILQ_FOREACH(t, &tuples->list, next)
		*size += t->size_enc;

	*data = tnt_mem_alloc(*size);
	if (*data == NULL)
		return TNT_EMEMORY;

	char *p = *data;
	memcpy(p, &tuples->count, 4);
	p += 4;

	STAILQ_FOREACH(t, &tuples->list, next) {
		enum tnt_error result = tnt_tuple_pack_to(t, p);
		if (result != TNT_EOK) {
			tnt_mem_free(*data);
			*data = NULL;
			*size = 0;
			return result;
		}
		p += t->size_enc;
	}
	return TNT_EOK;
}

enum tnt_error
tnt_tuples_unpack(struct tnt_tuples *tuples, char *data, unsigned int size)
{
	struct tnt_tuple *t = tnt_tuples_add(tuples);
	if (t == NULL)
		return TNT_EMEMORY;

	char *p = data;
	uint32_t i, c = *(uint32_t*)p;
	int off	= 4;
	p += 4;

	for (i = 0 ; i < c ; i++) {
		uint32_t s;
		int r = tnt_leb128_read(p, size - off, &s);
		if (r == -1) 
			return TNT_EPROTO;
		off += r, p += r;
		if (s > (uint32_t)(size - off))
			return TNT_EPROTO;
		if (tnt_tuple_add(t, p, s) == NULL)
			return TNT_EMEMORY;
		off += s, p+= s;
	}
	return TNT_EOK;
}
