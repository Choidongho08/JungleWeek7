#define f 16384

static inline int32_t int_to_fp(int32_t n)
{
    return n * f;
}

static inline int32_t fp_to_int_zero(int32_t x)
{
    return x / f;
}

static inline int32_t fp_to_int_nearest(int32_t x)
{
    return x >= 0 ? (x + f / 2) / f : (x - f / 2) / f;
}

static inline int32_t fp_plus(int32_t x, int32_t y)
{
    return x + y;
}

static inline int32_t fp_sub(int32_t x, int32_t y)
{
    return x - y;
}

static inline int32_t fp_int_plus(int32_t x, int32_t n)
{
    return x + n * f;
}

static inline int32_t fp_int_sub(int32_t x, int32_t n)
{
    return x - n * f;
}

static inline int32_t fp_mul(int32_t x, int32_t y)
{
    return ((int64_t)x ) * y / f;
}

static inline int32_t fp_int_mul(int32_t x, int32_t n)
{
    return x * n;
}

static inline int32_t fp_division(int32_t x, int32_t y)
{
    return ((int64_t)x ) * f / y;
}

static inline int32_t fp_int_division(int32_t x, int32_t n)
{
    return x / n;
}