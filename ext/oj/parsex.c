/* parse.c
 * Copyright (c) 2013, Peter Ohler
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  - Neither the name of Peter Ohler nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "oj.h"
#include "parsex.h"
#include "buf.h"
#include "val_stack.h"

// Workaround in case INFINITY is not defined in math.h or if the OS is CentOS
#define OJ_INFINITY	(1.0/0.0)

#ifdef RUBINIUS_RUBY
#define NUM_MAX		0x07FFFFFF
#else
#define NUM_MAX		(FIXNUM_MAX >> 8)
#endif
#define EXP_MAX		1023
#define DEC_MAX		14

static void
skip_comment(ParseInfo pi) {
    char	c = reader_get(&pi->rd);

    if ('*' == c) {
	while ('\0' != (c = reader_get(&pi->rd))) {
	    if ('*' == c) {
		c = reader_get(&pi->rd);
		if ('/' == c) {
		    return;
		}
	    }
	}
    } else if ('/' == c) {
	while ('\0' != (c = reader_get(&pi->rd))) {
	    switch (c) {
	    case '\n':
	    case '\r':
	    case '\f':
	    case '\0':
		return;
	    default:
		break;
	    }
	}
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid comment format");
    }
    if ('\0' == c) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "comment not terminated");
	return;
    }
}

static void
add_value(ParseInfo pi, VALUE rval) {
    Val	parent = stack_peek(&pi->stack);

    if (0 == parent) { // simple add
	pi->add_value(pi, rval);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_value(pi, rval);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_value(pi, parent->key, parent->klen, rval);
	    // TBD key should be protected
	    // - check key is between head and read_end or just end
	    if (0 != parent->key && !reader_ptr_in_pro(&pi->rd, parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s", oj_stack_next_string(parent->next));
	    break;
	}
    }
}

static void
add_num_value(ParseInfo pi, NumInfo ni) {
    Val	parent = stack_peek(&pi->stack);

    if (0 == parent) {
	pi->add_num(pi, ni);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_num(pi, ni);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_num(pi, parent->key, parent->klen, ni);
	    if (0 != parent->key && !reader_ptr_in_pro(&pi->rd, parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	default:
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s", oj_stack_next_string(parent->next));
	    break;
	}
    }
}

static void
read_true(ParseInfo pi) {
    if (0 == reader_expect(&pi->rd, "rue")) {
	add_value(pi, Qtrue);
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected true");
    }
}

static void
read_false(ParseInfo pi) {
    if (0 == reader_expect(&pi->rd, "alse")) {
	add_value(pi, Qfalse);
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected false");
    }
}

static uint32_t
read_hex(ParseInfo pi) {
    uint32_t	b = 0;
    int		i;
    char	c;

    for (i = 0; i < 4; i++) {
	c = reader_get(&pi->rd);
	b = b << 4;
	if ('0' <= c && c <= '9') {
	    b += c - '0';
	} else if ('A' <= c && c <= 'F') {
	    b += c - 'A' + 10;
	} else if ('a' <= c && c <= 'f') {
	    b += c - 'a' + 10;
	} else {
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid hex character");
	    return 0;
	}
    }
    return b;
}

static void
unicode_to_chars(ParseInfo pi, Buf buf, uint32_t code) {
    if (0x0000007F >= code) {
	buf_append(buf, (char)code);
    } else if (0x000007FF >= code) {
	buf_append(buf, 0xC0 | (code >> 6));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x0000FFFF >= code) {
	buf_append(buf, 0xE0 | (code >> 12));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x001FFFFF >= code) {
	buf_append(buf, 0xF0 | (code >> 18));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x03FFFFFF >= code) {
	buf_append(buf, 0xF8 | (code >> 24));
	buf_append(buf, 0x80 | ((code >> 18) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x7FFFFFFF >= code) {
	buf_append(buf, 0xFC | (code >> 30));
	buf_append(buf, 0x80 | ((code >> 24) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 18) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid Unicode character");
    }
}

// entered at backslash
static void
read_escaped_str(ParseInfo pi) {
    struct _Buf	buf;
    char	c;
    uint32_t	code;
    Val		parent = stack_peek(&pi->stack);

    buf_init(&buf);
    if (pi->rd.str < pi->rd.tail) {
	buf_append_string(&buf, pi->rd.str, pi->rd.tail - pi->rd.str);
    }
    while ('\"' != (c = reader_get(&pi->rd))) {
	if ('\0' == c) {
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "quoted string not terminated");
	    buf_cleanup(&buf);
	    return;
	} else if ('\\' == c) {
	    c = reader_get(&pi->rd);
	    switch (c) {
	    case 'n':	buf_append(&buf, '\n');	break;
	    case 'r':	buf_append(&buf, '\r');	break;
	    case 't':	buf_append(&buf, '\t');	break;
	    case 'f':	buf_append(&buf, '\f');	break;
	    case 'b':	buf_append(&buf, '\b');	break;
	    case '"':	buf_append(&buf, '"');	break;
	    case '/':	buf_append(&buf, '/');	break;
	    case '\\':	buf_append(&buf, '\\');	break;
	    case 'u':
		// TBD 
		if (0 == (code = read_hex(pi)) && err_has(&pi->err)) {
		    buf_cleanup(&buf);
		    return;
		}
		if (0x0000D800 <= code && code <= 0x0000DFFF) {
		    uint32_t	c1 = (code - 0x0000D800) & 0x000003FF;
		    uint32_t	c2;
		    char	ch2;

		    c = reader_get(&pi->rd);
		    ch2 = reader_get(&pi->rd);
		    if ('\\' != c || 'u' != ch2) {
			oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid escaped character");
			buf_cleanup(&buf);
			return;
		    }
		    if (0 == (c2 = read_hex(pi)) && err_has(&pi->err)) {
			buf_cleanup(&buf);
			return;
		    }
		    c2 = (c2 - 0x0000DC00) & 0x000003FF;
		    code = ((c1 << 10) | c2) + 0x00010000;
		}
		unicode_to_chars(pi, &buf, code);
		if (err_has(&pi->err)) {
		    buf_cleanup(&buf);
		    return;
		}
		break;
	    default:
		oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid escaped character");
		buf_cleanup(&buf);
		return;
	    }
	} else {
	    buf_append(&buf, c);
	}
    }
    if (0 == parent) {
	pi->add_cstr(pi, buf.head, buf_len(&buf), pi->rd.str);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_cstr(pi, buf.head, buf_len(&buf), pi->rd.str);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	    // key will not be between pi->json and pi->cur.
	    parent->key = strdup(buf.head);
	    parent->klen = buf_len(&buf);
	    parent->k1 = *pi->rd.str;
	    parent->next = NEXT_HASH_COLON;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_cstr(pi, parent->key, parent->klen, buf.head, buf_len(&buf), pi->rd.str);
	    if (0 != parent->key && !reader_ptr_in_pro(&pi->rd, parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a string", oj_stack_next_string(parent->next));
	    break;
	}
    }
    buf_cleanup(&buf);
}

static void
read_str(ParseInfo pi) {
    Val		parent = stack_peek(&pi->stack);
    char	c;

    reader_protect(&pi->rd);
    while ('\"' != (c = reader_get(&pi->rd))) {
	if ('\0' == c) {
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "quoted string not terminated");
	    return;
	} else if ('\\' == c) {
	    read_escaped_str(pi);
	    reader_reset(&pi->rd);
	    return;
	}
    }
    if (0 == parent) { // simple add
	pi->add_cstr(pi, pi->rd.str, pi->rd.tail - pi->rd.str, pi->rd.str);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_cstr(pi, pi->rd.str, pi->rd.tail - pi->rd.str, pi->rd.str);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	    parent->key = pi->rd.str;
	    parent->klen = pi->rd.tail - pi->rd.str;
	    parent->k1 = *pi->rd.str;
	    parent->next = NEXT_HASH_COLON;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_cstr(pi, parent->key, parent->klen, pi->rd.str, pi->rd.tail - pi->rd.str, pi->rd.str);
	    if (0 != parent->key && !reader_ptr_in_pro(&pi->rd, parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a string", oj_stack_next_string(parent->next));
	    break;
	}
    }
    reader_reset(&pi->rd);
}

static void
read_num(ParseInfo pi, char c) {
    struct _NumInfo	ni;
    int			zero_cnt = 0;

    reader_protect(&pi->rd);
    ni.str = pi->rd.str;
    ni.i = 0;
    ni.num = 0;
    ni.div = 1;
    ni.len = 0;
    ni.exp = 0;
    ni.dec_cnt = 0;
    ni.big = 0;
    ni.infinity = 0;
    ni.nan = 0;
    ni.neg = 0;
    ni.no_big = (FloatDec == pi->options.bigdec_load);

    if ('-' == c) {
	c = reader_get(&pi->rd);
	ni.neg = 1;
    } else if ('+' == c) {
	c = reader_get(&pi->rd);
    }
    if ('I' == c) {
	if (0 != reader_expect(&pi->rd, "nfinity")) {
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "not a number or other value");
	    return;
	}
	ni.infinity = 1;
    } else if ('N' == c || 'n' == c) {
	char	c2;

	c = reader_get(&pi->rd);
	c2 = reader_get(&pi->rd);
	if ('a' != c || ('N' != c2 && 'n' != c2)) {
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "not a number or other value");
	    return;
	}
	ni.nan = 1;
    } else {
	for (; '0' <= c && c <= '9'; c = reader_get(&pi->rd)) {
	    ni.dec_cnt++;
	    if (ni.big) {
		ni.big++;
	    } else {
		int	d = (c - '0');

		if (0 == d) {
		    zero_cnt++;
		} else {
		    zero_cnt = 0;
		}
		ni.i = ni.i * 10 + d;
		if (LONG_MAX <= ni.i || DEC_MAX < ni.dec_cnt - zero_cnt) {
		    ni.big = 1;
		}
	    }
	}
	if ('.' == c) {
	    c = reader_get(&pi->rd);
	    for (; '0' <= c && c <= '9'; c = reader_get(&pi->rd)) {
		int	d = (c - '0');

		if (0 == d) {
		    zero_cnt++;
		} else {
		    zero_cnt = 0;
		}
		ni.dec_cnt++;
		ni.num = ni.num * 10 + d;
		ni.div *= 10;
		if (LONG_MAX <= ni.div || DEC_MAX < ni.dec_cnt - zero_cnt) {
		    ni.big = 1;
		}
	    }
	}
	if ('e' == c || 'E' == c) {
	    int	eneg = 0;

	    c = reader_get(&pi->rd);
	    if ('-' == c) {
		c = reader_get(&pi->rd);
		eneg = 1;
	    } else if ('+' == c) {
		c = reader_get(&pi->rd);
	    }
	    for (; '0' <= c && c <= '9'; c = reader_get(&pi->rd)) {
		ni.exp = ni.exp * 10 + (c - '0');
		if (EXP_MAX <= ni.exp) {
		    ni.big = 1;
		}
	    }
	    if (eneg) {
		ni.exp = -ni.exp;
	    }
	}
	ni.dec_cnt -= zero_cnt;
	ni.len = pi->rd.tail - pi->rd.str;
    }
    if (BigDec == pi->options.bigdec_load) {
	ni.big = 1;
    }
    add_num_value(pi, &ni);
}

static void
array_start(ParseInfo pi) {
    VALUE	v = pi->start_array(pi);

    stack_push(&pi->stack, v, NEXT_ARRAY_NEW);
}

static void
array_end(ParseInfo pi) {
    Val	array = stack_pop(&pi->stack);

    if (0 == array) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected array close");
    } else if (NEXT_ARRAY_COMMA != array->next && NEXT_ARRAY_NEW != array->next) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not an array close", oj_stack_next_string(array->next));
    } else {
	pi->end_array(pi);
	add_value(pi, array->val);
    }
}

static void
hash_start(ParseInfo pi) {
    volatile VALUE	v = pi->start_hash(pi);

    stack_push(&pi->stack, v, NEXT_HASH_NEW);
}

static void
hash_end(ParseInfo pi) {
    volatile Val	hash = stack_peek(&pi->stack);

    // leave hash on stack until just before 
    if (0 == hash) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected hash close");
    } else if (NEXT_HASH_COMMA != hash->next && NEXT_HASH_NEW != hash->next) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a hash close", oj_stack_next_string(hash->next));
    } else {
	pi->end_hash(pi);
	stack_pop(&pi->stack);
	add_value(pi, hash->val);
    }
}

static void
comma(ParseInfo pi) {
    Val	parent = stack_peek(&pi->stack);

    if (0 == parent) {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected comma");
    } else if (NEXT_ARRAY_COMMA == parent->next) {
	parent->next = NEXT_ARRAY_ELEMENT;
    } else if (NEXT_HASH_COMMA == parent->next) {
	parent->next = NEXT_HASH_KEY;
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected comma");
    }
}

static void
colon(ParseInfo pi) {
    Val	parent = stack_peek(&pi->stack);

    if (0 != parent && NEXT_HASH_COLON == parent->next) {
	parent->next = NEXT_HASH_VALUE;
    } else {
	oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected colon");
    }
}

void
oj_parse2x(ParseInfo pi) {
    char	c;

    err_init(&pi->err);
    while (1) {
	c = reader_next_non_white(&pi->rd);
	switch (c) {
	case '{':
	    hash_start(pi);
	    break;
	case '}':
	    hash_end(pi);
	    break;
	case ':':
	    colon(pi);
	    break;
	case '[':
	    array_start(pi);
	    break;
	case ']':
	    array_end(pi);
	    break;
	case ',':
	    comma(pi);
	    break;
	case '"':
	    read_str(pi);
	    break;
	case '+':
	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case 'I':
	case 'N':
	    read_num(pi, c);
	    break;
	case 't':
	    read_true(pi);
	    break;
	case 'f':
	    read_false(pi);
	    break;
	case 'n':
	    // TBD protect or checkpoint
	    c = reader_get(&pi->rd);
	    if ('u' == c) {
		if (0 == reader_expect(&pi->rd, "ll")) {
		    add_value(pi, Qnil);
		} else {
		    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected null");
		    return;
		}
	    } else if ('a' == c) {
		struct _NumInfo	ni;

		c = reader_get(&pi->rd);
		if ('N' != c && 'n' != c) {
		    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "expected NaN");
		    return;
		}
		ni.str = pi->rd.str;
		ni.i = 0;
		ni.num = 0;
		ni.div = 1;
		ni.len = 0;
		ni.exp = 0;
		ni.dec_cnt = 0;
		ni.big = 0;
		ni.infinity = 0;
		ni.nan = 1;
		ni.neg = 0;
		ni.no_big = (FloatDec == pi->options.bigdec_load);
		add_num_value(pi, &ni);
	    } else {
		oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid token");
		return;
	    }
	    break;
	case '/':
	    skip_comment(pi);
	    break;
	case '\0':
	    return;
	default:
	    oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected character");
	    return;
	}
	if (err_has(&pi->err)) {
	    return;
	}
	if (Qundef != pi->proc && stack_empty(&pi->stack)) {
	    if (Qnil == pi->proc) {
		rb_yield(stack_head_val(&pi->stack));
	    } else {
#if HAS_PROC_WITH_BLOCK
		VALUE	args[1];

		*args = stack_head_val(&pi->stack);
		rb_proc_call_with_block(pi->proc, 1, args, Qnil);
#else
		rb_raise(rb_eNotImpError, "Calling a Proc with a block not supported in this version. Use func() {|x| } syntax instead.");
#endif
	    }
	}
    }
}

VALUE
oj_num_as_valuex(NumInfo ni) {
    VALUE	rnum = Qnil;

    if (ni->infinity) {
	if (ni->neg) {
	    rnum = rb_float_new(-OJ_INFINITY);
	} else {
	    rnum = rb_float_new(OJ_INFINITY);
	}
    } else if (ni->nan) {
	rnum = rb_float_new(0.0/0.0);
    } else if (1 == ni->div && 0 == ni->exp) { // fixnum
	if (ni->big) {
	    if (256 > ni->len) {
		char	buf[256];

		memcpy(buf, ni->str, ni->len);
		buf[ni->len] = '\0';
		rnum = rb_cstr_to_inum(buf, 10, 0);
	    } else {
		char	*buf = ALLOC_N(char, ni->len + 1);

		memcpy(buf, ni->str, ni->len);
		buf[ni->len] = '\0';
		rnum = rb_cstr_to_inum(buf, 10, 0);
		xfree(buf);
	    }
	} else {
	    if (ni->neg) {
		rnum = LONG2NUM(-ni->i);
	    } else {
		rnum = LONG2NUM(ni->i);
	    }
	}
    } else { // decimal
	if (ni->big) {
	    rnum = rb_funcall(oj_bigdecimal_class, oj_new_id, 1, rb_str_new(ni->str, ni->len));
	    if (ni->no_big) {
		rnum = rb_funcall(rnum, rb_intern("to_f"), 0);
	    }
	} else {
	    double	d = (double)ni->i + (double)ni->num / (double)ni->div;

	    if (ni->neg) {
		d = -d;
	    }
	    if (0 != ni->exp) {
		d *= pow(10.0, ni->exp);
	    }
	    rnum = rb_float_new(d);
	}
    }
    return rnum;
}

void
oj_set_error_atx(ParseInfo pi, VALUE err_clas, const char* file, int line, const char *format, ...) {
    va_list	ap;
    char	msg[128];

    va_start(ap, format);
    vsnprintf(msg, sizeof(msg) - 1, format, ap);
    va_end(ap);
    pi->err.clas = err_clas;
    // TBD set error
    //_oj_err_set_with_location(&pi->err, err_clas, msg, pi->json, pi->cur - 1, file, line);
}

static VALUE
protect_parse(VALUE pip) {
    oj_parse2x((ParseInfo)pip);

    return Qnil;
}

VALUE
oj_pi_parsex(int argc, VALUE *argv, ParseInfo pi) {
    char		*buf = 0;
    volatile VALUE	input;
    volatile VALUE	wrapped_stack;
    VALUE		result = Qnil;
    int			line = 0;

    if (argc < 1) {
	rb_raise(rb_eArgError, "Wrong number of arguments to parse.");
    }
    input = argv[0];
    if (2 == argc) {
	oj_parse_options(argv[1], &pi->options);
    }
    if (rb_block_given_p()) {
	pi->proc = Qnil;
    } else {
	pi->proc = Qundef;
    }
    pi->cbc = (void*)0;

    oj_reader_init(&pi->rd, input);

    if (Yes == pi->options.circular) {
	pi->circ_array = oj_circ_array_new();
    } else {
	pi->circ_array = 0;
    }
    if (No == pi->options.allow_gc) {
	rb_gc_disable();
    }
    // GC can run at any time. When it runs any Object created by C will be
    // freed. We protect against this by wrapping the value stack in a ruby
    // data object and poviding a mark function for ruby objects on the
    // value stack (while it is in scope).
    wrapped_stack = oj_stack_init(&pi->stack);
    rb_protect(protect_parse, (VALUE)pi, &line);
    result = stack_head_val(&pi->stack);
    DATA_PTR(wrapped_stack) = 0;
    if (No == pi->options.allow_gc) {
	rb_gc_enable();
    }
    if (!err_has(&pi->err)) {
	// If the stack is not empty then the JSON terminated early.
	Val	v;

	if (0 != (v = stack_peek(&pi->stack))) {
	    switch (v->next) {
	    case NEXT_ARRAY_NEW:
	    case NEXT_ARRAY_ELEMENT:
	    case NEXT_ARRAY_COMMA:
		oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "Array not terminated");
		break;
	    case NEXT_HASH_NEW:
	    case NEXT_HASH_KEY:
	    case NEXT_HASH_COLON:
	    case NEXT_HASH_VALUE:
	    case NEXT_HASH_COMMA:
		oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "Hash/Object not terminated");
		break;
	    default:
		oj_set_error_atx(pi, oj_parse_error_class, __FILE__, __LINE__, "not terminated");
	    }
	}
    }
    // proceed with cleanup
    if (0 != pi->circ_array) {
	oj_circ_array_free(pi->circ_array);
    }
    if (0 != buf) {
	xfree(buf);
    }
    stack_cleanup(&pi->stack);
    if (0 != line) {
	rb_jump_tag(line);
    }
    if (err_has(&pi->err)) {
	oj_err_raise(&pi->err);
    }
    return result;
}
