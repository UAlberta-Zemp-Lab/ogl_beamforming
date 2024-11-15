/* See LICENSE for license details. */
static i32 hadamard_12_12_transpose[] = {
	1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
	1, -1, -1,  1, -1, -1, -1,  1,  1,  1, -1,  1,
	1,  1, -1, -1,  1, -1, -1, -1,  1,  1,  1, -1,
	1, -1,  1, -1, -1,  1, -1, -1, -1,  1,  1,  1,
	1,  1, -1,  1, -1, -1,  1, -1, -1, -1,  1,  1,
	1,  1,  1, -1,  1, -1, -1,  1, -1, -1, -1,  1,
	1,  1,  1,  1, -1,  1, -1, -1,  1, -1, -1, -1,
	1, -1,  1,  1,  1, -1,  1, -1, -1,  1, -1, -1,
	1, -1, -1,  1,  1,  1, -1,  1, -1, -1,  1, -1,
	1, -1, -1, -1,  1,  1,  1, -1,  1, -1, -1,  1,
	1,  1, -1, -1, -1,  1,  1,  1, -1,  1, -1, -1,
	1, -1,  1, -1, -1, -1,  1,  1,  1, -1,  1, -1,
};

static void *
mem_clear(void *p_, u8 c, size len)
{
	u8 *p = p_;
	while (len) p[--len] = c;
	return p;
}

static void
mem_copy(void *src, void *dest, size n)
{
	ASSERT(n >= 0);
	u8 *s = src, *d = dest;
#if defined(__AVX512BW__)
	/* TODO: aligned load/store and comparison */
	for (; n >= 64; n -= 64, s += 64, d += 64)
		_mm512_storeu_epi8(d, _mm512_loadu_epi8(s));
#endif
	for (; n >= 16; n -= 16, s += 16, d += 16)
		_mm_storeu_si128((__m128i *)d, _mm_loadu_si128((__m128i*)s));
	for (; n; n--) *d++ = *s++;
}

static void
mem_move(u8 *src, u8 *dest, size n)
{
	if (dest < src) mem_copy(src, dest, n);
	else            while (n) { n--; dest[n] = src[n]; }
}

#define alloc(a, t, n)  (t *)alloc_(a, sizeof(t), _Alignof(t), n)
static void *
alloc_(Arena *a, size len, size align, size count)
{
	/* NOTE: special case 0 arena */
	if (a->beg == 0)
		return 0;

	size padding   = -(uintptr_t)a->beg & (align - 1);
	size available = a->end - a->beg - padding;
	if (available < 0 || count > available / len)
		ASSERT(0 && "arena OOM\n");
	void *p = a->beg + padding;
	a->beg += padding + count * len;
	/* TODO: Performance? */
	return mem_clear(p, 0, count * len);
}

static Arena
sub_arena(Arena *a, size size)
{
	Arena result = {0};
	if ((a->end - a->beg) >= size) {
		result.beg  = a->beg;
		result.end  = a->beg + size;
		a->beg     += size;
	}
	return result;
}

static TempArena
begin_temp_arena(Arena *a)
{
	TempArena result = {.arena = a, .old_beg = a->beg};
	return result;
}

static void
end_temp_arena(TempArena ta)
{
	Arena *a = ta.arena;
	if (a) {
		ASSERT(a->beg >= ta.old_beg)
		a->beg = ta.old_beg;
	}
}

static Stream
arena_stream(Arena *a)
{
	Stream result = {0};
	result.data   = a->beg;
	result.cap    = a->end - a->beg;
	a->beg = a->end;
	return result;
}

static Stream
stream_alloc(Arena *a, size cap)
{
	Stream result = {.cap = cap};
	result.data = alloc(a, u8, cap);
	return result;
}

static s8
stream_to_s8(Stream *s)
{
	s8 result = {.len = s->widx, .data = s->data};
	return result;
}

static void
stream_append_byte(Stream *s, u8 b)
{
	s->errors |= s->widx + 1 > s->cap;
	if (!s->errors)
		s->data[s->widx++] = b;
}

static void
stream_append_s8(Stream *s, s8 str)
{
	s->errors |= (s->cap - s->widx) < str.len;
	if (!s->errors) {
		mem_copy(str.data, s->data + s->widx, str.len);
		s->widx += str.len;
	}
}

static void
stream_append_s8_array(Stream *s, s8 *strs, size count)
{
	while (count > 0) {
		stream_append_s8(s, *strs);
		strs++;
		count--;
	}
}

static void
stream_append_u64(Stream *s, u64 n)
{
	u8 tmp[64];
	u8 *end = tmp + sizeof(tmp);
	u8 *beg = end;
	do { *--beg = '0' + (n % 10); } while (n /= 10);
	stream_append_s8(s, (s8){.len = end - beg, .data = beg});
}

static void
stream_append_i64(Stream *s, i64 n)
{
	if (n < 0) {
		stream_append_byte(s, '-');
		n *= -1;
	}
	stream_append_u64(s, n);
}

static void
stream_append_f64(Stream *s, f64 f, i64 prec)
{
	if (f < 0) {
		stream_append_byte(s, '-');
		f *= -1;
	}

	/* NOTE: round last digit */
	f += 0.5f / prec;

	if (f >= (f64)(-1UL >> 1)) {
		stream_append_s8(s, s8("inf"));
	} else {
		u64 integral = f;
		u64 fraction = (f - integral) * prec;
		stream_append_u64(s, integral);
		stream_append_byte(s, '.');
		for (i64 i = prec / 10; i > 1; i /= 10) {
			if (i > fraction)
				stream_append_byte(s, '0');
		}
		stream_append_u64(s, fraction);
	}
}

static void
stream_append_f64_e(Stream *s, f64 f)
{
	/* TODO: there should be a better way of doing this */
	#if 0
	/* NOTE: we ignore subnormal numbers for now */
	union { f64 f; u64 u; } u = {.f = f};
	i32 exponent = ((u.u >> 52) & 0x7ff) - 1023;
	f32 log_10_of_2 = 0.301f;
	i32 scale       = (exponent * log_10_of_2);
	/* NOTE: normalize f */
	for (i32 i = ABS(scale); i > 0; i--)
		f *= (scale > 0)? 0.1f : 10.0f;
	#else
	i32 scale = 0;
	if (f != 0) {
		while (f > 1) {
			f *= 0.1f;
			scale++;
		}
		while (f < 1) {
			f *= 10.0f;
			scale--;
		}
	}
	#endif

	i32 prec = 100;
	stream_append_f64(s, f, prec);
	stream_append_byte(s, 'e');
	stream_append_byte(s, scale >= 0? '+' : '-');
	for (i32 i = prec / 10; i > 1; i /= 10)
		stream_append_byte(s, '0');
	stream_append_u64(s, ABS(scale));
}

static void
stream_append_variable(Stream *s, Variable *var)
{
	switch (var->type) {
	case VT_F32: {
		f32 *f32_val = var->store;
		stream_append_f64(s, *f32_val * var->display_scale, 100);
	} break;
	case VT_I32: {
		i32 *i32_val = var->store;
		stream_append_i64(s, *i32_val * var->display_scale);
	} break;
	default: INVALID_CODE_PATH;
	}
}

static s8
cstr_to_s8(char *cstr)
{
	s8 result = {.data = (u8 *)cstr};
	while (*cstr) { result.len++; cstr++; }
	return result;
}

static s8
s8_cut_head(s8 s, size cut)
{
	s8 result = s;
	if (cut > 0) {
		result.data += cut;
		result.len -= cut;
	}
	return result;
}

static s8
s8alloc(Arena *a, size len)
{
	return (s8){ .data = alloc(a, u8, len), .len = len };
}

static s8
push_s8(Arena *a, s8 str)
{
	s8 result = s8alloc(a, str.len);
	mem_copy(str.data, result.data, result.len);
	return result;
}

static b32
uv4_equal(uv4 a, uv4 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static u32
round_down_power_of_2(u32 a)
{
	u32 result = 0x80000000UL >> _lzcnt_u32(a);
	return result;
}

static v3
cross(v3 a, v3 b)
{
	v3 result = {
		.x = a.y * b.z - a.z * b.y,
		.y = a.z * b.x - a.x * b.z,
		.z = a.x * b.y - a.y * b.x,
	};
	return result;
}

static v3
sub_v3(v3 a, v3 b)
{
	v3 result = {
		.x = a.x - b.x,
		.y = a.y - b.y,
		.z = a.z - b.z,
	};
	return result;
}

static f32
length_v3(v3 a)
{
	f32 result = a.x * a.x + a.y * a.y + a.z * a.z;
	return result;
}

static v3
normalize_v3(v3 a)
{
	f32 length = length_v3(a);
	v3 result = {.x = a.x / length, .y = a.y / length, .z = a.z / length};
	return result;
}

static f64
parse_f64(s8 s)
{
	f64 integral = 0, fractional = 0, sign = 1;

	if (s.len && *s.data == '-') {
		sign = -1;
		s.data++;
		s.len--;
	}

	while (s.len && *s.data != '.') {
		integral *= 10;
		integral += *s.data - '0';
		s.data++;
		s.len--;
	}

	if (*s.data == '.') { s.data++; s.len--; }

	while (s.len) {
		ASSERT(s.data[s.len - 1] != '.');
		fractional /= 10;
		fractional += (f64)(s.data[--s.len] - '0') / 10.0;
	}
	f64 result = sign * (integral + fractional);
	return result;
}

static void
fill_kronecker_sub_matrix(__m128i *out, i32 out_stride, i32 scale, __m128i *b, uv2 b_dim)
{
	__m128 vscale = _mm_set1_ps(scale);
	for (u32 i = 0; i < b_dim.y; i++) {
		for (u32 j = 0; j < b_dim.x / 4; j++) {
			__m128 vb = _mm_cvtepi32_ps(_mm_loadu_si128(b++));
			_mm_storeu_si128(out + j, _mm_cvtps_epi32(_mm_mul_ps(vscale, vb)));
		}
		out += out_stride;
	}
}

/* NOTE: this won't check for valid space/etc and assumes row major order */
static void
kronecker_product(i32 *out, i32 *a, uv2 a_dim, i32 *b, uv2 b_dim)
{
	uv2 out_dim = {.x = a_dim.x * b_dim.x, .y = a_dim.y * b_dim.y};
	ASSERT(out_dim.y % 4 == 0);
	for (u32 i = 0; i < a_dim.y; i++) {
		__m128i *vout = (__m128i *)out;
		for (u32 j = 0; j < a_dim.x; j++, a++) {
			fill_kronecker_sub_matrix(vout, out_dim.y / 4, *a, (__m128i *)b, b_dim);
			vout += b_dim.y / 4;
		}
		out += out_dim.y * b_dim.x;
	}
}

/* NOTE/TODO: to support even more hadamard sizes use the Paley construction */
static void
fill_hadamard_transpose(i32 *out, i32 *tmp, u32 dim)
{
	ASSERT(dim);
	b32 power_of_2 = ISPOWEROF2(dim);

	if (!power_of_2) {
		ASSERT(dim % 12 == 0);
		dim /= 12;
	}

	i32 *m;
	if (power_of_2) m = out;
	else            m = tmp;

	#define IND(i, j) ((i) * dim + (j))
	m[0] = 1;
	for (u32 k = 1; k < dim; k *= 2) {
		for (u32 i = 0; i < k; i++) {
			for (u32 j = 0; j < k; j++) {
				i32 val = m[IND(i, j)];
				m[IND(i + k, j)]     =  val;
				m[IND(i, j + k)]     =  val;
				m[IND(i + k, j + k)] = -val;
			}
		}
	}
	#undef IND

	if (!power_of_2)
		kronecker_product(out, tmp, (uv2){.x = dim, .y = dim}, hadamard_12_12_transpose,
		                  (uv2){.x = 12, .y = 12});
}
