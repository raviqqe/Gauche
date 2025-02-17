/*
 * number.c - numeric functions
 *
 *   Copyright (c) 2000-2021  Shiro Kawai  <shiro@acm.org>
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/scmconst.h"
#include "gauche/bits.h"
#include "gauche/bits_inline.h"
#include "gauche/priv/bignumP.h"
#include "gauche/priv/builtin-syms.h"
#include "gauche/priv/arith.h"
#include "gauche/priv/bytesP.h"

#include <limits.h>
#include <float.h>
#include <math.h>
#include <ctype.h>

/*================================================================
 * Some macros
 */

#ifdef HAVE_SUNMATH_H
#include "sunmath.h"            /* for isinf() on Solaris */
#endif /* HAVE_SUNMATH_H */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef HAVE_ISINF
/* NB: If we inline this, some version of gcc incorrectly assumes
   the condition would never be satisfied and optimize it away. */
int Scm_IsInf(double x)
{
    volatile double xx = x;     /* suppress gcc to be too clever */
    return ((xx) != 0 && (xx) == (xx)/2.0);
}
#endif

/* Maximum allowable range of exponent in the number litereal.
   For flonums, IEEE double can support [-324..308].  For exact
   numbers we can go further, but it would easily consume huge
   memory.  So I assume it is reasonable to limit its range. */
#define MAX_EXPONENT  325

/* Linux gcc have those, but the declarations aren't included unless
   __USE_ISOC9X is defined.  Just in case. */
#ifdef HAVE_TRUNC
extern double trunc(double);
#endif

#ifdef HAVE_RINT
extern double rint(double);
#define roundeven rint
#else
static double roundeven(double);
#endif

/* Many built-in arithmetic routines come with VM* variants, which
   are used when the resulting flonum is directly returned to the VM.
   Such routines share the body with their variants, with an extra
   flag 'vmp' that indicates whether the call is 'VM' variant or not.
   RETURN_FLONUM and RETURN_FLOBJ macros are used to hide the different
   flonum generation in such routines.
   RETURN_FLONUM takes double arg and returns ScmObj.
   RETURN_FLOBJ takes ScmObj (ScmFlonum*) and returns ScmObj.
*/
#if GAUCHE_FFX
#define RETURN_FLONUM(z)                        \
    do {                                        \
        if (vmp) return Scm_VMReturnFlonum(z);  \
        else     return Scm_MakeFlonum(z);      \
    } while (0)
#else  /*!GAUCHE_FFX*/
#define RETURN_FLONUM(z) return Scm_MakeFlonum(z)
#endif /*!GAUCHE_FFX*/

#define DEFINE_DUAL_API1(a, b, kernel) \
    ScmObj a(ScmObj obj) { return kernel(obj, FALSE); } \
    ScmObj b(ScmObj obj) { return kernel(obj, TRUE); }

#define DEFINE_DUAL_API2(a, b, kernel) \
    ScmObj a(ScmObj obj1, ScmObj obj2) { return kernel(obj1, obj2, FALSE); } \
    ScmObj b(ScmObj obj1, ScmObj obj2) { return kernel(obj1, obj2, TRUE); }

/*================================================================
 * Classes of Numeric Tower
 */

static ScmClass *numeric_cpl[] = {
    SCM_CLASS_STATIC_PTR(Scm_RationalClass),
    SCM_CLASS_STATIC_PTR(Scm_RealClass),
    SCM_CLASS_STATIC_PTR(Scm_ComplexClass),
    SCM_CLASS_STATIC_PTR(Scm_NumberClass),
    SCM_CLASS_STATIC_PTR(Scm_TopClass),
    NULL
};

static void number_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx);

SCM_DEFINE_BUILTIN_CLASS(Scm_NumberClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+4);
SCM_DEFINE_BUILTIN_CLASS(Scm_ComplexClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+3);
SCM_DEFINE_BUILTIN_CLASS(Scm_RealClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+2);
SCM_DEFINE_BUILTIN_CLASS(Scm_RationalClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+1);
SCM_DEFINE_BUILTIN_CLASS(Scm_IntegerClass, number_print, NULL, NULL, NULL,
                         numeric_cpl);

/*=====================================================================
 *  Generic Arithmetic
 */

/* Some arithmetic operations calls the corresponding generic function
 * if the operand is not a number.
 */

/* Fallback Gf */
static ScmObj bad_number_method(ScmObj *args, int nargs, ScmGeneric *gf)
{
    const char *fn = (const char *)SCM_GENERIC_DATA(gf);
    if (nargs == 1) {
        Scm_Error("operation %s is not defined on object %S", fn, args[0]);
    } else if (nargs == 2) {
        Scm_Error("operation %s is not defined between %S and %S",
                  fn, args[0], args[1]);
    } else {
        Scm_Error("generic function for %s is called with args %S",
                  fn, Scm_ArrayToList(args, nargs));
    }
    return SCM_UNDEFINED;
}
static SCM_DEFINE_GENERIC(generic_add, bad_number_method, "+");
static SCM_DEFINE_GENERIC(generic_sub, bad_number_method, "-");
static SCM_DEFINE_GENERIC(generic_mul, bad_number_method, "*");
static SCM_DEFINE_GENERIC(generic_div, bad_number_method, "/");

/*=====================================================================
 * IEEE754 double and Endianness
 */

/* Structure to extract bits from a double.  This info may be provided
 * by a system header (e.g. ieee754.h) but for the portability we
 * define by ourselves.
 */
typedef union {
    double d;
    struct {
#ifdef WORDS_BIGENDIAN
#if SIZEOF_LONG >= 8
        unsigned int sign:1;
        unsigned int exp:11;
        unsigned long mant:52;
#else  /*SIZEOF_LONG < 8*/
        unsigned int sign:1;
        unsigned int exp:11;
        unsigned long mant0:20;
        unsigned long mant1:32;
#endif /*SIZEOF_LONG < 8*/
#else  /*!WORDS_BIGENDIAN*/
#if SIZEOF_LONG >= 8
        unsigned long mant:52;
        unsigned int  exp:11;
        unsigned int  sign:1;
#else  /*SIZEOF_LONG < 8*/
        unsigned long mant1:32;
        unsigned long mant0:20;
        unsigned int  exp:11;
        unsigned int  sign:1;
#endif /*SIZEOF_LONG < 8*/
#endif /*!WORDS_BIGENDIAN*/
    } components;
} ScmIEEEDouble;

#ifdef DOUBLE_ARMENDIAN
/* ARM processor may be configured to use a special mixed endian.
   We check at runtime. */
typedef union {
    double d;
    struct {
        unsigned long mant0:20;
        unsigned int exp:11;
        unsigned int sign:1;
        unsigned long mant1:32;
    } components;
} ScmIEEEDoubleARM;
#endif /*DOUBLE_ARMENDIAN*/

/* ARM special handling */
#ifdef DOUBLE_ARMENDIAN
static int armendian_p = FALSE;

#define TEST_DBL 1.9999999999999998  /* all '1' bits for mantissa */

void check_armendian()
{
    ScmIEEEDouble z;
    z.d = TEST_DBL;
    if (z.components.exp != 1023) {
        ScmIEEEDoubleARM z2;
        z2.d = TEST_DBL;
        if (z2.components.exp != 1023) {
            Scm_Panic("Initiaization failed: Cannot determine double's endian "
                      "on this ARM processor.");
        }
        armendian_p = TRUE;
    } else {
        armendian_p = FALSE;
    }
}
#endif  /*DOUBLE_ARMENDIAN*/

/* This is lazyily initialized, for at the time number.c is initialized,
   not sufficient infrastructure is booted to initialize a parameter.
 */
static ScmPrimitiveParameter *default_endian = NULL;
static ScmInternalMutex default_endian_mutex;

static void init_default_endian()
{
    if (default_endian == NULL) {
        SCM_INTERNAL_MUTEX_LOCK(default_endian_mutex);
        if (default_endian == NULL) {
            default_endian =
                Scm_BindPrimitiveParameter(Scm_GaucheModule(), "default-endian",
                                           SCM_OBJ(Scm_NativeEndian()), 0);

        }
        SCM_INTERNAL_MUTEX_UNLOCK(default_endian_mutex);
    }
}

ScmObj Scm_NativeEndian()
{
#ifdef DOUBLE_ARMENDIAN
    if (armendian_p) return SCM_SYM_ARM_LITTLE_ENDIAN;
#endif /*DOUBLE_ARMENDIAN*/
#if WORDS_BIGENDIAN
    return SCM_SYM_BIG_ENDIAN;
#else
    return SCM_SYM_LITTLE_ENDIAN;
#endif
}

ScmObj Scm_DefaultEndian(void)
{
    init_default_endian();
    return Scm_PrimitiveParameterRef(Scm_VM(), default_endian);
}

void Scm_SetDefaultEndian(ScmObj endian)
{
    /* We trust the caller passes one of symbols big-endian, little-endian
       or arm-little-endian. */
    init_default_endian();
    Scm_PrimitiveParameterSet(Scm_VM(), default_endian, endian);
}

int Scm_IsBE(ScmObj endian)
{
    return SCM_IS_BE(endian);
}

int Scm_IsLE(ScmObj endian)
{
    return SCM_IS_LE(endian);
}

int Scm_IsArmLE(ScmObj endian)
{
    return SCM_IS_ARM_LE(endian);
}


/*=====================================================================
 *  Flonums
 */

#undef COUNT_FLONUM_ALLOC

#ifdef COUNT_FLONUM_ALLOC  /* for benchmarks.  usually this should be off. */
static u_long flonum_count = 0;

static void report_flonum_count(void *data)
{
    fprintf(stderr, "allocated flonums = %8d\n", flonum_count);
}
#endif /*COUNT_FLONUM_ALLOC*/

ScmObj Scm_MakeFlonum(double d)
{
    ScmFlonum *f = SCM_NEW(ScmFlonum);
    SCM_FLONUM_VALUE(f) = d;
#ifdef COUNT_FLONUM_ALLOC
    flonum_count++;
#endif
    return SCM_MAKE_FLONUM_MEM(f);
}

ScmObj Scm_FlonumIntegerToExact(double d) /* d mustn't have fractional part */
{
#if SIZEOF_LONG >= 8
    /* On 64bit machine, double can't exactly represent SCM_SMALL_INT_MIN and
       SCM_SMALL_INT_MAX, and comparing d with them could pass through
       out-of-range value, so we convert d to long first. */
    if (LONG_MIN <= d && d <= LONG_MAX) {
        long n = (long)d;
        if (SCM_SMALL_INT_MIN <= n && n <= SCM_SMALL_INT_MAX) {
            return SCM_MAKE_INT(n);
        }
    }
    /* FALLTHROUGH */
#else
    /* On 32bit machine, double has enough precision to cover small int
       range. */
    if (SCM_SMALL_INT_MIN <= d && d <= SCM_SMALL_INT_MAX) {
        return SCM_MAKE_INT((long)d);
    }
#endif
    return Scm_MakeBignumFromDouble(d);
}

ScmObj Scm_MakeFlonumToNumber(double d, int exact)
{
    if (exact && !SCM_IS_INF(d)) {
        /* see if d can be demoted to integer */
        double i, f;
        f = modf(d, &i);
        if (f == 0.0) return Scm_FlonumIntegerToExact(i);
        /*FALLTHROUGH*/
    }
    return Scm_MakeFlonum(d);
}

/*
 * Flonum decomposition
 */

static inline void decode_double(double d,
                                 u_long *mant1 SCM_UNUSED,
                                 u_long *mant0,
                                 int *exp, int *sign)
{
    ScmIEEEDouble dd;
#ifdef DOUBLE_ARMENDIAN         /* ARM-specific handling */
    ScmIEEEDoubleARM dd2;
    if (armendian_p) {
        dd2.d = d;
        *mant1 = (u_int)dd2.components.mant1;
        *mant0 = (u_int)dd2.components.mant0;
        *exp   = dd2.components.exp;
        *sign  = dd2.components.sign;
        return;
    }
#endif /*DOUBLE_ARMENDIAN*/

    dd.d = d;
#if SIZEOF_LONG >= 8
    *mant0 = dd.components.mant;
    *exp   = dd.components.exp;
    *sign  = dd.components.sign;
#else  /* SIZEOF_LONG == 4 */
    *mant1 = (u_int)dd.components.mant1;
    *mant0 = (u_int)dd.components.mant0;
    *exp   = dd.components.exp;
    *sign  = dd.components.sign;
#endif /* SIZEOF_LONG == 4 */
}

/* Decompose flonum D into an integer mantissa F and exponent E, where
 *   -1074 <= E <= 971
 *    0 <= abs(F) < 2^53
 *    D = F * 2^E
 * Some special cases:
 *    F = 0, E = 0 if D = 0.0 or -0.0
 *    F = #t if D is infinity (positive or negative)
 *    F = #f if D is NaN.
 * If D is normalized number (e.g. E > -1074), F >= 2^52.
 * If D is denormalized number, E == -1074 and F < 2^52
 * Cf. IEEE 754 Reference
 * http://babbage.cs.qc.edu/courses/cs341/IEEE-754references.html
 */
ScmObj Scm_DecodeFlonum(double d, int *exp, int *sign)
{
    ScmObj f;
    u_long mant1, mant0;
    int exp0, sign0;

    decode_double(d, &mant1, &mant0, &exp0, &sign0);

    *sign = (sign0? -1 : 1);

    /* Check exceptional cases */
    if (exp0 == 0x7ff) {
        *exp = 0;
        if (
#if SIZEOF_LONG >= 8
            mant0 == 0
#else  /*SIZEOF_LONG < 8*/
            mant0 == 0 && mant1 == 0
#endif /*SIZEOF_LONG < 8*/
            ) {
            return SCM_TRUE;  /* infinity */
        } else {
            return SCM_FALSE; /* NaN */
        }
    }

    *exp  = (exp0? exp0 - 0x3ff - 52 : -0x3fe - 52);

#if SIZEOF_LONG >= 8
    {
        if (exp0 > 0) mant0 += (1L<<52); /* hidden bit */
        f = Scm_MakeInteger(mant0);
    }
#else  /*SIZEOF_LONG < 8*/
    {
        u_long values[2];
        values[0] = mant1;
        values[1] = mant0;
        if (exp0 > 0) values[1] += (1L<<20); /* hidden bit */
        f = Scm_NormalizeBignum(SCM_BIGNUM(Scm_MakeBignumFromUIArray(1, values, 2)));
    }
#endif /*SIZEOF_LONG < 8*/
    return f;
}

/* returns -1 or 1.  Scm_Sign cannot distinguish 0.0 and -0.0.  This one can.
   signbit(3) is in C99. */
int Scm_FlonumSign(double d)
{
    return signbit(d)? -1 : 1;
}

/* Half float support */

double Scm_HalfToDouble(ScmHalfFloat v)
{
    int e = SCM_HALF_FLOAT_EXPONENT(v);
    int m = SCM_HALF_FLOAT_MANTISSA(v);
    int s = SCM_HALF_FLOAT_SIGN_BIT(v);
    if (e == 31) {              /* special */
        if (m == 0) {
            if (s) return SCM_DBL_NEGATIVE_INFINITY;
            else   return SCM_DBL_POSITIVE_INFINITY;
        } else {
            return SCM_DBL_NAN;
        }
    }
    if (e > 0) {                /* normalized */
        double d = ldexp(1.0 + m/1024.0, e - 15);
        return s? -d : d;
    }
    else {                      /* denormalized */
        double d = ldexp(m/1024.0, -14);
        return s? -d : d;
    }
}

ScmHalfFloat Scm_DoubleToHalf(double v)
{
    u_long mant1, mant0;
    int exp0, sign0;

    decode_double(v, &mant1, &mant0, &exp0, &sign0);

    if (exp0 == 0x7ff) {  /* special */
        if (
#if SIZEOF_LONG >= 8
            mant0 == 0
#else  /*SIZEOF_LONG < 8*/
            mant0 == 0 && mant1 == 0
#endif /*SIZEOF_LONG < 8*/
            ) {
            return sign0? 0xfc00 : 0x7c00;
        } else {
            return 0x7fff;
        }
    }
    int e = exp0 - 1023 + 15;
    if (e >= 31) {              /* overflow */
        return sign0? 0xfc00 : 0x7c00;
    }
    /* Calculate required mantissa bits.  We need upper 10 bits, unless
       e < 0, in which case we get denormalized number. */
    int mbits = 10 + ((e <= 0)? e-1 : 0);
    if (mbits < -1) {           /* underflow (-1 for rounding, see below) */
        return sign0? 0x8000 : 0x0000;
    }
    if (e < 0) e = 0;
    /* Take the mantissa bits.  We take one extra bit to perform
       roudning.  R is used to determine whether lower bits are
       all 0 or not. */
#if SIZEOF_LONG >= 8
    unsigned long m = mant0 >> (52-mbits-1);
    unsigned long r = mant0 & ((1UL << (52-mbits-1)) - 1);
#else  /*SIZEOF_LONG < 8*/
    unsigned long m = mant0 >> (20-mbits-1);
    unsigned long r = (mant0 & ((1UL << (20-mbits-1)) - 1))|mant1;
#endif /*SIZEOF_LONG < 8*/
    m += 1<<(mbits+1);          /* recover hidden bit */

    if (m%2 == 1) {
        if (r == 0) {
            /* half point.  we round to even */
            if (m&2) m += 2;
        } else {
            m += 2;
        }
    }

    /* drop rounding bits */
    m >>= 1;
    if (m >= 0x800) {
        e += 1;
        m >>= 1;
    }
    if (e == 0 && m >= 0x400) {
        e += 1;
        m &= ~0x400;
    }
    if (e >= 31) {              /* overflow by rounding */
        return sign0? 0xfc00 : 0x7c00;
    }
    /* at this point, normalized numbers should get
       0x400 <= m <= 0x7ff, e > 0,  and denormalized numbers should get
       0 <= m <= 0x3ff, e == 0.  So we don't need to treat denormalized
       specially. */
    return (ScmHalfFloat)((sign0? 0x8000 : 0x0000)|(e << 10)|(m & 0x3ff));
}

/* Construct a double directly from the given bit patterns. This is
   an internal procedure; external procedure should use Scm_EncodeFlonum.

   On 64bit architecture, only mant1 is used for mantissa.
   On 32bit architecture, mant1 is for lower 32bits of mantissa, and
   lower 20bits of mant0 is used for higher bits.
 */
double Scm__EncodeDouble(u_long mant1,
                         u_long mant0 SCM_UNUSED,
                         int exp, int signbit)
{
    ScmIEEEDouble dd;
#ifdef DOUBLE_ARMENDIAN
    ScmIEEEDoubleARM dd2;
    if (armendian_p) {
        dd2.components.mant1 = mant1;
        dd2.components.mant0 = mant0;
        dd2.components.exp = exp;
        dd2.components.sign = signbit;
        return dd2.d;
    }
#endif /*DOUBLE_ARMENDIAN*/

    dd.components.exp = exp;
    dd.components.sign = signbit;
#if SIZEOF_LONG >= 8
    dd.components.mant = mant1;
#else  /*SIZEOF_LONG==4*/
    dd.components.mant1 = mant1;
    dd.components.mant0 = mant0;
#endif /*SIZEOF_LONG==4*/
    return dd.d;
}

/* Inverse of Scm_DecodeFlonum.
   Returns the double representation of S * F * 2^E,
   where S = 1 | -1,
         -1074 <= E <= 971
         2^52 <= F < 2^53 (if E > -1074)
         or 0 <= F < 2^52 (if E = -1074)
   NB: If E < -1074, we scale F accordingly until E becomes -1074.
*/
double Scm_EncodeFlonum(ScmObj mant, int exp, int sign)
{
    if (SCM_FALSEP(mant)) return SCM_DBL_NAN;
    if (SCM_TRUEP(mant)) {
        if (sign < 0) return SCM_DBL_NEGATIVE_INFINITY;
        else return SCM_DBL_POSITIVE_INFINITY;
    }

    int signbit = sign < 0? 1 : 0;
    if (exp < -1074) {
        /* scale mantissa */
        int shift = -1074 - exp;
        mant = Scm_Ash(mant, -shift);
        exp = -1074;
    } else if (exp > 971) {
        Scm_Error("flonum exponent out of range: %d", exp);
    }

    uint64_t mant64 = Scm_GetIntegerU64Clamp(mant, SCM_CLAMP_ERROR, NULL);
    if (!Scm_NumLT(mant, SCM_2_53)) {
        Scm_Error("flonum mantissa out of range: %S", mant);
    }
    if (exp == -1074 && Scm_NumLT(mant, SCM_2_52)) {
        exp -= 1;               /* denormalized range */
    } else if (Scm_NumLT(mant, SCM_2_52)) {
        Scm_Error("flonum mantissa out of range: %S", mant);
    }

    int expfield = exp + 0x3ff + 52;
#if SIZEOF_LONG >= 8
    return Scm__EncodeDouble(mant64, 0, expfield, signbit);
#else
    u_long hi = (mant64 >> 32);
    u_long lo = (u_long)(mant64 & ULONG_MAX);
    return Scm__EncodeDouble(lo, hi, expfield, signbit);
#endif
}

/*=====================================================================
 *  Ratnums
 */

/* possibly returns denomalized number */
ScmObj Scm_MakeRatnum(ScmObj numer, ScmObj denom)
{
    if (!SCM_INTEGERP(numer)) {
        Scm_Error("numerator must be an exact integer, but got %S", numer);
    }
    if (!SCM_INTEGERP(denom)) {
        Scm_Error("denominator must be an exact integer, but got %S", denom);
    }
    if (SCM_EXACT_ZERO_P(denom)) {
        Scm_Error("attempt to calculate a division by zero");
    }
    ScmRatnum *r = SCM_NEW(ScmRatnum);
    SCM_SET_CLASS(r, SCM_CLASS_RATIONAL);
    r->numerator = numer;
    r->denominator = denom;
    return SCM_OBJ(r);
}

#define ENSURE_RATNUM(integer) \
    SCM_RATNUM(Scm_MakeRatnum(integer, SCM_MAKE_INT(1)))

ScmObj Scm_MakeRational(ScmObj numer, ScmObj denom)
{
    if (!SCM_INTEGERP(numer)) {
        Scm_Error("numerator must be an exact integer, but got %S", numer);
    }
    if (!SCM_INTEGERP(denom)) {
        Scm_Error("denominator must be an exact integer, but got %S", denom);
    }
    if (SCM_EXACT_ZERO_P(denom)) {
        Scm_Error("attempt to calculate a division by zero");
    }
    if (SCM_EXACT_ONE_P(denom)) return numer;
    if (SCM_EXACT_ZERO_P(numer)) return SCM_MAKE_INT(0);
    else return Scm_ReduceRational(Scm_MakeRatnum(numer, denom));
}

ScmObj Scm_Numerator(ScmObj n)
{
    if (SCM_RATNUMP(n)) return SCM_RATNUM_NUMER(n);
    if (SCM_EXACTP(n)) return n; /* fixnum or bignum */
    if (!SCM_REALP(n)) SCM_TYPE_ERROR(n, "real number");
    return Scm_Inexact(Scm_Numerator(Scm_Exact(n)));
}

ScmObj Scm_Denominator(ScmObj n)
{
    if (SCM_RATNUMP(n))  return SCM_RATNUM_DENOM(n);
    if (SCM_INTEGERP(n)) return SCM_MAKE_INT(1);
    if (!SCM_REALP(n)) SCM_TYPE_ERROR(n, "real number");
    return Scm_Inexact(Scm_Denominator(Scm_Exact(n)));
}

ScmObj Scm_ReduceRational(ScmObj rational)
{
    int negated = FALSE;

    if (SCM_INTEGERP(rational)) return rational;
    if (!SCM_RATNUMP(rational)) {
        Scm_Error("exact rational number required, but got %S", rational);
    }
    ScmObj numer = SCM_RATNUM_NUMER(rational);
    ScmObj denom = SCM_RATNUM_DENOM(rational);

    if (Scm_Sign(denom) < 0) {
        numer = Scm_Negate(numer);
        denom = Scm_Negate(denom);
        negated = TRUE;
    }

    /* special cases */
    if (SCM_EXACT_ONE_P(denom)) return numer;
    if (SCM_EXACT_ZERO_P(denom)) {
        int s = Scm_Sign(numer);
        if (s > 0) return SCM_POSITIVE_INFINITY;
        if (s < 0) return SCM_NEGATIVE_INFINITY;
        return SCM_NAN;
    }

    ScmObj common = Scm_Gcd(numer, denom);
    if (SCM_EXACT_ONE_P(common)) {
        if (negated) {
            return Scm_MakeRatnum(numer, denom);
        } else {
            return rational;
        }
    } else {
        numer = Scm_Quotient(numer, common, NULL);
        denom = Scm_Quotient(denom, common, NULL);
        if (SCM_EQ(denom, SCM_MAKE_INT(1))) {
            return numer;
        } else {
            return Scm_MakeRatnum(numer, denom);
        }
    }
}

/* x, y must be exact numbers */
ScmObj Scm_RatnumAddSub(ScmObj x, ScmObj y, int subtract)
{
    ScmObj nx = SCM_RATNUMP(x)? SCM_RATNUM_NUMER(x) : x;
    ScmObj dx = SCM_RATNUMP(x)? SCM_RATNUM_DENOM(x) : SCM_MAKE_INT(1);
    ScmObj ny = SCM_RATNUMP(y)? SCM_RATNUM_NUMER(y) : y;
    ScmObj dy = SCM_RATNUMP(y)? SCM_RATNUM_DENOM(y) : SCM_MAKE_INT(1);
    ScmObj gcd, dr, nr;

    /* shortcut */
    if (Scm_NumEq(dx, dy)) {
        dr = dx;
        goto finish;
    }

    if (SCM_EXACT_ONE_P(dx)||SCM_EXACT_ONE_P(dx)) gcd = SCM_MAKE_INT(1);
    else gcd = Scm_Gcd(dx, dy);
    if (Scm_NumEq(dx, gcd)) {
        /* only factor x */
        nx = Scm_Mul(Scm_Quotient(dy, dx, NULL), nx);
        dr = dy;
        goto finish;
    }
    if (Scm_NumEq(dy, gcd)) {
        /* only factor y */
        ny = Scm_Mul(Scm_Quotient(dx, dy, NULL), ny);
        dr = dx;
        goto finish;
    }

    /* general case */
    ScmObj fx = Scm_Quotient(dx, gcd, NULL);
    ScmObj fy = Scm_Quotient(dy, gcd, NULL);
    nx = Scm_Mul(nx, fy);
    ny = Scm_Mul(ny, fx);
    dr = Scm_Mul(dx, fy);
  finish:
    nr = (subtract? Scm_Sub(nx, ny) : Scm_Add(nx, ny));
    return Scm_MakeRational(nr, dr);
}

ScmObj Scm_RatnumMulDiv(ScmObj x, ScmObj y, int divide)
{
    ScmObj nx = SCM_RATNUMP(x)? SCM_RATNUM_NUMER(x) : x;
    ScmObj dx = SCM_RATNUMP(x)? SCM_RATNUM_DENOM(x) : SCM_MAKE_INT(1);
    ScmObj ny = SCM_RATNUMP(y)? SCM_RATNUM_NUMER(y) : y;
    ScmObj dy = SCM_RATNUMP(y)? SCM_RATNUM_DENOM(y) : SCM_MAKE_INT(1);

    if (divide) {
        ScmObj t = ny; ny = dy; dy = t;
    }
    return Scm_MakeRational(Scm_Mul(nx, ny),
                            Scm_Mul(dx, dy));
}

#define Scm_RatnumAdd(x, y)  Scm_RatnumAddSub(x, y, FALSE)
#define Scm_RatnumSub(x, y)  Scm_RatnumAddSub(x, y, TRUE)
#define Scm_RatnumMul(x, y)  Scm_RatnumMulDiv(x, y, FALSE)
#define Scm_RatnumDiv(x, y)  Scm_RatnumMulDiv(x, y, TRUE)


/*=======================================================================
 *  Compnums
 */

ScmObj Scm_MakeCompnum(double r, double i)
{
    ScmCompnum *c = SCM_NEW_ATOMIC(ScmCompnum);
    SCM_SET_CLASS(c, SCM_CLASS_COMPLEX);
    c->real = r;
    c->imag = i;
    return SCM_OBJ(c);
}

ScmObj Scm_MakeComplex(double r, double i)
{
    if (i == 0.0) return Scm_MakeFlonum(r);
    else          return Scm_MakeCompnum(r, i);
}

ScmObj Scm_MakeComplexPolar(double mag, double angle)
{
    double real = mag * cos(angle);
    double imag = mag * sin(angle);
    if (imag == 0.0) return Scm_MakeFlonum(real);
    else             return Scm_MakeCompnum(real, imag);
}

ScmObj Scm_MakeComplexPolarPI(double mag, double pi_angle)
{
    double real = mag * Scm_CosPi(pi_angle);
    double imag = mag * Scm_SinPi(pi_angle);
    if (imag == 0.0) return Scm_MakeFlonum(real);
    else             return Scm_MakeCompnum(real, imag);
}

/* NB: This isn't called by Scheme's real-part; see libnum.scm */
double Scm_RealPart(ScmObj z)
{
    if (SCM_REALP(z)) {
        return Scm_GetDouble(z);
    }
    if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        return 0.0;                /* dummy */
    }
    return SCM_COMPNUM_REAL(z);
}

/* NB: This isn't called by Scheme's imag-part; see libnum.scm */
double Scm_ImagPart(ScmObj z)
{
    if (SCM_COMPNUMP(z)) {
        return SCM_COMPNUM_IMAG(z);
    }
    if (!SCM_REALP(z)) {
        Scm_Error("number required, but got %S", z);
    }
    return 0.0;
}

ScmHalfComplex Scm_GetHalfComplex(ScmObj z)
{
    ScmHalfComplex c = {0, 0};
    if (SCM_COMPNUMP(z)) {
        c.r = Scm_DoubleToHalf(SCM_COMPNUM_REAL(z));
        c.i = Scm_DoubleToHalf(SCM_COMPNUM_IMAG(z));
    } else if (SCM_REALP(z)) {
        c.r = Scm_DoubleToHalf(Scm_GetDouble(z));
        c.i = 0;
    } else {
        Scm_Error("number required, but got %S", z);
    }
    return c;
}

ScmFloatComplex Scm_GetFloatComplex(ScmObj z)
{
    ScmFloatComplex c = 0.0f;
    if (SCM_COMPNUMP(z)) {
        c = (float)SCM_COMPNUM_REAL(z)
            + (float)SCM_COMPNUM_IMAG(z) * _Complex_I;
    } else if (SCM_REALP(z)) {
        c = (float)Scm_GetDouble(z);
    } else {
        Scm_Error("number required, but got %S", z);
    }
    return c;
}

ScmDoubleComplex Scm_GetDoubleComplex(ScmObj z)
{
    ScmDoubleComplex c = 0.0f;
    if (SCM_COMPNUMP(z)) {
        c = SCM_COMPNUM_REAL(z)
            + SCM_COMPNUM_IMAG(z) * _Complex_I;
    } else if (SCM_REALP(z)) {
        c = Scm_GetDouble(z);
    } else {
        Scm_Error("number required, but got %S", z);
    }
    return c;
}

ScmObj Scm_HalfComplexToComplex(ScmHalfComplex z)
{
    return Scm_MakeComplex(Scm_HalfToDouble(z.r),
                           Scm_HalfToDouble(z.i));
}

ScmObj Scm_FloatComplexToComplex(ScmFloatComplex z)
{
    return Scm_MakeComplex((double)crealf(z), (double)cimagf(z));
}

ScmObj Scm_DoubleComplexToComplex(ScmDoubleComplex z)
{
    return Scm_MakeComplex(creal(z), cimag(z));
}


/* NB: This isn't called by Scheme's magnitude; see libnum.scm */
double Scm_Magnitude(ScmObj z)
{
    if (SCM_REALP(z)) {
        return fabs(Scm_GetDouble(z));
    }
    if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        return 0.0;                /* dummy */
    }
    double r = SCM_COMPNUM_REAL(z);
    double i = SCM_COMPNUM_IMAG(z);
    return sqrt(r*r+i*i);
}

double Scm_Angle(ScmObj z)
{
    if (SCM_REALP(z)) {
        return (Scm_Sign(z) < 0)? M_PI : 0.0;
    }
    if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        return 0.0;                /* dummy */
    }
    double r = SCM_COMPNUM_REAL(z);
    double i = SCM_COMPNUM_IMAG(z);
    return atan2(i, r);
}

/*=======================================================================
 *  Coertion
 */

ScmObj Scm_MakeInteger(long i)
{
    if (i >= SCM_SMALL_INT_MIN && i <= SCM_SMALL_INT_MAX) {
        return SCM_MAKE_INT(i);
    } else {
        return Scm_MakeBignumFromSI(i);
    }
}

ScmObj Scm_MakeIntegerU(u_long i)
{
    if (i <= (u_long)SCM_SMALL_INT_MAX) return SCM_MAKE_INT(i);
    else return Scm_MakeBignumFromUI(i);
}

ScmObj Scm_MakeIntegerFromUIArray(int sign,
                                  const u_long *values,
                                  int size)
{
    ScmBignum *b = SCM_BIGNUM(Scm_MakeBignumFromUIArray(sign, values, size));
    return Scm_NormalizeBignum(b);
}

static void range_error(ScmObj obj, int clamp, int *oor)
{
    if (clamp != SCM_CLAMP_ERROR && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
}

/* Convert scheme integer to C integer */
long Scm_GetIntegerClamp(ScmObj obj, int clamp, int *oor)
{
    double v = 0.0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) return SCM_INT_VALUE(obj);
    else if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToSI(SCM_BIGNUM(obj), clamp, oor);
    }
    else if (SCM_FLONUMP(obj)) {
        v = SCM_FLONUM_VALUE(obj);
        goto flonum;
    }
    else if (SCM_RATNUMP(obj)) {
        v = Scm_GetDouble(obj);
        goto flonum;
    }
    else {
        goto err;
    }
  flonum:
    if (v > (double)LONG_MAX) {
        if (clamp & SCM_CLAMP_HI) return LONG_MAX;
        else goto err;
    }
    if (v < (double)LONG_MIN) {
        if (clamp & SCM_CLAMP_LO) return LONG_MIN;
        else goto err;
    }
    return (long)v;
  err:
    range_error(obj, clamp, oor);
    return 0;
}

u_long Scm_GetIntegerUClamp(ScmObj obj, int clamp, int *oor)
{
    double v = 0.0;

    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        if (SCM_INT_VALUE(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            else goto err;
        }
        return SCM_INT_VALUE(obj);
    }
    else if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToUI(SCM_BIGNUM(obj), clamp, oor);
    }
    else if (SCM_FLONUMP(obj)) {
        v = SCM_FLONUM_VALUE(obj);
        goto flonum;
    }
    else if (SCM_RATNUMP(obj)) {
        v = Scm_GetDouble(obj);
        goto flonum;
    }
    else {
        goto err;
    }
  flonum:
    if (v > (double)ULONG_MAX) {
        if (clamp & SCM_CLAMP_HI) return ULONG_MAX;
        else goto err;
    }
    if (v < 0.0) {
        if (clamp & SCM_CLAMP_LO) return 0;
        else goto err;
    }
    return (u_long)v;
  err:
    range_error(obj, clamp, oor);
    return 0;
}

/* 8- and 16-bit integer extraction with range check */
#define SMALL_INT_XTRACT(name, upper, lower)                    \
name(ScmObj obj, int clamp, int *oor)                           \
{                                                               \
    long n = 0;                                                 \
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;   \
    if (SCM_INTP(obj)) {                                        \
        n = SCM_INT_VALUE(obj);                                 \
    } else if (SCM_FLONUMP(obj)) {                              \
        n = (long)SCM_FLONUM_VALUE(obj);                        \
    } else if (SCM_RATNUMP(obj)) {                              \
        n = (long)Scm_GetDouble(obj);                           \
    } else if (SCM_BIGNUMP(obj)) {                              \
        if (Scm_Sign(obj) > 0) {                                \
            if (clamp & SCM_CLAMP_HI) return upper;             \
            else goto err;                                      \
        } else {                                                \
            if (clamp & SCM_CLAMP_LO) return lower;             \
            else goto err;                                      \
        }                                                       \
    } else {                                                    \
        goto err;                                               \
    }                                                           \
    if (n > upper) {                                            \
        if (clamp & SCM_CLAMP_HI) return upper;                 \
        else goto err;                                          \
    }                                                           \
    if (n < lower) {                                            \
        if (clamp & SCM_CLAMP_LO) return lower;                 \
        else goto err;                                          \
    }                                                           \
    return n;                                                   \
  err:                                                          \
    range_error(obj, clamp, oor);                               \
    return 0;                                                   \
}

SMALL_INT_XTRACT(int   Scm_GetInteger8Clamp, 127, -128)
SMALL_INT_XTRACT(u_int Scm_GetIntegerU8Clamp, 255, 0)
SMALL_INT_XTRACT(int   Scm_GetInteger16Clamp, 32767, -32768)
SMALL_INT_XTRACT(u_int Scm_GetIntegerU16Clamp, 65535, 0)


/* 32bit integer specific */
int32_t Scm_GetInteger32Clamp(ScmObj obj, int clamp, int *oor)
{
#if SIZEOF_LONG == 4
    return (int32_t)Scm_GetIntegerClamp(obj, clamp, oor);
#else  /* SIZEOF_LONG >= 8 */

    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    /* NB: we denote the constant directly here.  (1L<<31) fails on
       Alpha machines, since the compiler somehow calculates the constant
       in 32bit integer even it has 'L'.  We have to write (1LL<<31), but
       I'm afraid that it's not portable. */
    if (SCM_INTP(obj)) {
        long r = SCM_INT_VALUE(obj);
        if (r < -0x80000000L) {
            if (clamp & SCM_CLAMP_LO) return -0x80000000L;
            goto err;
        }
        if (r > 0x7fffffffL) {
            if (clamp & SCM_CLAMP_HI) return 0x7fffffffL;
            goto err;
        }
        return r;
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return -0x80000000L;
            goto err;
        } else {
            if (clamp & SCM_CLAMP_HI) return 0x7fffffffL;
            goto err;
        }
    }
    /*TODO: flonum and ratnum! */
  err:
    range_error(obj, clamp, oor);
    return 0;
#endif /* SIZEOF_LONG >= 8 */
}

uint32_t Scm_GetIntegerU32Clamp(ScmObj obj, int clamp, int *oor)
{
#if SIZEOF_LONG == 4
    return (uint32_t)Scm_GetIntegerUClamp(obj, clamp, oor);
#else  /* SIZEOF_LONG >= 8 */
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long r = SCM_INT_VALUE(obj);
        if (r < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            goto err;
        }
        if (r > 0xffffffffL) {
            if (clamp & SCM_CLAMP_HI) return 0xffffffffUL;
            goto err;
        }
        return r;
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            goto err;
        } else {
            if (clamp & SCM_CLAMP_HI) return 0xffffffffUL;
            goto err;
        }
    }
  err:
    range_error(obj, clamp, oor);
    return 0;
#endif /* SIZEOF_LONG >= 8 */
}

/* get an unsigned integer value modulo u_long range.
   convenient when you only concern lower bits. */
u_long Scm_GetIntegerUMod(ScmObj obj)
{
    if (SCM_INTP(obj)) return (u_long)SCM_INT_VALUE(obj);
    if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIZE(obj) == 0) {
            /* this shouldn't happen in normalized bignums, but just in case */
            return 0;
        }
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            u_long v = SCM_BIGNUM(obj)->values[0];
            return ~v + 1;
        } else {
            return SCM_BIGNUM(obj)->values[0];
        }
    }
    Scm_Error("Exact integer required, but got %S", obj);
    return 0;                   /* dummy */
}

#if SIZEOF_LONG == 4
/* we need special routines */
ScmObj Scm_MakeInteger64(int64_t i)
{
    u_long val[2];
    val[0] = (u_long)((uint64_t)i & ULONG_MAX);
    val[1] = (u_long)((uint64_t)i >> 32);
    if (val[1] == 0 && val[0] <= LONG_MAX) return Scm_MakeInteger(val[0]);
    return Scm_NormalizeBignum(SCM_BIGNUM(Scm_MakeBignumFromUIArray(0, val, 2)));
}

ScmObj Scm_MakeIntegerU64(uint64_t i)
{
    u_long val[2];
    val[0] = (u_long)((uint64_t)i & ULONG_MAX);
    val[1] = (u_long)((uint64_t)i >> 32);
    if (val[1] == 0) return Scm_MakeIntegerU(val[0]);
    return Scm_MakeBignumFromUIArray(1, val, 2);
}

int64_t Scm_GetInteger64Clamp(ScmObj obj, int clamp, int *oor)
{
    int64_t r = 0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) return (int64_t)SCM_INT_VALUE(obj);
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToSI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_RATNUMP(obj)) {
        obj = Scm_Inexact(obj);
        /* FALLTHROUGH */
    }
    if (SCM_FLONUMP(obj)) {
        int64_t maxval, minval;
        double v;

        maxval = INT64_MAX;
        minval = INT64_MIN;
        v = SCM_FLONUM_VALUE(obj);
        if (v > (double)maxval) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            return maxval;
        } else if (v < (double)minval) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return minval;
        } else {
            return (int64_t)v;
        }
    }
  err:
    range_error(obj, clamp, oor);
    return r;
}

uint64_t Scm_GetIntegerU64Clamp(ScmObj obj, int clamp, int *oor)
{
    uint64_t r = 0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return 0;
        } else {
            return (uint64_t)v;
        }
    }
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToUI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_RATNUMP(obj)) {
        obj = Scm_Inexact(obj);
        /* FALLTHROUGH */
    }
    if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        uint64_t maxval;

        if (v < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return 0;
        }
        maxval = UINT64_MAX;
        if (v > (double)maxval) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            return maxval;
        } else {
            return (uint64_t)v;
        }
    }
  err:
    range_error(obj, clamp, oor);
    return r;
}

#endif /* SIZEOF_LONG == 4 */

#if GAUCHE_API_VERSION < 98
/* Since we adopt C99, we can simply use cast instead of the funcitons.
   These are kept for ABI compatibility until 1.0 release */
int64_t Scm_DoubleToInt64(double v)
{
    return (int64_t)v;
}

uint64_t Scm_DoubleToUInt64(double v)
{
    return (uint64_t)v;
}

double Scm_Int64ToDouble(int64_t v)
{
    return (double)v;
}

double Scm_UInt64ToDouble(uint64_t v)
{
    return (double)v;
}
#endif /*GAUCHE_API_VERSION < 98*/

/* See if a Scheme integer si can be representable with 53bit mantissa.
   Also store the highest and lowest bit posisions in *hi and *lo
   (if they're not NULL) */
static int double_precision(ScmObj si, int *phi, int *plo)
{
    if (SCM_INTP(si)) {
        ScmSmallInt i = SCM_INT_VALUE(si);
        if (i < 0) i = -i;      /* won't overflow */
        ScmBits bi = (ScmBits)i;
        int hi = Scm_BitsHighest1(&bi, 0, SCM_WORD_BITS-1);
        int lo = Scm_BitsLowest1(&bi, 0, SCM_WORD_BITS-1);
        if (phi) *phi = hi;
        if (plo) *plo = lo;
        return (hi - lo) < 53;
    } else {
        SCM_ASSERT(SCM_BIGNUMP(si));
        const ScmBits *bits = (ScmBits*)SCM_BIGNUM(si)->values;
        int hi = Scm_BitsHighest1(bits, 0, SCM_BIGNUM_SIZE(si)*SCM_WORD_BITS-1);
        int lo = Scm_BitsLowest1(bits, 0, SCM_BIGNUM_SIZE(si)*SCM_WORD_BITS-1);
        if (phi) *phi = hi;
        if (plo) *plo = lo;
        return (hi - lo) < 53;
    }
}

/* Get n-th bit of absolute value of Scheme integer si */
static int abs_bittest(ScmObj si, int bit)
{
    if (SCM_INTP(si)) {
        ScmSmallInt i = SCM_INT_VALUE(si);
        if (i < 0) i = -i;
        return i & (1L<<bit);
    } else {
        SCM_ASSERT(SCM_BIGNUMP(si));
        ScmBits *bits = (ScmBits*)SCM_BIGNUM(si)->values;
        return SCM_BITS_TEST(bits, bit);
    }
}

double Scm_GetDouble(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) return SCM_FLONUM_VALUE(obj);
    else if (SCM_INTP(obj)) return (double)SCM_INT_VALUE(obj);
    else if (SCM_BIGNUMP(obj)) return Scm_BignumToDouble(SCM_BIGNUM(obj));
    else if (SCM_RATNUMP(obj)) {
        /* This is more subtle than it appears.  A naive approach is to
           convert numerator and denominator to double, and do flonum division.
           However,
           - Denominator and/or numerator may exceed FLT_MAX, yielding
             infinities in the intermediate results.  However, their ratio
             can be in a valid flonum range.
           - If denominator and/or numerator requires more than 53bit precision,
             converting each to double causes rounding, which occurs before
             the final division, causing ULP-off error.
             E.g.  (inexact (/ (+ 1 (* (exact (flonum-epsilon)) 33/100)) 1))
             should be 1, but would yield 1.0000000000000002 if the numerator
             is rounded up first.
        */
        ScmObj numer = SCM_RATNUM_NUMER(obj);
        ScmObj denom = SCM_RATNUM_DENOM(obj);

        int n_hi, n_lo, d_hi, d_lo;
        int n_dp = double_precision(numer, &n_hi, &n_lo);
        int d_dp = double_precision(denom, &d_hi, &d_lo);

        if (!(n_dp && d_dp)) goto fullpath;

        double dnumer = Scm_GetDouble(numer);
        double ddenom = Scm_GetDouble(denom);

        if (!SCM_IS_INF(dnumer) && !SCM_IS_INF(ddenom)) {
            /* short path */
            volatile double result;
            SCM_FP_ENSURE_DOUBLE_PRECISION_BEGIN();
            /* It is critical to perform this division in IEEE double (53bit
               mantissa) precision, _not_ in x87 extended double precision;
               if the latter were used, double-rounding would yield different
               results, which makes inexact -> exact -> inexact round-trip
               fail.  For example, (inexact 3002399751580332/3002399751580331)
               should be 1.0000000000000002 (1LSB greater than 1.0), but
               extended double precision division yields
               1.0000000000000004 (2LSB greater than 1.0).
            */
            result = dnumer/ddenom;
            SCM_FP_ENSURE_DOUBLE_PRECISION_END();
            return result;
        }
    fullpath:;
        /* Need more precise but expensive calculation.
           We find K such that 2^K * numer >= 2^54 * denom, so that
           the integer division yiels more than 53bit integral part.
         */
        int shift = 0;
        if (n_hi - d_hi < 54) {
            shift = 54 - (n_hi - d_hi);
            numer = Scm_Ash(numer, shift);
        }

        ScmObj rem;
        ScmObj quo = Scm_Quotient(numer, denom, &rem);

        /* If shift > 1076, the result would be denomarlized range.
           Less than 53bits of quo is used, so we have to mask out the
           unncessary digit.  (Otherwise, it causes double-rounding.) */
        if (shift > 1076) {
            ScmObj mask =
                Scm_LogNot(Scm_Sub(Scm_Ash(SCM_MAKE_INT(1), shift-1076-1),
                                   SCM_MAKE_INT(1)));
            if (Scm_Sign(quo) < 0) {
                quo = Scm_Negate(Scm_LogAnd(Scm_Negate(quo), mask));
            } else {
                quo = Scm_LogAnd(quo, mask);
            }
        }

        int q_hi, q_lo;
        if (double_precision(quo, &q_hi, &q_lo)) {
            /* Result fits in double precision. */
            double dquo = Scm_GetDouble(quo);
            return ldexp(dquo, -shift);
        }
        /* We have to look at the 54th bit and below to decide rounding.
           If 54-th bit is 0, we can safely convert it to double, truncating
           the lower bits. */
        if (!abs_bittest(quo, q_hi - 53)) {
            double dquo = Scm_GetDouble(quo);
            return ldexp(dquo, -shift);
        }
        /* If 54-th bit is 1, we see the lower bits.  If we have at least
           one '1' bit, or remainder isn't zero, we can round up. */
        int roundup = FALSE;
        ScmObj mask = Scm_Ash(SCM_MAKE_INT(1), q_hi - 53);
        if (rem != SCM_MAKE_INT(0)) roundup = TRUE;
        else {
            ScmObj mask_1 = Scm_Sub(mask, SCM_MAKE_INT(1));
            ScmObj q = quo;
            if (Scm_Sign(q) < 0) {
                q = Scm_Negate(q);
            }
            if (Scm_LogAnd(q, mask_1) != SCM_MAKE_INT(0)) roundup = TRUE;
        }
        if (roundup) {
            if (Scm_Sign(quo) < 0) {
                quo = Scm_Sub(quo, mask);
            } else {
                quo = Scm_Add(quo, mask);
            }
            double dquo = Scm_GetDouble(quo);
            return ldexp(dquo, -shift);
        }
        /* We are half-point.  See the 53-bit and round to even.  */
        if (!abs_bittest(quo, q_hi - 52)) {
            if (Scm_Sign(quo) < 0) {
                quo = Scm_Add(quo, mask);
            } else {
                quo = Scm_Sub(quo, mask);
            }
        }
        double dquo = Scm_GetDouble(quo);
        return ldexp(dquo, -shift);
    }
    else return 0.0;
}

/*
 *   Generic Methods
 */

/* Predicates */

int Scm_IntegerP(ScmObj obj)
{
    if (SCM_INTP(obj) || SCM_BIGNUMP(obj)) return TRUE;
    if (SCM_RATNUMP(obj)) return FALSE; /* normalized ratnum never be integer */
    if (SCM_FLONUMP(obj)) {
        double d = SCM_FLONUM_VALUE(obj);
        if (SCM_IS_INF(d) || SCM_IS_NAN(d)) return FALSE;
        double i;
        double f = modf(d, &i);
        if (f == 0.0) return TRUE;
        return FALSE;
    }
    if (SCM_COMPNUMP(obj)) return FALSE;
    Scm_Error("number required, but got %S", obj);
    return FALSE;           /* dummy */
}

int Scm_OddP(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        return (SCM_INT_VALUE(obj)&1);
    }
    if (SCM_BIGNUMP(obj)) {
        return (SCM_BIGNUM(obj)->values[0] & 1);
    }
    if (SCM_FLONUMP(obj) && Scm_IntegerP(obj)) {
        return (fmod(SCM_FLONUM_VALUE(obj), 2.0) != 0.0);
    }
    Scm_Error("integer required, but got %S", obj);
    return FALSE;       /* dummy */

}

int Scm_FiniteP(ScmObj obj)
{
    return !Scm_InfiniteP(obj) && !Scm_NanP(obj);
}

int Scm_InfiniteP(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        return SCM_IS_INF(v);
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj);
        double i = SCM_COMPNUM_IMAG(obj);
        return SCM_IS_INF(r) || SCM_IS_INF(i);
    } else if (!SCM_NUMBERP(obj)) {
        SCM_TYPE_ERROR(obj, "number");
    }
    return FALSE;
}

int Scm_NanP(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        return SCM_IS_NAN(v);
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj);
        double i = SCM_COMPNUM_IMAG(obj);
        return SCM_IS_NAN(r) || SCM_IS_NAN(i);
    } else if (!SCM_NUMBERP(obj)) {
        SCM_TYPE_ERROR(obj, "number");
    }
    return FALSE;
}

/* Unary Operator */

static ScmObj scm_abs(ScmObj obj, int vmp)
{
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v < 0) {
            if (v == SCM_SMALL_INT_MIN) {
                obj = Scm_MakeBignumFromSI(-v);
            } else {
                obj = SCM_MAKE_INT(-v);
            }
        }
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            obj = Scm_BignumCopy(SCM_BIGNUM(obj));
            SCM_BIGNUM_SIGN(obj) = 1;
        }
    } else if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        if (signbit(v)) RETURN_FLONUM(-v);
    } else if (SCM_RATNUMP(obj)) {
        if (Scm_Sign(SCM_RATNUM_NUMER(obj)) < 0) {
            obj = Scm_MakeRational(Scm_Negate(SCM_RATNUM_NUMER(obj)),
                                   SCM_RATNUM_DENOM(obj));
        }
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj);
        double i = SCM_COMPNUM_IMAG(obj);
        double a = sqrt(r*r+i*i);
        RETURN_FLONUM(a);
    } else {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}
DEFINE_DUAL_API1(Scm_Abs, Scm_VMAbs, scm_abs)

/* Return -1, 0 or 1 when arg is minus, zero or plus, respectively.
   used to implement zero?, positive? and negative?
   NB: This returns 0 for both positive and negative zeros. */
int Scm_Sign(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        long r = SCM_INT_VALUE(obj);
        if (r == 0) return 0;
        return (r > 0)? 1 : -1;
    }
    if (SCM_BIGNUMP(obj)) {
        return SCM_BIGNUM_SIGN(obj);
    }
    if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        if (v == 0.0) return 0;
        return (v > 0.0)? 1 : -1;
    }
    if (SCM_RATNUMP(obj)) {
        return Scm_Sign(SCM_RATNUM_NUMER(obj));
    }
    /* NB: zero? can accept a complex number, but it is processed in
       the stub function.   see libnum.scm */
    Scm_Error("real number required, but got %S", obj);
    return 0; /* dummy */
}

static ScmObj negate(ScmObj obj, int vmp)
{
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v == SCM_SMALL_INT_MIN) {
            return Scm_MakeBignumFromSI(-v);
        } else {
            return SCM_MAKE_INT(-v);
        }
    } else if (SCM_BIGNUMP(obj)) {
        return Scm_BignumNegate(SCM_BIGNUM(obj));
    } else if (SCM_FLONUMP(obj)) {
        double r = -SCM_FLONUM_VALUE(obj);
        RETURN_FLONUM(r);
    } else if (SCM_RATNUMP(obj)) {
        return Scm_MakeRational(Scm_Negate(SCM_RATNUM_NUMER(obj)),
                                SCM_RATNUM_DENOM(obj));
    } else if (SCM_COMPNUMP(obj)) {
        return Scm_MakeCompnum(-SCM_COMPNUM_REAL(obj),
                               -SCM_COMPNUM_IMAG(obj));
    } else {
        return Scm_ApplyRec(SCM_OBJ(&generic_sub), SCM_LIST1(obj));
    }
}
DEFINE_DUAL_API1(Scm_Negate, Scm_VMNegate, negate)


static ScmObj reciprocal(ScmObj obj, int vmp)
{
    if (SCM_INTP(obj) || SCM_BIGNUMP(obj)) {
        return Scm_MakeRational(SCM_MAKE_INT(1), obj);
    } else if (SCM_FLONUMP(obj)) {
        double val = 1.0/SCM_FLONUM_VALUE(obj);
        RETURN_FLONUM(val);
    } else if (SCM_RATNUMP(obj)) {
        return Scm_MakeRational(SCM_RATNUM_DENOM(obj),
                                SCM_RATNUM_NUMER(obj));
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj);
        double i = SCM_COMPNUM_IMAG(obj);
        double d = r*r + i*i;
        double r1 = r/d;
        double i1 = -i/d;
        return Scm_MakeComplex(r1, i1);
    } else {
        return Scm_ApplyRec(SCM_OBJ(&generic_div), SCM_LIST1(obj));
    }
}
DEFINE_DUAL_API1(Scm_Reciprocal, Scm_VMReciprocal, reciprocal)


static ScmObj ireciprocal(ScmObj obj, int vmp)
{
    if (SCM_EXACT_ZERO_P(obj)) return SCM_POSITIVE_INFINITY;
    if (SCM_EXACT_ONE_P(obj))  return obj;
    if (SCM_REALP(obj)) {
        double z = 1.0/Scm_GetDouble(obj);
        RETURN_FLONUM(z);
    }
    // delegate the rest to exact reciprocal
    return reciprocal(obj, vmp);
}
DEFINE_DUAL_API1(Scm_ReciprocalInexact, Scm_VMReciprocalInexact, ireciprocal)


/*
 * Conversion operators
 */

static ScmObj inexact(ScmObj obj, int vmp)
{
    if (SCM_INTP(obj)) {
        double z = (double)SCM_INT_VALUE(obj);
        RETURN_FLONUM(z);
    } else if (SCM_BIGNUMP(obj)) {
        double z = Scm_BignumToDouble(SCM_BIGNUM(obj));
        RETURN_FLONUM(z);
    } else if (SCM_RATNUMP(obj)) {
        RETURN_FLONUM(Scm_GetDouble(obj));
    } else if (!SCM_FLONUMP(obj) && !SCM_COMPNUMP(obj)) {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}
DEFINE_DUAL_API1(Scm_Inexact, Scm_VMInexact, inexact)


ScmObj Scm_Exact(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) {
        double d = SCM_FLONUM_VALUE(obj);
        double f, i;
        if (SCM_IS_NAN(d) || SCM_IS_INF(d)) {
            Scm_Error("Exact infinity/nan is not supported: %S", obj);
        }
        if ((f = modf(d, &i)) == 0.0) {
            obj = Scm_FlonumIntegerToExact(i);
        } else {
            /* We'd find out the simplest rational numebr within the precision
               of IEEE double floating point number.  The actual code is in
               lib/gauche/numerical.scm. */
            static ScmObj real_to_rational = SCM_UNDEFINED;
            SCM_BIND_PROC(real_to_rational, "real->rational",
                          Scm_GaucheModule());
            obj = Scm_ApplyRec1(real_to_rational, obj);
        }
    } else if (SCM_COMPNUMP(obj)) {
        Scm_Error("exact complex is not supported: %S", obj);
    } if (!SCM_EXACTP(obj)) {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}

/*===============================================================
 * Arithmetics
 */

/* NB: we used to support n-ary operations in C API, expecting
   them to be faster since we can carry around the intermediate
   results unboxed.  The newer versions of compiler, however,
   decomposes n-ary arithmetic operations into binary ones
   during optimization, so n-ary API hadn't really been used much.
   So we dropped them, in favor of simple code. */

/*
 * Addition and subtraction
 */

static ScmObj scm_add(ScmObj arg0, ScmObj arg1, int vmp)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) + SCM_INT_VALUE(arg1);
            return Scm_MakeInteger(r);
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_BignumAddSI(SCM_BIGNUM(arg1), SCM_INT_VALUE(arg0));
        }
        if (SCM_RATNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            double z = (double)SCM_INT_VALUE(arg0) + SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_BignumAddSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumAdd(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            double z = Scm_GetDouble(arg0) + SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            double z = Scm_GetDouble(arg0) + SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            double z = SCM_FLONUM_VALUE(arg0) + (double)SCM_INT_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            double z = SCM_FLONUM_VALUE(arg0) + Scm_GetDouble(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg0) == 0.0) return arg1;
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            double z = SCM_FLONUM_VALUE(arg0) + SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg0) == 0.0) return arg1;
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   + SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    /* object-+ handling */
    SCM_FLONUM_ENSURE_MEM(arg0);
    SCM_FLONUM_ENSURE_MEM(arg1);
    return Scm_ApplyRec(SCM_OBJ(&generic_add), SCM_LIST2(arg0, arg1));
}
DEFINE_DUAL_API2(Scm_Add, Scm_VMAdd, scm_add)


static ScmObj scm_sub(ScmObj arg0, ScmObj arg1, int vmp)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) - SCM_INT_VALUE(arg1);
            return Scm_MakeInteger(r);
        }
        if (SCM_BIGNUMP(arg1)) {
            ScmObj big = Scm_MakeBignumFromSI(SCM_INT_VALUE(arg0));
            return Scm_BignumSub(SCM_BIGNUM(big), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            double z = (double)SCM_INT_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_BignumSubSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumSub(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            double z = Scm_GetDouble(arg0) - SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            double z = Scm_GetDouble(arg0) - SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            double z = SCM_FLONUM_VALUE(arg0) - (double)SCM_INT_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            double z = SCM_FLONUM_VALUE(arg0) - Scm_GetDouble(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            double z = SCM_FLONUM_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    /* object-- handling */
    return Scm_ApplyRec(SCM_OBJ(&generic_sub), SCM_LIST2(arg0, arg1));
}
DEFINE_DUAL_API2(Scm_Sub, Scm_VMSub, scm_sub)

/*
 * Multiplication
 */

static ScmObj scm_mul(ScmObj arg0, ScmObj arg1, int vmp)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long v0 = SCM_INT_VALUE(arg0);
            long v1 = SCM_INT_VALUE(arg1);
            long k;
            int ov;
            /* Using SMULOV to detect overflow portably. */
            SMULOV(k, ov, v0, v1);
            if (ov || !SCM_SMALL_INT_FITS(k)) {
                ScmObj big = Scm_MakeBignumFromSI(v0);
                return Scm_BignumMulSI(SCM_BIGNUM(big), v1);
            } else {
                return Scm_MakeInteger(k);
            }
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_BignumMulSI(SCM_BIGNUM(arg1), SCM_INT_VALUE(arg0));
        }
        if (SCM_RATNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            double z = (double)SCM_INT_VALUE(arg0) * SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   (double)SCM_INT_VALUE(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_BignumMulSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumMul(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            double z = Scm_GetDouble(arg0) * SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            double z = Scm_GetDouble(arg0);
            return Scm_MakeComplex(z * SCM_COMPNUM_REAL(arg1),
                                   z * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg1;
            double z = Scm_GetDouble(arg0) * SCM_FLONUM_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   Scm_GetDouble(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            /* inexact number * exact zero makes exact zero */
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            double z = SCM_FLONUM_VALUE(arg0) * (double)SCM_INT_VALUE(arg1);
            RETURN_FLONUM(z);
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            RETURN_FLONUM(SCM_FLONUM_VALUE(arg0) * Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 1.0) return arg0;
            RETURN_FLONUM(SCM_FLONUM_VALUE(arg0) * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   SCM_FLONUM_VALUE(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * (double)SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 1.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            double r0 = SCM_COMPNUM_REAL(arg0);
            double i0 = SCM_COMPNUM_IMAG(arg0);
            double r1 = SCM_COMPNUM_REAL(arg1);
            double i1 = SCM_COMPNUM_IMAG(arg1);
            return Scm_MakeComplex(r0 * r1 - i0 * i1,
                                   r0 * i1 + r1 * i0);
        }
        /* fallback to generic */
    }
    SCM_FLONUM_ENSURE_MEM(arg0);
    SCM_FLONUM_ENSURE_MEM(arg1);
    return Scm_ApplyRec(SCM_OBJ(&generic_mul), SCM_LIST2(arg0, arg1));
}
DEFINE_DUAL_API2(Scm_Mul, Scm_VMMul, scm_mul)

/*
 * Division
 */

/* We have three flavors of division API.
 * - Scm_Div : Scheme's `/'.  Exact division produces exact result (maybe
 *             rational)
 * - Scm_DivInexact : Scheme's `/.'.  The result is always inexact.  Fast.
 * - Scm_DivCompat : Scheme's `inexact-/'.  This is only for the backward
 *             compatibility, and probably we'll drop this in 1.0.
 *             It works as Scm_Div, except that when Scm_Div produces ratnum,
 *             Scm_DivCompat produces flonum.
 * There are also two 'VM' API, which can be used if you're returning
 * the value directly to the VM.
 * - Scm_VMDiv : The 'VM' version of Scm_Div.
 * - Scm_VMDivInexact : The 'VM' version of Scm_DivInexact.
 *
 * All these flavors are handled by a single function scm_div, with
 * three flags specifying the behavior.
 *
 *                      inexact    compat     vmp
 *  Scm_Div              FALSE      FALSE     FALSE
 *  Scm_DivInexact       TRUE       FALSE     FALSE
 *  Scm_DivCompat        d/c        TRUE      FALSE
 *  Scm_VMDiv            FALSE      FALSE     TRUE
 *  Scm_VMDivInexact     TRUE       FALSE     TRUE
 */

static ScmObj
scm_div(ScmObj arg0, ScmObj arg1, int inexact, int compat, int vmp)
{
    ScmObj r = SCM_UNBOUND;

#define SIMPLE_RETURN(x) do { r = (x); goto simple_return; } while (0)

    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) {
                if (inexact) goto anormal;
                else goto div_by_zero;
            }
            if (SCM_EXACT_ZERO_P(arg0)) SIMPLE_RETURN(arg0);
            if (SCM_EXACT_ONE_P(arg1))  SIMPLE_RETURN(arg0);
            if (compat) {
                if (SCM_INT_VALUE(arg0)%SCM_INT_VALUE(arg1) == 0) {
                    long q = SCM_INT_VALUE(arg0)/SCM_INT_VALUE(arg1);
                    return Scm_MakeInteger(q);
                } else {
                    double z = (double)SCM_INT_VALUE(arg0)
                            / (double)SCM_INT_VALUE(arg1);
                    RETURN_FLONUM(z);
                }
            } else if (inexact) {
                double z = Scm_GetDouble(arg0)/Scm_GetDouble(arg1);
                RETURN_FLONUM(z);
            } else {
                return Scm_MakeRational(arg0, arg1);
            }
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) SIMPLE_RETURN(arg0);
            goto ratnum_return;
        }
        if (SCM_RATNUMP(arg1)) {
            arg0 = Scm_Mul(arg0, SCM_RATNUM_DENOM(arg1));
            arg1 = SCM_RATNUM_NUMER(arg1);
            goto ratnum_return;
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto anormal;
            RETURN_FLONUM(SCM_INT_VALUE(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto do_complex;
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) {
                if (inexact) goto anormal;
                else goto div_by_zero;
            }
            if (SCM_EXACT_ONE_P(arg1)) SIMPLE_RETURN(arg0);
            goto ratnum_return;
        }
        if (SCM_BIGNUMP(arg1)) {
            goto ratnum_return;
        }
        if (SCM_RATNUMP(arg1)) {
            arg0 = Scm_Mul(arg0, SCM_RATNUM_DENOM(arg1));
            arg1 = SCM_RATNUM_NUMER(arg1);
            goto ratnum_return;
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto anormal;
            RETURN_FLONUM(Scm_GetDouble(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto do_complex;
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) {
                if (inexact) goto anormal;
                else goto div_by_zero;
            }
            if (SCM_EXACT_ONE_P(arg1)) SIMPLE_RETURN(arg0);
            arg1 = Scm_Mul(SCM_RATNUM_DENOM(arg0), arg1);
            arg0 = SCM_RATNUM_NUMER(arg0);
            goto ratnum_return;
        }
        if (SCM_BIGNUMP(arg1)) {
            arg1 = Scm_Mul(SCM_RATNUM_DENOM(arg0), arg1);
            arg0 = SCM_RATNUM_NUMER(arg0);
            goto ratnum_return;
        }
        if (SCM_RATNUMP(arg1)) {
            if (!compat && !inexact) {
                return Scm_RatnumDiv(arg0, arg1);
            } else {
                ScmObj numer = Scm_Mul(SCM_RATNUM_NUMER(arg0),
                                       SCM_RATNUM_DENOM(arg1));
                ScmObj denom = Scm_Mul(SCM_RATNUM_DENOM(arg0),
                                       SCM_RATNUM_NUMER(arg1));
                arg0 = numer;
                arg1 = denom;
                goto ratnum_return;
            }
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto anormal;
            RETURN_FLONUM(Scm_GetDouble(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto do_complex;
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_IS_NAN(SCM_FLONUM_VALUE(arg0))) return SCM_NAN;
            if (SCM_EXACT_ZERO_P(arg1)) goto anormal;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            RETURN_FLONUM(SCM_FLONUM_VALUE(arg0)/SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            RETURN_FLONUM(SCM_FLONUM_VALUE(arg0)/Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_IS_NAN(SCM_FLONUM_VALUE(arg0))) return SCM_NAN;
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto anormal;
            RETURN_FLONUM(SCM_FLONUM_VALUE(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto do_complex;
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            /* NB: Gauche has no exact compnum */
            if (SCM_EXACT_ZERO_P(arg1)) goto anormal_comp;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)/SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            double z = Scm_GetDouble(arg1);
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/z,
                                   SCM_COMPNUM_IMAG(arg0)/z);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto anormal_comp;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            double r0 = SCM_COMPNUM_REAL(arg0);
            double i0 = SCM_COMPNUM_IMAG(arg0);
            double r1 = SCM_COMPNUM_REAL(arg1);
            double i1 = SCM_COMPNUM_IMAG(arg1);
            double d = r1*r1+i1*i1;
            return Scm_MakeComplex((r0*r1 + i0*i1)/d,
                                   (i0*r1 - r0*i1)/d);
        }
        /* fallback to generic */
    }
    SCM_FLONUM_ENSURE_MEM(arg0);
    SCM_FLONUM_ENSURE_MEM(arg1);
    return Scm_ApplyRec(SCM_OBJ(&generic_div), SCM_LIST2(arg0, arg1));

  ratnum_return:
    {
        /* arg0 and arg1 contains exact numbers.*/
        if (compat) goto compat_return;
        if (inexact) goto inexact_return;
        return Scm_MakeRational(arg0, arg1);
    }
  compat_return:
    {
        /* We have exact integer division arg0/arg1 (arg1 != 0).
           If it doesn't produce a whole integer, we coerce the
           result to flonum. */
        ScmObj rem;
        ScmObj q = Scm_Quotient(arg0, arg1, &rem);
        if (SCM_EXACT_ZERO_P(rem)) {
            return q;
        }
        /*FALLTHROUGH*/
    }
  inexact_return:
    {
        double numer = Scm_GetDouble(arg0);
        double denom = Scm_GetDouble(arg1);
        if (SCM_IS_INF(numer) || SCM_IS_INF(denom)) {
            /* special path - we need more sophisticated calculaton. */
            ScmObj r = Scm_MakeRational(arg0, arg1);
            RETURN_FLONUM(Scm_GetDouble(r));
        } else {
            RETURN_FLONUM(numer/denom);
        }
    }
  simple_return:
    {
        if (inexact) return Scm_Inexact(r);
        else return r;
    }
  div_by_zero:
    {
      Scm_Error("attempt to calculate a division by zero");
    }
  anormal:
    /* real inexact division by zero */
    {
        int s0 = Scm_Sign(arg0);
        int s1 = SCM_FLONUMP(arg1)? Scm_FlonumSign(SCM_FLONUM_VALUE(arg1)) : 1;
        if (s0 == 0)   return SCM_NAN;
        if (s0*s1 < 0) return SCM_NEGATIVE_INFINITY;
        else           return SCM_POSITIVE_INFINITY;
    }
  anormal_comp:
    /* complex inexact division by zero */
    {
        double r0 = SCM_COMPNUM_REAL(arg0);
        double i0 = SCM_COMPNUM_IMAG(arg0);
        int s1 = SCM_FLONUMP(arg1)? Scm_FlonumSign(SCM_FLONUM_VALUE(arg1)) : 1;
        double r =
            SCM_IS_NAN(r0) ? SCM_DBL_NAN
            : ((r0*s1 > 0.0) ? SCM_DBL_POSITIVE_INFINITY
               : ((r0*s1 < 0.0) ? SCM_DBL_NEGATIVE_INFINITY
                  : SCM_DBL_NAN));
        double i =
            SCM_IS_NAN(i0) ? SCM_DBL_NAN
            : ((i0*s1 > 0.0) ? SCM_DBL_POSITIVE_INFINITY
               : ((i0*s1 < 0.0) ? SCM_DBL_NEGATIVE_INFINITY
                  : SCM_DBL_NAN));
        return Scm_MakeComplex(r, i);
    }
  do_complex:
    {
        double r1 = SCM_COMPNUM_REAL(arg1);
        double i1 = SCM_COMPNUM_IMAG(arg1);
        double d = r1*r1+i1*i1;
        return Scm_MakeComplex(r1 * Scm_GetDouble(arg0) / d,
                               -i1 * Scm_GetDouble(arg0) / d);
    }
}

ScmObj Scm_Div(ScmObj x, ScmObj y)
{
    return scm_div(x, y, FALSE, FALSE, FALSE);
}

ScmObj Scm_DivInexact(ScmObj x, ScmObj y)
{
    return scm_div(x, y, TRUE, FALSE, FALSE);
}

ScmObj Scm_DivCompat(ScmObj x, ScmObj y)
{
    return scm_div(x, y, FALSE, TRUE, FALSE);
}

ScmObj Scm_VMDiv(ScmObj x, ScmObj y)
{
    return scm_div(x, y, FALSE, FALSE, TRUE);
}

ScmObj Scm_VMDivInexact(ScmObj x, ScmObj y)
{
    return scm_div(x, y, TRUE, FALSE, TRUE);
}


/*
 * Integer division
 *   Returns (quotient x y)
 *   If rem != NULL, sets *rem to be (remainder x y) as well.
 *   We don't provide Scm_VMQuotient, assuming passing flonums to
 *   quotient is rare.
 */
ScmObj Scm_Quotient(ScmObj x, ScmObj y, ScmObj *rem)
{
    double rx, ry;

    /* Trivial shortcut.  This case may seem too specific, but actually
       it appears rather often in rational operations. */
    if (SCM_EQ(y, SCM_MAKE_INT(1))) {
        if (!Scm_IntegerP(x)) goto BADARG;
        if (rem) *rem = SCM_MAKE_INT(0);
        return x;
    }

    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            if (SCM_INT_VALUE(y) == 0) goto DIVBYZERO;
            long q = SCM_INT_VALUE(x)/SCM_INT_VALUE(y);
            if (rem) {
                long r = SCM_INT_VALUE(x)%SCM_INT_VALUE(y);
                *rem = SCM_MAKE_INT(r);
            }
            return SCM_MAKE_INT(q);
        }
        if (SCM_BIGNUMP(y)) {
            if (rem) *rem = x;
            return SCM_MAKE_INT(0);
        }
        if (SCM_FLONUMP(y)) {
            rx = (double)SCM_INT_VALUE(x);
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_BIGNUMP(x)) {
        if (SCM_INTP(y)) {
            long r;
            ScmObj q = Scm_BignumDivSI(SCM_BIGNUM(x), SCM_INT_VALUE(y), &r);
            if (rem) *rem = SCM_MAKE_INT(r);
            return q;
        } else if (SCM_BIGNUMP(y)) {
            ScmObj qr = Scm_BignumDivRem(SCM_BIGNUM(x), SCM_BIGNUM(y));
            if (rem) *rem = SCM_CDR(qr);
            return SCM_CAR(qr);
        } else if (SCM_FLONUMP(y)) {
            rx = Scm_BignumToDouble(SCM_BIGNUM(x));
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_FLONUMP(x)) {
        rx = SCM_FLONUM_VALUE(x);
        if (rx != floor(rx)) goto BADARG;
        if (SCM_INTP(y)) {
            ry = (double)SCM_INT_VALUE(y);
        } else if (SCM_BIGNUMP(y)) {
            ry = Scm_BignumToDouble(SCM_BIGNUM(y));
        } else if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
        } else {
            goto BADARGY;
        }
      DO_FLONUM:
        {
            if (ry == 0.0) goto DIVBYZERO;
            double q = (rx*ry > 0)? floor(rx/ry) : ceil(rx/ry);
            if (q == 0.0) q = 0.0; /* Don't return -0.0 */
            if (rem) {
                double rr = roundeven(rx - q*ry);
                if (rr == 0.0) rr = 0.0; /* Don't return -0.0 */
                *rem = Scm_MakeFlonum(rr);
            }
            return Scm_MakeFlonum(q);
        }
    } else {
        goto BADARG;
    }
  DIVBYZERO:
    Scm_Error("attempt to calculate a quotient by zero");
  BADARGY:
    x = y;
  BADARG:
    SCM_FLONUM_ENSURE_MEM(x);
    Scm_Error("integer required, but got %S", x);
    return SCM_UNDEFINED;       /* dummy */
}

/* Modulo and Reminder.
   We don't provide Scm_VMModulo, assuming passing flonums to modulo and
   remainder is rare.
   TODO: on gcc, % works like reminder.  I'm not sure the exact behavior
   of % is defined in ANSI C.  Need to check it later. */
ScmObj Scm_Modulo(ScmObj x, ScmObj y, int remp)
{
    double rx, ry;
    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            if (SCM_INT_VALUE(y) == 0) goto DIVBYZERO;
            long r = SCM_INT_VALUE(x)%SCM_INT_VALUE(y);
            if (!remp && r) {
                if ((SCM_INT_VALUE(x) > 0 && SCM_INT_VALUE(y) < 0)
                    || (SCM_INT_VALUE(x) < 0 && SCM_INT_VALUE(y) > 0)) {
                    r += SCM_INT_VALUE(y);
                }
            }
            return SCM_MAKE_INT(r);
        }
        if (SCM_BIGNUMP(y)) {
            if (remp) {
                return x;
            } else {
                if ((SCM_INT_VALUE(x) < 0 && SCM_BIGNUM_SIGN(y) > 0)
                    || (SCM_INT_VALUE(x) > 0 && SCM_BIGNUM_SIGN(y) < 0)) {
                    return Scm_BignumAddSI(SCM_BIGNUM(y), SCM_INT_VALUE(x));
                } else {
                    return x;
                }
            }
        }
        rx = (double)SCM_INT_VALUE(x);
        if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_BIGNUMP(x)) {
        if (SCM_INTP(y)) {
            long iy = SCM_INT_VALUE(y);
            long rem = Scm_BignumRemSI(SCM_BIGNUM(x), iy);
            if (!remp
                && rem
                && ((SCM_BIGNUM_SIGN(x) < 0 && iy > 0)
                    || (SCM_BIGNUM_SIGN(x) > 0 && iy < 0))) {
                return SCM_MAKE_INT(iy + rem);
            }
            return SCM_MAKE_INT(rem);
        }
        if (SCM_BIGNUMP(y)) {
            ScmObj rem = SCM_CDR(Scm_BignumDivRem(SCM_BIGNUM(x), SCM_BIGNUM(y)));
            if (!remp
                && (rem != SCM_MAKE_INT(0))
                && (SCM_BIGNUM_SIGN(x) * SCM_BIGNUM_SIGN(y) < 0)) {
                if (SCM_BIGNUMP(rem)) {
                    return Scm_BignumAdd(SCM_BIGNUM(y), SCM_BIGNUM(rem));
                } else {
                    return Scm_BignumAddSI(SCM_BIGNUM(y), SCM_INT_VALUE(rem));
                }
            }
            return rem;
        }
        rx = Scm_BignumToDouble(SCM_BIGNUM(x));
        if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_FLONUMP(x)) {
        double rem;
        rx = SCM_FLONUM_VALUE(x);
        if (rx != floor(rx)) goto BADARG;
        if (SCM_INTP(y)) {
            ry = (double)SCM_INT_VALUE(y);
        } else if (SCM_BIGNUMP(y)) {
            ry = Scm_BignumToDouble(SCM_BIGNUM(y));
        } else if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
        } else {
            goto BADARGY;
        }
      DO_FLONUM:
        if (ry == 0.0) goto DIVBYZERO;
        rem = fmod(rx, ry);
        if (!remp && rem != 0.0) {
            if ((rx > 0 && ry < 0) || (rx < 0 && ry > 0)) {
                rem += ry;
            }
        }
        if (rem == 0.0) rem = 0.0; /* Don't return -0.0 */
        return Scm_MakeFlonum(rem);
    } else {
        goto BADARG;
    }
  DIVBYZERO:
    Scm_Error("attempt to take a modulo or remainder by zero");
  BADARGY:
    x = y;
  BADARG:
    SCM_FLONUM_ENSURE_MEM(x);
    Scm_Error("integer required, but got %S", x);
    return SCM_UNDEFINED;       /* dummy */
}

/*
 * Gcd
 */

/* assumes x > y >= 0 */
static u_long gcd_fixfix(u_long x, u_long y)
{
    while (y > 0) {
        u_long r = x % y;
        x = y;
        y = r;
    }
    return x;
}

static double gcd_floflo(double x, double y)
{
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    if (x < y) { double t = x; x = y; y = t; }

    while (y > 0.0) {
        double r = fmod(x, y);
        x = y;
        y = r;
    }
    return x;
}

/* assumes y <= LONG_MAX.  curiously, the sign of x doesn't matter,
   since it only affects the remainder's sign which we adjust afterwards. */
static u_long gcd_bigfix(ScmBignum *x, u_long y)
{
    long rem;
    (void)Scm_BignumDivSI(x, (signed long)y, &rem);
    if (rem < 0) rem = -rem;
    return gcd_fixfix(y, (u_long)rem);
}

/* We don't provide Scm_VMGcd, assuming passing flonums to gcd is rare. */
ScmObj Scm_Gcd(ScmObj x, ScmObj y)
{
    if (!Scm_IntegerP(x)) {
        Scm_Error("integer required, but got %S", x);
    }
    if (!Scm_IntegerP(y)) {
        Scm_Error("integer required, but got %S", y);
    }
    if (SCM_FLONUMP(x) || SCM_FLONUMP(y)) {
        return Scm_MakeFlonum(gcd_floflo(Scm_GetDouble(x), Scm_GetDouble(y)));
    }

    if (SCM_EXACT_ZERO_P(x)) return y;
    if (SCM_EXACT_ZERO_P(y)) return x;

    int ox = FALSE, oy = FALSE;
    long ix = Scm_GetIntegerClamp(x, SCM_CLAMP_NONE, &ox);
    long iy = Scm_GetIntegerClamp(y, SCM_CLAMP_NONE, &oy);

    if (!ox && !oy) {
        u_long ux = (ix < 0)? -ix : ix;
        u_long uy = (iy < 0)? -iy : iy;
        u_long ur = (ux >= uy)? gcd_fixfix(ux, uy) : gcd_fixfix(uy, ux);
        return Scm_MakeIntegerU(ur);
    }

    if (!oy && iy != LONG_MIN) {
        /* x overflows long.  y doesn't.  so we know abs(x) > abs(y)
           (abs(x) == abs(y) iff LONG_MAX+1 and y == LONG_MIN, but we've
           excluded it above). */
        SCM_ASSERT(SCM_BIGNUMP(x));
        u_long uy = (iy < 0)? -iy : iy;
        u_long ur = gcd_bigfix(SCM_BIGNUM(x), uy);
        return Scm_MakeIntegerU(ur);
    }

    if (!ox && ix != LONG_MIN) {
        /* reverse condition of above */
        SCM_ASSERT(SCM_BIGNUMP(y));
        u_long ux = (ix < 0)? -ix : ix;
        u_long ur = gcd_bigfix(SCM_BIGNUM(y), ux);
        return Scm_MakeIntegerU(ur);
    }

    /* Now we need to treat both args as bignums.  We could use
       Algorithm L in Knuth's TAOCP 4.5.2, but we assume this path
       is rarely executed, so we don't bother for now. */
    x = Scm_Abs(x);
    y = Scm_Abs(y);
    if (Scm_NumCmp(x, y) < 0) {ScmObj t = x; x = y; y = t;}

    while (!SCM_EXACT_ZERO_P(y)) {
        ScmObj r = Scm_Modulo(x, y, TRUE);
        x = y;
        y = r;
    }
    return x;
}

/*===============================================================
 * Exponential and trigometric functions
 */

/* Integer power of 10.  It is extensively used during string->number
   and number->string operations.
   IEXPT10_TABLESIZ is ceil(-log10(ldexp(1.0, -1022-52))) + 2 */
/* NB: actually we need more margin here to handle denormalized numbers. */
#define IEXPT10_TABLESIZ  341
static ScmObj iexpt10_n[IEXPT10_TABLESIZ] = { NULL };
static int    iexpt10_initialized = FALSE;

static void iexpt10_init(void)
{
    iexpt10_n[0] = SCM_MAKE_INT(1);
    iexpt10_n[1] = SCM_MAKE_INT(10);
    iexpt10_n[2] = SCM_MAKE_INT(100);
    iexpt10_n[3] = SCM_MAKE_INT(1000);
    iexpt10_n[4] = SCM_MAKE_INT(10000);
    iexpt10_n[5] = SCM_MAKE_INT(100000);
    iexpt10_n[6] = SCM_MAKE_INT(1000000);
    for (int i=7; i<IEXPT10_TABLESIZ; i++) {
        iexpt10_n[i] = Scm_Mul(iexpt10_n[i-1], SCM_MAKE_INT(10));
    }
    iexpt10_initialized = TRUE;
}

#define IEXPT10_INIT() \
    do { if (!iexpt10_initialized) iexpt10_init(); } while (0)

/* expt(x, y) where x is exact and y is integer */
ScmObj Scm_ExactIntegerExpt(ScmObj x, ScmObj y)
{
    int sign = Scm_Sign(y);
    ScmObj r = SCM_MAKE_INT(1);

    if (sign == 0) return r;
    if (SCM_EQ(x, SCM_MAKE_INT(1))) return r;
    if (SCM_EQ(x, SCM_MAKE_INT(-1))) return Scm_OddP(y)? SCM_MAKE_INT(-1) : r;

    if (!SCM_INTP(y)) {
        /* who wants such a heavy calculation? */
        Scm_Error("exponent too big: %S", y);
    }
    long iy = SCM_INT_VALUE(y);
    /* Shortcut for special cases */
    if (SCM_EQ(x, SCM_MAKE_INT(10)) && iy > 0 && iy < IEXPT10_TABLESIZ) {
        /* We have a precalculated table for 10^y */
        IEXPT10_INIT();
        r = iexpt10_n[iy];
    } else if (SCM_EQ(x, SCM_MAKE_INT(2)) && iy > 0) {
        /* Use shift operation for 2^y, y>0 */
        r = Scm_Ash(SCM_MAKE_INT(1), iy);
    } else {
        /* General case */
        if (iy < 0) iy = -iy;
        for (;;) {
            if (iy == 0) break;
            if (iy == 1) { r = Scm_Mul(r, x); break; }
            if (iy & 0x01) r = Scm_Mul(r, x);
            x = Scm_Mul(x, x);
            iy >>= 1;
        }
    }
    return (sign < 0)? Scm_Reciprocal(r) : r;
}

static ScmObj scm_expt(ScmObj x, ScmObj y, int vmp)
{
    /* NB: The exact case is handled by expt in libnum.scm; we check this case
       just for the backward compatibility. */
    if (SCM_EXACTP(x) && SCM_INTEGERP(y)) return Scm_ExactIntegerExpt(x, y);
    if (!SCM_REALP(x)) Scm_Error("real number required, but got %S", x);
    if (!SCM_REALP(y)) Scm_Error("real number required, but got %S", y);
    double dx = Scm_GetDouble(x);
    double dy = Scm_GetDouble(y);
    if (dy == 0.0) {
        RETURN_FLONUM(1.0);
    } else if (dx < 0 && !Scm_IntegerP(y)) {
        /* x^y == exp(y * log(x)) = exp(y*log(|x|))*exp(y*arg(x)*i)
           if x is a negative real number, arg(x) == pi
        */
        double mag = exp(dy * log(-dx));
        return Scm_MakeComplex(mag * Scm_CosPi(dy), mag * Scm_SinPi(dy));
    } else {
        RETURN_FLONUM(pow(dx, dy));
    }
}
DEFINE_DUAL_API2(Scm_Expt, Scm_VMExpt, scm_expt)

/* If num is exact 2^s (s >= 0), returns s.  Otherwise returns -1. */
long Scm_TwosPower(ScmObj n)
{
    if (SCM_INTP(n)) {
        long i = SCM_INT_VALUE(n);
        if (i <= 0) return -1;
        if ((i<<1) == ((i ^ (i-1)) + 1)) {
            return Scm__HighestBitNumber(i);
        }
        /*FALTHROUGH*/
    } else if (SCM_BIGNUMP(n) && SCM_BIGNUM_SIGN(n) > 0) {
        ScmBits *b = (ScmBits*)SCM_BIGNUM(n)->values;
        int l = SCM_BIGNUM_SIZE(n) * SCM_WORD_BITS;
        int c = Scm_BitsLowest1(b, 0, l);
        if (c == Scm_BitsHighest1(b, 0, l)) return c;
        /*FALTHROUGH*/
    }
    return -1;
}

/* sinpi(x) = sin(x * pi)
   cospi(x) = cos(x * pi)
   tanpi(x) = tan(x * pi)

   We first reduce input range to -1 <= x <= 1 by trig_pi_reduce_range.
 */
static double trig_pi_reduce_range(double x)
{
    double xx = fmod(x, 2.0);   /* -2.0 < x < 2.0 */
    if (xx > 1.0)  return xx - 2.0;
    if (xx < -1.0) return xx + 2.0;
    if (xx == 0.0) return 0.0; /* we don't return -0.0 */
    else return xx;
}

double Scm_SinPi(double x)
{
    double xx = trig_pi_reduce_range(x);
    if (xx >= 0) {
        if (xx > 0.5)  xx = 1 - xx;
        if (xx > 0.25) return cos(M_PI*(0.5-xx));
        else return sin(M_PI*xx);
    } else {
        if (xx < -0.5) xx = -1 - xx;
        if (xx < -0.25) return -cos(M_PI*(-0.5-xx));
        else return sin(M_PI*xx);
    }
}

double Scm_CosPi(double x)
{
    double xx = fabs(trig_pi_reduce_range(x));
    if (xx >= 0.75) return -cos(M_PI*(1-xx));
    if (xx >  0.25) return sin(M_PI*(0.5-xx));
    else            return cos(M_PI*xx);
}

double Scm_TanPi(double x)
{
    return Scm_SinPi(x)/Scm_CosPi(x);
}

/*===============================================================
 * Comparison
 */

static inline int nan_p(ScmObj arg)
{
    return (SCM_FLONUMP(arg) && SCM_IS_NAN(SCM_FLONUM_VALUE(arg)));
}

static inline int either_nan_p(ScmObj arg0, ScmObj arg1)
{
    return (nan_p(arg0) || nan_p(arg1));
}

int Scm_NumEq(ScmObj arg0, ScmObj arg1)
{
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_COMPNUMP(arg1)) {
            return ((SCM_COMPNUM_REAL(arg0) == SCM_COMPNUM_REAL(arg1))
                    && (SCM_COMPNUM_IMAG(arg0) == SCM_COMPNUM_IMAG(arg1)));
        }
        return FALSE;
    } else {
        if (SCM_COMPNUMP(arg1)) return FALSE;
        if (either_nan_p(arg0, arg1)) return FALSE;
        return (Scm_NumCmp(arg0, arg1) == 0);
    }
}

int Scm_NumLT(ScmObj arg0, ScmObj arg1)
{
    if (either_nan_p(arg0, arg1)) return FALSE;
    return Scm_NumCmp(arg0, arg1) < 0;
}

int Scm_NumLE(ScmObj arg0, ScmObj arg1)
{
    if (either_nan_p(arg0, arg1)) return FALSE;
    return Scm_NumCmp(arg0, arg1) <= 0;
}

int Scm_NumGT(ScmObj arg0, ScmObj arg1)
{
    if (either_nan_p(arg0, arg1)) return FALSE;
    return Scm_NumCmp(arg0, arg1) > 0;
}

int Scm_NumGE(ScmObj arg0, ScmObj arg1)
{
    if (either_nan_p(arg0, arg1)) return FALSE;
    return Scm_NumCmp(arg0, arg1) >= 0;
}

/* 2-arg comparison.
   NB: This routine is called from VM or built-in SUBRs when arg0 and/or
   arg1 contain register flonums.  That's why we insert SCM_FLONUM_ENSURE_MEM
   in the path to Scm_Error at the end.  For all other paths we know the
   register flonum will never leak out.   If you make changes here,
   keep in mind that args can be register flonums, and make sure to insert
   SCM_FLONUM_ENSURE_MEM wherever they can leak out.

   Caveat: Scm_NumCmp returns 0 (means equal) when arg0 and/or arg1 is/are NaN.
   That's because NaN doesn't make any sense in three-way comparison.  The
   premise is that NaN has already been filtered out before NumCmp is called.

   Pitfall: R7RS requires '=' transitivity.  When comparing an exact number vs
   an inexact number, simply converting the exact one to inexact can't satisfy
   this requirement (cf. https://github.com/shirok/Gauche/issues/805 )
   However, we don't want to always convert the inexact one to exact, for
   it can be expensive.  So we first compare inexactly, and only when it
   yields equal and we determine we need more precision, we do exact
   comparison.
 */
int Scm_NumCmp(ScmObj arg0, ScmObj arg1)
{
    ScmObj badnum;

    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) - SCM_INT_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_FLONUMP(arg1)) {
            double r = (double)SCM_INT_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
#if SIZEOF_LONG >= 8
            /* exact v inexact case.  see if we need more precise comparison */
            if (!double_precision(arg0, NULL, NULL)) {
                return Scm_NumCmp(arg0, Scm_Exact(arg1));
            }
#endif
            return 0;
        }
        if (SCM_BIGNUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(Scm_MakeBignumFromSI(SCM_INT_VALUE(arg0))),
                                 SCM_BIGNUM(arg1));
        if (SCM_RATNUMP(arg1)) {
            if (SCM_EQ(SCM_INT_VALUE(0), arg0)) {
                return -Scm_Sign(arg1);
            } else {
                /* Roughly estimates the result by coercing the RATNUM to
                   double.  We have 53bits of precision (denomalized
                   numbers only matter when ARG0 is zero, which is excluded
                   already). */
                double y = Scm_GetDouble(arg1);
                double r = SCM_INT_VALUE(arg0) - y;
                double err = y * 2.0e-52;

                if (r < -err) return -1;
                if (r > err) return 1;
                /* We need more precise comparison. */
                return Scm_NumCmp(Scm_Mul(arg0, SCM_RATNUM_DENOM(arg1)),
                                  SCM_RATNUM_NUMER(arg1));
            }
        }
        badnum = arg1;
    }
    else if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            double r = SCM_FLONUM_VALUE(arg0) - SCM_INT_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
#if SIZEOF_LONG >= 8
            /* exact v inexact case.  see if we need more precise comparison */
            if (!double_precision(arg1, NULL, NULL)) {
                return Scm_NumCmp(Scm_Exact(arg0), arg1);
            }
#endif
            return 0;
        }
        if (SCM_FLONUMP(arg1)) {
            double r = SCM_FLONUM_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            /* NaN is already excluded.  We filter out obvious. */
            if (SCM_IS_INF(SCM_FLONUM_VALUE(arg0))) {
                return Scm_Sign(arg0);
            }
            double r = SCM_FLONUM_VALUE(arg0) - Scm_GetDouble(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            /* exact v inexact */
            return Scm_NumCmp(Scm_Exact(arg0), arg1);
        }
        badnum = arg1;
    }
    else if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(arg0),
                                 SCM_BIGNUM(Scm_MakeBignumFromSI(SCM_INT_VALUE(arg1))));
        if (SCM_FLONUMP(arg1))
            return -Scm_NumCmp(arg1, arg0);
        if (SCM_BIGNUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        if (SCM_RATNUMP(arg1)) {
            /* we can't coerce to flonum, for it may lose precision. */
            ScmObj d1 = SCM_RATNUM_DENOM(arg1);
            return Scm_NumCmp(Scm_Mul(arg0, d1),
                              SCM_RATNUM_NUMER(arg1));
        }
        badnum = arg1;
    }
    else if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1) || SCM_BIGNUMP(arg1) || SCM_FLONUMP(arg1)) {
            return -Scm_NumCmp(arg1, arg0);
        }
        if (SCM_RATNUMP(arg1)) {
            ScmObj n0 = SCM_RATNUM_NUMER(arg0), d0 = SCM_RATNUM_DENOM(arg0);
            ScmObj n1 = SCM_RATNUM_NUMER(arg1), d1 = SCM_RATNUM_DENOM(arg1);
            int s0 = Scm_Sign(n0), s1 = Scm_Sign(n1);

            /* screen the obvious cases without allocating new numbers */
            if (s0 < s1) return -1;
            if (s0 > s1) return 1;
            int d = Scm_NumCmp(d0, d1);
            if (d == 0) return Scm_NumCmp(n0, n1);
            if ((s0 > 0 && s1 > 0) || (s0 < 0 && s1 < 0)) {
                int n = Scm_NumCmp(n0, n1) * s0;
                if (d > 0 && n <= 0) return -s0;
                if (d < 0 && n >= 0) return s0;
            }

            return Scm_NumCmp(Scm_Mul(n0, d1),
                              Scm_Mul(n1, d0));
        }
        badnum = arg1;
    }
    else badnum = arg0;

    SCM_FLONUM_ENSURE_MEM(badnum);
    Scm_Error("real number required: %S", badnum);
    return 0;                    /* dummy */
}

void Scm_MinMax(ScmObj arg0, ScmObj args, ScmObj *min, ScmObj *max)
{
    if (!SCM_REALP(arg0)) Scm_Error("real number required, but got %S", arg0);
    if (nan_p(arg0)) goto got_nan;
    int inexact = !SCM_EXACTP(arg0);
    ScmObj mi = arg0;
    ScmObj ma = arg0;

    for (;SCM_PAIRP(args); args = SCM_CDR(args)) {
        ScmObj arg = SCM_CAR(args);
        if (!SCM_REALP(arg)) Scm_Error("real number required, but got %S", arg);
        if (nan_p(arg)) goto got_nan;
        if (!SCM_EXACTP(arg)) inexact = TRUE;
        if (min && Scm_NumCmp(mi, arg) > 0) mi = arg;
        if (max && Scm_NumCmp(ma, arg) < 0) ma = arg;
    }
    if (min) {
        if (inexact && SCM_EXACTP(mi)) {
            *min = Scm_Inexact(mi);
        } else {
            *min = mi;
        }
    }
    if (max) {
        if (inexact && SCM_EXACTP(ma)) {
            *max = Scm_Inexact(ma);
        } else {
            *max = ma;
        }
    }
    return;
 got_nan:
    if (min) *min = SCM_NAN;
    if (max) *max = SCM_NAN;
}

/*===============================================================
 * ROUNDING
 */

/* NB: rint() is not in POSIX, so an alternative is provided here.
   We don't use round(), for it behaves differently when the
   argument is exactly the halfway of two whole numbers. */
#ifdef HAVE_RINT
#define roundeven rint
#else  /* !HAVE_RINT */
static double roundeven(double v)
{
    double r;
    double frac = modf(v, &r);
    if (v > 0.0) {
        if (frac > 0.5) r += 1.0;
        else if (frac == 0.5) {
            if (fmod(r,2.0) != 0.0) r += 1.0;
        }
    } else {
        if (frac < -0.5) r -= 1.0;
        else if (frac == -0.5) {
            if (fmod(r,2.0) != 0.0) r -= 1.0;
        }
    }
    return r;
}
#endif /* !HAVE_RINT */

static ScmObj scm_round(ScmObj num, int mode, int vmp)
{

    if (SCM_INTEGERP(num)) return num;
    if (SCM_RATNUMP(num)) {
        int offset = 0;
        ScmObj rem;
        ScmObj quot = Scm_Quotient(SCM_RATNUM_NUMER(num),
                                   SCM_RATNUM_DENOM(num), &rem);
        /* this shouldn't happen, but just in case.. */
        if (SCM_EXACT_ZERO_P(rem)) return quot;

        /* Here we have quotient, which is always closer to zero
           than the original value */
        switch (mode) {
        case SCM_ROUND_FLOOR:
            offset = (Scm_Sign(num) < 0)? -1 : 0;
            break;
        case SCM_ROUND_CEIL:
            offset = (Scm_Sign(num) < 0)? 0 : 1;
            break;
        case SCM_ROUND_TRUNC:
            offset = 0;
            break;
        case SCM_ROUND_ROUND: {
            ScmObj rem2 = Scm_Mul(Scm_Abs(rem), SCM_MAKE_INT(2));
            int cmp = Scm_NumCmp(SCM_RATNUM_DENOM(num), rem2);

            if (cmp > 0) {
                /* NUM is closer to zero than halfway */
                offset = 0;
            } else if (cmp < 0) {
                /* NUM is further from zero than halfway */
                offset = (Scm_Sign(num) < 0)? -1 : 1;
            } else {
                /* NUM is exactly the halfway.  We round to even */
                if (Scm_OddP(quot)) {
                    offset = (Scm_Sign(num) < 0)? -1 : 1;
                } else {
                    offset = 0;
                }
            }
            break;
        }
        default: Scm_Panic("something screwed up");
        }

        if (offset == 0) return quot;
        else return scm_add(quot, SCM_MAKE_INT(offset), vmp);
    }
    if (SCM_FLONUMP(num)) {
        double r = 0.0;
        double v = SCM_FLONUM_VALUE(num);
        switch (mode) {
        case SCM_ROUND_FLOOR: r = floor(v); break;
        case SCM_ROUND_CEIL:  r = ceil(v); break;
        /* trunc is neither in ANSI nor in POSIX. */
#ifdef HAVE_TRUNC
        case SCM_ROUND_TRUNC: r = trunc(v); break;
#else
        case SCM_ROUND_TRUNC: r = (v < 0.0)? ceil(v) : floor(v); break;
#endif
        case SCM_ROUND_ROUND: r = roundeven(v); break;
        default: Scm_Panic("something screwed up");
        }
        if (r == 0.0) r = 0.0;  /* Don't return -0.0 */
        RETURN_FLONUM(r);
    }
    Scm_Error("real number required, but got %S", num);
    return SCM_UNDEFINED;       /* dummy */
}

ScmObj Scm_Round(ScmObj num, int mode)
{
    return scm_round(num, mode, FALSE);
}

ScmObj Scm_VMRound(ScmObj num, int mode)
{
    return scm_round(num, mode, TRUE);
}


ScmObj Scm_RoundToExact(ScmObj num, int mode)
{
    if (SCM_FLONUMP(num)) {
        double r = 0.0;
        double v = SCM_FLONUM_VALUE(num);
        if (SCM_IS_NAN(v) || SCM_IS_INF(v)) {
            Scm_Error("Exact infinity/nan is not supported: %S", num);
        }
        switch (mode) {
        case SCM_ROUND_FLOOR: r = floor(v); break;
        case SCM_ROUND_CEIL:  r = ceil(v); break;
        /* trunc is neither in ANSI nor in POSIX. */
#ifdef HAVE_TRUNC
        case SCM_ROUND_TRUNC: r = trunc(v); break;
#else
        case SCM_ROUND_TRUNC: r = (v < 0.0)? ceil(v) : floor(v); break;
#endif
        case SCM_ROUND_ROUND: r = roundeven(v); break;
        default: Scm_Panic("something screwed up");
        }
        return Scm_FlonumIntegerToExact(r);
    }
    if (SCM_INTEGERP(num)) return num;
    if (SCM_RATNUMP(num))  return Scm_Round(num, mode);
    Scm_Error("real number required, but got %S", num);
    return SCM_UNDEFINED;       /* dummy */
}

/*===============================================================
 * Logical (bitwise) operations
 */

ScmObj Scm_Ash(ScmObj x, ScmSmallInt cnt)
{
    /* TODO: This is an arbitrary limit, but we need *some* limit anyway
       to prevent a silly mistake from consuming large amount of memory.
       Eventually we need a consistent limit on how big a bignum can be. */
    if (cnt >= 0x10000000) {
        Scm_Error("ash: shift amount too big to handle: %ld", cnt);
    }

    if (SCM_INTP(x)) {
        long ix = SCM_INT_VALUE(x);
        if (cnt <= -(SIZEOF_LONG * 8)) {
            ix = (ix < 0)? -1 : 0;
            return Scm_MakeInteger(ix);
        } else if (cnt < 0) {
            if (ix < 0) {
                ix = ~((~ix) >> (-cnt));
            } else {
                ix >>= -cnt;
            }
            return Scm_MakeInteger(ix);
        } else if (cnt < SCM_SMALL_INT_SIZE) {
            if (ix < 0) {
                if (-ix < (SCM_SMALL_INT_MAX >> cnt)) {
                    ix <<= cnt;
                    return Scm_MakeInteger(ix);
                }
            } else {
                if (ix < (SCM_SMALL_INT_MAX >> cnt)) {
                    ix <<= cnt;
                    return Scm_MakeInteger(ix);
                }
            }
        }
        /* Here, we know the result must be a bignum. */
        {
            ScmObj big = Scm_MakeBignumFromSI(ix);
            return Scm_BignumAsh(SCM_BIGNUM(big), cnt);
        }
    } else if (SCM_BIGNUMP(x)) {
        return Scm_BignumAsh(SCM_BIGNUM(x), cnt);
    }
    Scm_Error("exact integer required, but got %S", x);
    return SCM_UNDEFINED;
}

ScmObj Scm_LogNot(ScmObj x)
{
    if (!SCM_INTEGERP(x)) Scm_Error("exact integer required, but got %S", x);
    if (SCM_INTP(x)) {
        /* this won't cause an overflow */
        return SCM_MAKE_INT(~SCM_INT_VALUE(x));
    } else {
        return Scm_Negate(Scm_BignumAddSI(SCM_BIGNUM(x), 1));
    }
}

ScmObj Scm_LogAnd(ScmObj x, ScmObj y)
{
    if (!SCM_INTEGERP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_INTEGERP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            return SCM_MAKE_INT(SCM_INT_VALUE(x) & SCM_INT_VALUE(y));
        } else if (SCM_INT_VALUE(x) >= 0 && SCM_BIGNUM_SIGN(y) >= 0) {
            return Scm_MakeInteger(SCM_INT_VALUE(x)&SCM_BIGNUM(y)->values[0]);
        }
        x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else if (SCM_INTP(y)) {
        if (SCM_INT_VALUE(y) >= 0 && SCM_BIGNUM_SIGN(x) >= 0) {
            return Scm_MakeInteger(SCM_INT_VALUE(y)&SCM_BIGNUM(x)->values[0]);
        }
        y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    }
    return Scm_BignumLogAnd(SCM_BIGNUM(x), SCM_BIGNUM(y));
}

ScmObj Scm_LogIor(ScmObj x, ScmObj y)
{
    if (!SCM_INTEGERP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_INTEGERP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y))
            return SCM_MAKE_INT(SCM_INT_VALUE(x) | SCM_INT_VALUE(y));
        else
            x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else {
        if (SCM_INTP(y)) y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    }
    return Scm_BignumLogIor(SCM_BIGNUM(x), SCM_BIGNUM(y));
}


ScmObj Scm_LogXor(ScmObj x, ScmObj y)
{
    if (!SCM_INTEGERP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_INTEGERP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y))
            return SCM_MAKE_INT(SCM_INT_VALUE(x) ^ SCM_INT_VALUE(y));
        else
            x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else {
        if (SCM_INTP(y)) y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    }
    return Scm_BignumLogXor(SCM_BIGNUM(x), SCM_BIGNUM(y));
}


/*===============================================================
 * System-specific integral types
 */

static ScmObj SCM_SIZE_T_MAX;
static ScmObj SCM_SSIZE_T_MAX;
static ScmObj SCM_SSIZE_T_MIN;
static ScmObj SCM_PTRDIFF_T_MAX;
static ScmObj SCM_PTRDIFF_T_MIN;

int Scm_IntegerFitsSizeP(ScmObj i)
{
    if (SCM_INTP(i)) return SCM_INT_VALUE(i) >= 0;
    if (SCM_BIGNUMP(i)) {
        return (SCM_BIGNUM_SIGN(i) >= 0
                && Scm_NumCmp(i, SCM_SSIZE_T_MAX) <= 0);
    }
    return FALSE;
}

/* i must satisfy Scm_IntegerFitsSizeP */
size_t Scm_IntegerToSize(ScmObj i)
{
    if (SCM_INTP(i) && SCM_INT_VALUE(i) >= 0) {
        return (size_t)SCM_INT_VALUE(i);
    } else if (SCM_BIGNUMP(i) && SCM_BIGNUM_SIGN(i) >= 0) {
#if SIZEOF_SIZE_T == SIZEOF_LONG
        return (size_t)Scm_GetIntegerUClamp(i, SCM_CLAMP_ERROR, NULL);
#elif SIZEOF_SIZE_T == 8
        return (size_t)Scm_GetIntegerU64Clamp(i, SCM_CLAMP_ERROR, NULL);
#else
        /* I don't think there's such an architecture. */
# error "size_t size on this platform is not suported."
#endif
    }
    Scm_Error("bad value as size_t: %S", i);
    return (size_t)-1;       /* dummy */
}

ScmObj Scm_SizeToInteger(size_t off)
{
#if SIZEOF_SIZE_T == SIZEOF_LONG
    return Scm_MakeInteger(off);
#elif SIZEOF_SIZE_T == 8
    return Scm_MakeInteger64((int64_t)off);
#else
# error "size_t size on this platform is not suported."
#endif
}

int Scm_IntegerFitsSsizeP(ScmObj i)
{
    if (SCM_INTP(i)) return TRUE; /* all fixnums should fit in ssize_t */
    if (SCM_BIGNUMP(i)) {
        return (Scm_NumCmp(i, SCM_SSIZE_T_MIN) >= 0
                && Scm_NumCmp(i, SCM_SSIZE_T_MAX) <= 0);
    }
    return FALSE;
}

/* i must satisfy Scm_IntegerFitsSsizeP */
ssize_t Scm_IntegerToSsize(ScmObj i)
{
    if (SCM_INTP(i)) return (ssize_t)SCM_INT_VALUE(i);
    if (SCM_BIGNUMP(i)) {
#if SIZEOF_SSIZE_T == SIZEOF_LONG
        return (ssize_t)Scm_GetIntegerClamp(i, SCM_CLAMP_ERROR, NULL);
#elif SIZEOF_SSIZE_T == 8
        return (ssize_t)Scm_GetInteger64Clamp(i, SCM_CLAMP_ERROR, NULL);
#else
        /* I don't think there's such an architecture. */
# error "ssize_t size on this platform is not suported."
#endif
    }
    Scm_Error("bad value as ssize_t: %S", i);
    return (ssize_t)-1;       /* dummy */
}

/* There're platforms where pointer width is smaller than long's, so
   not all fixnums may fit in ptfdiff_t.
 */
int Scm_IntegerFitsPtrdiffP(ScmObj i)
{
    if (SCM_INTEGERP(i)) {
        return (Scm_NumCmp(i, SCM_PTRDIFF_T_MIN) >= 0
                && Scm_NumCmp(i, SCM_PTRDIFF_T_MAX) <= 0);
    }
    return FALSE;
}

ptrdiff_t Scm_IntegerToPtrdiff(ScmObj i)
{
    if (SCM_INTEGERP(i)) {
#if SIZEOF_PTRDIFF_T == SIZEOF_LONG
        return (ptrdiff_t)Scm_GetIntegerClamp(i, SCM_CLAMP_ERROR, NULL);
#elif SIZEOF_PTRDIFF_T == 8
        return (ptrdiff_t)Scm_GetInteger64Clamp(i, SCM_CLAMP_ERROR, NULL);
#else
# error "ptrdiff_t size on this platform is not suported."
#endif
    }
    Scm_Error("bad value as ptrdiff: %S", i);
    return (ptrdiff_t)-1;       /* dummy */
}

ScmObj Scm_PtrdiffToInteger(ptrdiff_t d)
{
#if SIZEOF_PTRDIFF_T == SIZEOF_LONG
    return Scm_MakeInteger((long)d);
#elif SIZEOF_PTRDIFF_T == 8
    return Scm_MakeInteger64((int64_t)d);
#else
# error "ptrdiff_t size on this platform is not suported."
#endif
}

/* Range of off_t is not provided in POSIX.  We assume the range of signed
   integer that fits in the size of off_t. */
int Scm_IntegerFitsOffsetP(ScmObj i)
{
    if (SCM_INTP(i)) return TRUE;
#if SIZEOF_OFF_T == 4
    if (SCM_BIGNUMP(i)) {
        return (Scm_NumCmp(i, SCM_MINUS_2_31) >= 0
                && Scm_NumCmp(i, SCM_2_31) >= 0);
    }
#elif SIZEOF_OFF_T == 8
    if (SCM_BIGNUMP(i)) {
        return (Scm_NumCmp(i, SCM_MINUS_2_63) >= 0
                && Scm_NumCmp(i, SCM_2_63) >= 0);
    }
#else
# error "off_t size on this platform is not suported."
#endif
    return FALSE;
}

off_t Scm_IntegerToOffset(ScmObj i)
{
    if (SCM_INTP(i)) {
        return (off_t)SCM_INT_VALUE(i);
    } else if (SCM_BIGNUMP(i)) {
#if SIZEOF_OFF_T == SIZEOF_LONG
        return (off_t)Scm_GetIntegerClamp(i, SCM_CLAMP_ERROR, NULL);
#elif SIZEOF_OFF_T == 8
        return (off_t)Scm_GetInteger64Clamp(i, SCM_CLAMP_ERROR, NULL);
#else
# error "off_t size on this platform is not suported."
#endif
    }
    Scm_Error("bad value as offset: %S", i);
    return (off_t)-1;       /* dummy */
}

ScmObj Scm_OffsetToInteger(off_t off)
{
#if SIZEOF_OFF_T == SIZEOF_LONG
    return Scm_MakeInteger(off);
#elif SIZEOF_OFF_T == 8
    return Scm_MakeInteger64((int64_t)off);
#else
# error "off_t size on this platform is not suported."
#endif
}

intptr_t Scm_IntegerToIntptr(ScmObj i)
{
    if (SCM_INTP(i)) {
        return (intptr_t)SCM_INT_VALUE(i);
    } else if (SCM_BIGNUMP(i)) {
#if SIZEOF_INTPTR_T == SIZEOF_LONG
        return (intptr_t)Scm_GetIntegerClamp(i, SCM_CLAMP_ERROR, NULL);
#elif SIZEOF_OFF_T == 8
        return (intptr_t)Scm_GetInteger64Clamp(i, SCM_CLAMP_ERROR, NULL);
#else
        /* I don't think there's such an architecture. */
# error "intptr_t size on this platform is not suported."
#endif
    }
    Scm_Error("bad value as intptr: %S", i);
    return (intptr_t)-1;       /* dummy */
}

ScmObj Scm_IntptrToInteger(intptr_t i)
{
#if SIZEOF_INTPTR_T == SIZEOF_LONG
    return Scm_MakeInteger(i);
#elif SIZEOF_INTPTR_T == 8
    return Scm_MakeInteger64((int64_t)i);
#else
# error "intptr_t size on this platform is not suported."
#endif
}





/*===============================================================
 * Number I/O
 */

/* contants frequently used in number I/O */
static double dexpt2_minus_52  = 0.0;  /* 2.0^-52 */
static double dexpt2_minus_53  = 0.0;  /* 2.0^-53 */

/* max N where 10.0^N can be representable exactly in double.
   it is max N where N * log2(5) < 53. */
#define MAX_EXACT_10_EXP  23

/* fast 10^n for limited cases */
static inline ScmObj iexpt10(int e)
{
    if (e < IEXPT10_TABLESIZ) {
        return iexpt10_n[e];
    } else {
        /* This recursive case can happen if excessive number of decimal
           digits are given below the decimal point, and the exponent
           is close to minimum.  There may be more efficient way to
           prune lower digits so that we can keep scale factor bounded,
           since those lower digits are very unlikely to contribute to
           the result.  But it is difficult to avoid double-rounding
           error compeletely, so we take safer path for now. */
        return Scm_Mul(iexpt10_n[IEXPT10_TABLESIZ-1],
                       iexpt10(e - IEXPT10_TABLESIZ + 1));
    }
}

/* integer power of R by N, N is rather small.
   Assuming everything is in range. */
static inline u_long ipow(int r, int n)
{
    u_long k;
    for (k=1; n>0; n--) k *= r;
    return k;
}

/* X * 10.0^N by double.
   10.0^N can be represented _exactly_ in double-precision floating point
   number in the range 0 <= N <= 23.
   If N is out of this range, a rounding error occurs, which will be
   corrected in the algorithmR routine below. */
static double raise_pow10(double x, int n)
{
    static double dpow10[] = { 1.0, 1.0e1, 1.0e2, 1.0e3, 1.0e4,
                               1.0e5, 1.0e6, 1.0e7, 1.0e8, 1.0e9,
                               1.0e10, 1.0e11, 1.0e12, 1.0e13, 1.0e14,
                               1.0e15, 1.0e16, 1.0e17, 1.0e18, 1.0e19,
                               1.0e20, 1.0e21, 1.0e22, 1.0e23 };
    if (n >= 0) {
        while (n > 23) {
            x *= 1.0e24;
            n -= 24;
        }
        return x*dpow10[n];
    } else {
        while (n < -23) {
            x /= 1.0e24;
            n += 24;
        }
        return x/dpow10[-n];
    }
}

/*
 * Number Printer
 *
 * This version implements Burger&Dybvig algorithm (Robert G. Burger
 * and and R. Kent Dybvig, "Priting Floating-Point Numbers Quickly and
 * Accurately", PLDI '96, pp.108--116, 1996).
 */

/* compare x+d and y.  x, d, y are exact positive integers.
   this is called in inner loops so we need to be fast. */
static inline int numcmp3(ScmObj x, ScmObj d, ScmObj y)
{
    if (SCM_INTP(x) && SCM_INTP(d) && SCM_INTP(y)) {
        long xd = SCM_INT_VALUE(x)+SCM_INT_VALUE(d);
        if (xd < SCM_INT_VALUE(y)) return -1;
        if (xd > SCM_INT_VALUE(y)) return 1;
        else return 0;
    } else {
        ScmObj bx = SCM_BIGNUMP(x)? x : Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
        ScmObj bd = SCM_BIGNUMP(d)? d : Scm_MakeBignumFromSI(SCM_INT_VALUE(d));
        ScmObj by = SCM_BIGNUMP(y)? y : Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
        return Scm_BignumCmp3U(SCM_BIGNUM(bx), SCM_BIGNUM(bd), SCM_BIGNUM(by));
    }
}

/* Increment the given decimal number represented as a string.

          /------------------ start
          |             /---- end
          v             v
   buf |- 3 . 1 4 1 5 9         |

   In this example, we add 1 to the 10000th ('9') and handle any carryovers.
   If the carry over spills on the left, we shift entire string to right.
   Returns NUL-terminated string.
 */
static char *notational_roundup(const char *src, int start, int end)
{
    char *r = SCM_NEW_ATOMIC2(char*, end+2);
    memcpy(r, src, end);
    r[end] = r[end+1] = '\0';

    /* loop as long as we carry over */
    char *s = r+start;
    char *p = r+end-1;
    for (; s <= p; p--) {
        if (*p == '.') continue;
        if (*p == '9') { *p = '0'; continue; }
        else { (*p)++; break; }
    }
    if (p < s && *s == '0') {
        /* spill over */
        for (p = r+end-1; s <= p; p--) p[1] = p[0];
        *s = '1';
    }
    return r;
}

/* Notational rounding
   Should we use commercial rounding (round half towards infinity)
   or banker's rounding?  Since notational rounding is inherently
   taking convenience than accuracy, we go with commercial rounding.
*/
static void notational_rounding(ScmDString *ds, int numstart, int precision)
{
    Scm_DStringWeld(ds);
    ScmSmallInt size;
    const char *cbuf = Scm_DStringPeek(ds, &size, NULL);
    const char *end = cbuf + size;
    const char *p = cbuf;
    while (p < end && *p != '.') p++;
    SCM_ASSERT(p < end);
    p++;                    /* p is on tenths */
    if (end-p > precision) {
        char c = *(p+precision);
        if (c < '5') {
            /* round down - we just truncate */
            Scm_DStringTruncate(ds, p+precision-cbuf);
        } else {
            /* round up */
            char *nbuf = notational_roundup(cbuf, numstart,
                                            p+precision-cbuf);
            Scm_DStringTruncate(ds, 0);
            Scm_DStringPutz(ds, nbuf, -1);
        }
    }
}

/* Increment the last digit and handle all carryovers.
   If we always emit highest precision, Burger&Dybvig algorithm strictly
   emits digits forward, without backing up to handle carry over.
   However, if we round the number before it, there can be a case that
   the last digit gets rounded up and its carry-over propagates upward.
   In such a case, this is called.  The last digit in DS is '9', and we
   know we need to round it up.
 */
static void spill_fixup(ScmDString *ds, int numstart)
{
    ScmSmallInt size;
    const char *cbuf = Scm_DStringPeek(ds, &size, NULL);
    char *nbuf = notational_roundup(cbuf, numstart, size);
    Scm_DStringTruncate(ds, 0);
    Scm_DStringPutz(ds, nbuf, -1);
}

/* The main routine to get string representation of double.
   Convert VAL to a string and store to BUF, which must have at least FLT_BUF
   bytes long.
   True for PLUS_SIGN forces adding '+' for nonnegative numbers.
   EXP_LO and EXP_HI control when to switch exponential representation.
   We use n.nnne+zz representation when zz can be smaller than or equal
   to EXP_LO, or greater than or equal to EXP_HI.
   PRECISION specifies the number of digits to be printed after
   the decimal point; -1 means no limit.
   NOTATIONAL flags changes how the rounding with precision is done; if it's
   false, we choose closest decimal to the actual number represented by VAL.
   If it's true, we first generate optimal decimal notation, then round.
   */
static void print_double(ScmDString *ds, double val, int plus_sign,
                         int precision, int notational,
                         int exp_lo, int exp_hi, int exp_width)
{
    /* Handle a few special cases first. */
    if (val == 0.0) {
        if (Scm_FlonumSign(val) > 0) {
            if (plus_sign) SCM_DSTRING_PUTC(ds, '+');
        } else {
            SCM_DSTRING_PUTC(ds, '-');
        }
        if (precision < 0) Scm_DStringPutz(ds, "0.0", 3);
        else {
            Scm_DStringPutz(ds, "0.", 2);
            for (int i=0; i<precision; i++) {
                Scm_DStringPutc(ds, '0');
            }
        }
        return;
    } else if (SCM_IS_INF(val)) {
        if (val < 0.0) Scm_DStringPutz(ds, "-inf.0", 6);
        else Scm_DStringPutz(ds, "+inf.0", 6);
        return;
    } else if (SCM_IS_NAN(val)) {
        Scm_DStringPutz(ds, "+nan.0", 6);
        return;
    }

    if (val < 0.0) SCM_DSTRING_PUTC(ds, '-');
    else if (plus_sign) SCM_DSTRING_PUTC(ds, '+');

    int numstart = Scm_DStringSize(ds); /* remember this for notational rounding */

    /* variable names follows Burger&Dybvig paper. mp, mm for m+, m-.
       note that m+ == m- for most cases, and m+ == 2*m- for the rest.
       so we calculate m+ from m- for each iteration, using the flag
       mp2 as   m+ = mp? m- : 2*m-. */
    ScmObj r, s, mm;
    int exp, sign;
    int mp2 = FALSE, fixup = FALSE;
    int fracdigs = -1;   /* Count digits below the decimal point.
                            Initial value is -1.  Once we emit the decimal
                            point it becomes 0, then we start counting. */

    IEXPT10_INIT();
    if (val < 0) val = -val;

    /* initialize r, s, m+ and m- */
    ScmObj f = Scm_DecodeFlonum(val, &exp, &sign);
    int round = !Scm_OddP(f);
    if (exp >= 0) {
        ScmObj be = Scm_Ash(SCM_MAKE_INT(1), exp);
        if (Scm_NumCmp(f, SCM_2_52) != 0) {
            r = Scm_Ash(f, exp+1);
            s = SCM_MAKE_INT(2);
            mp2= FALSE;
            mm = be;
        } else {
            r = Scm_Ash(f, exp+2);
            s = SCM_MAKE_INT(4);
            mp2 = TRUE;
            mm = be;
        }
    } else {
        if (exp == -1023 || Scm_NumCmp(f, SCM_2_52) != 0) {
            r = Scm_Ash(f, 1);
            s = Scm_Ash(SCM_MAKE_INT(1), -exp+1);
            mp2 = FALSE;
            mm = SCM_MAKE_INT(1);
        } else {
            r = Scm_Ash(f, 2);
            s = Scm_Ash(SCM_MAKE_INT(1), -exp+2);
            mp2 = TRUE;
            mm = SCM_MAKE_INT(1);
        }
    }

    /* estimate scale */
    int est = (int)ceil(log10(val) - 0.1);
    if (est >= 0) {
        s = Scm_Mul(s, iexpt10(est));
    } else {
        ScmObj scale = iexpt10(-est);
        r =  Scm_Mul(r, scale);
        mm = Scm_Mul(mm, scale);
    }

    /* fixup.  avoid calculating m+ for obvious case. */
    if (Scm_NumCmp(r, s) >= 0) {
        fixup = TRUE;
    } else {
        ScmObj mp = (mp2? Scm_Ash(mm, 1) : mm);
        if (round) {
            fixup = (numcmp3(r, mp, s) >= 0);
        } else {
            fixup = (numcmp3(r, mp, s) > 0);
        }
    }
    if (fixup) {
        s = Scm_Mul(s, SCM_MAKE_INT(10));
        est++;
    }

    /* Scm_Printf(SCM_CURERR, "est=%d, r=%S, s=%S, mp=%S, mm=%S\n",
       est, r, s, mp, mm); */

    /* Determine position of decimal point.  we avoid exponential
       notation if exponent is small, i.e. 0.9 and 30.0 instead of
       9.0e-1 and 3.0e1.  */
    int point;
    if (est < exp_hi && est > exp_lo) { point = est; est = 1; }
    else { point = 1; }

    /* Now, we print XX.YYeZZ, where XX.YY is VAL*10^EST and
       ZZ is EST.  If EST == 1 we omit exponent part.  POINT is
       the number of digits in XX part; so POINT=1 for 1.23,
       POINT=2 for 12.3 and so on. */

    /* If POINT <= 0, we need to emit preceding zeros. */
    if (point <= 0) {
        for (int digs=point; digs<1 ;digs++) {
            /* check the number of digits to be printed */
            if (!notational && precision >= 0 && fracdigs >= precision-1) {
                if (digs == 0) {
                    /* whether r/s is greater than 1/2 or not defines the
                       first number of significant digits */
                    ScmObj r2 = Scm_Ash(r, 1);
                    if (Scm_NumCmp(r2, s) > 0) SCM_DSTRING_PUTC(ds, '1');
                    else                       SCM_DSTRING_PUTC(ds, '0');
                } else {
                    SCM_DSTRING_PUTC(ds, '0');
                }
                if (digs == point) SCM_DSTRING_PUTC(ds, '.');
                /* no more digits. */
                goto show_exponent;
            }
            SCM_DSTRING_PUTC(ds, '0');
            if (digs == point) SCM_DSTRING_PUTC(ds, '.');
            fracdigs++;
        }
    }

    /* generate the digits */
    int spilled = FALSE;
    int digs;
    for (digs=1; ; digs++) {
        ScmObj r10 = Scm_Mul(r, SCM_MAKE_INT(10));
        ScmObj q = Scm_Quotient(r10, s, &r);
        ScmObj mp;

        /* if we round early, we should extend the boundary
           _one digit before the rounded digit_.  It's a bit complicated
           since when precision == 0 we have to do it before printing
           decimal point. */
        if (!notational
            && ((precision == 0 && digs == point)
                || (precision > 0 && fracdigs >= precision-1))) {
            mm = mp = Scm_Ash(s, -1);
        } else {
            mm = Scm_Mul(mm, SCM_MAKE_INT(10));
            mp = (mp2? Scm_Ash(mm, 1) : mm);
        }

        /*Scm_Printf(SCM_CURERR, "q=%S, r=%S, s=%S mp=%S, mm=%S\n",
          q, r, s, mp, mm);*/

        SCM_ASSERT(SCM_INTP(q));
        int tc1, tc2;
        if (round) {
            tc1 = (Scm_NumCmp(r, mm) <= 0);
            tc2 = (numcmp3(r, mp, s) >= 0);
        } else {
            tc1 = (Scm_NumCmp(r, mm) < 0);
            tc2 = (numcmp3(r, mp, s) > 0);
        }
        if (!tc1) {
            if (!tc2) {
                ScmChar c = (char)SCM_INT_VALUE(q) + '0';
                SCM_DSTRING_PUTC(ds, c);
                if (digs == point) SCM_DSTRING_PUTC(ds, '.');
                if (digs >= point) fracdigs++;
                continue;
            } else {
                ScmChar c = (char)SCM_INT_VALUE(q) + '1';
                if (c > '9') { spilled = TRUE; c = '9'; }
                SCM_DSTRING_PUTC(ds, c);
                if (digs > point) fracdigs++;
                break;
            }
        } else {
            if (!tc2) {
                SCM_ASSERT(SCM_INTP(q));
                ScmChar c = (char)SCM_INT_VALUE(q) + '0';
                SCM_DSTRING_PUTC(ds, c);
                if (digs > point) fracdigs++;
                break;
            } else {
                int tc3 = numcmp3(r, r, s); /* r*2 <=> s */
                if ((round && tc3 <= 0) || (!round && tc3 < 0)) {
                    ScmChar c = (char)SCM_INT_VALUE(q) + '0';
                    SCM_DSTRING_PUTC(ds, c);
                    if (digs > point) fracdigs++;
                    break;
                } else {
                    ScmChar c = (char)SCM_INT_VALUE(q) + '1';
                    if (c > '9') { spilled = TRUE; c = '9'; }
                    SCM_DSTRING_PUTC(ds, c);
                    if (digs > point) fracdigs++;
                    break;
                }
            }
        }
    }
    if (spilled) spill_fixup(ds, numstart);

    /* Notational rounding, if necessary */
    if (notational && precision >= 0 && fracdigs > precision) {
        notational_rounding(ds, numstart, precision);
    }

    /* print the trailing zeros if necessary */
    if (digs <= point) {
        for (;digs < point; digs++) SCM_DSTRING_PUTC(ds, '0');
        SCM_DSTRING_PUTC(ds, '.');
        if (precision < 0) SCM_DSTRING_PUTC(ds, '0');
    }
    for (;(digs-point) < precision; digs++) {
        SCM_DSTRING_PUTC(ds, '0');
    }

 show_exponent:
    SCM_ASSERT(est < 1000 && est > -1000);
    /* prints exponent.  we shifted decimal point, so -1. */
    est--;
    if (est != 0) {
        SCM_DSTRING_PUTC(ds, 'e');
        if (est < 0) {
            Scm_DStringPutc(ds, '-');
            est = -est;
        }
        char zbuf[5]; /* we know est is at most 4 digits */
        int echars = sprintf(zbuf, "%d", (int)est);
        if (echars < exp_width) {
            int fill = exp_width - echars;
            while (fill--) {
                Scm_DStringPutc(ds, '0');
            }
        }
        Scm_DStringPutz(ds, zbuf, -1);
    }
}

static void number_print(ScmObj obj, ScmPort *port,
                         ScmWriteContext *ctx SCM_UNUSED)
{
    Scm_PrintNumber(port, obj, NULL);
}

#define FLT_BUF 65  /* need to hold binary representation of the least fixnum */

static size_t
print_radix_prefix(ScmPort *port, u_long radix)
{
    char buf[FLT_BUF];
    switch (radix) {
    case 2:  Scm_Putz("#b", 2, port); return 2;
    case 8:  Scm_Putz("#o", 2, port); return 2;
    case 10: Scm_Putz("#d", 2, port); return 2;
    case 16: Scm_Putz("#x", 2, port); return 2;
    default:
        {
            int nc = snprintf(buf, sizeof(buf), "#%lur", radix);
            Scm_Putz(buf, nc, port);
            return nc;
        }
    }
}

static size_t
print_number(ScmPort *port, ScmObj obj, u_long flags, ScmNumberFormat *fmt)
{
    int use_upper = flags & SCM_NUMBER_FORMAT_USE_UPPER;
    int show_plus = flags & SCM_NUMBER_FORMAT_SHOW_PLUS;
    int radix = fmt->radix;
    int nchars = 0;
    char buf[FLT_BUF];

    if ((flags & SCM_NUMBER_FORMAT_ALT_RADIX) && SCM_EXACTP(obj)) {
        nchars += print_radix_prefix(port, radix);
    }

    if (SCM_INTP(obj)) {
        long value = SCM_INT_VALUE(obj);
        if (value < 0) {
            SCM_PUTC('-', port);
            nchars++;
            value = -value;     /* this won't overflow */
        } else if (show_plus) {
            SCM_PUTC('+', port);
            nchars++;
        }
        if (value == 0) { SCM_PUTC('0', port); return nchars+1; }
        int i;
        for (i = FLT_BUF-1; i >= 0 && value > 0; i--) {
            int c = value % radix;
            buf[i] = (c<10)?(c+'0'):(use_upper?(c-10+'A'):(c-10+'a'));
            value /= radix;
            nchars++;
        }
        Scm_Putz(buf+i+1, FLT_BUF-i-1, port);
        return nchars;
    } else if (SCM_BIGNUMP(obj)) {
        ScmObj s = Scm_BignumToString(SCM_BIGNUM(obj), radix, use_upper);
        if(show_plus && Scm_Sign(obj) >= 0) {
            Scm_Putc('+', port);
            nchars++;
        }
        Scm_Puts(SCM_STRING(s), port);
        return nchars + SCM_STRING_BODY_LENGTH(SCM_STRING_BODY(s));
    } else if (SCM_FLONUMP(obj)) {
        ScmDString ds;
        Scm_DStringInit(&ds);
        print_double(&ds, SCM_FLONUM_VALUE(obj), show_plus,
                     fmt->precision,
                     fmt->flags&SCM_NUMBER_FORMAT_ROUND_NOTATIONAL,
                     fmt->exp_lo, fmt->exp_hi, fmt->exp_width);
        Scm_Putz(Scm_DStringGetz(&ds), -1, port);
        return Scm_DStringSize(&ds);
    } else if (SCM_RATNUMP(obj)) {
        u_long flags2 = flags & ~SCM_NUMBER_FORMAT_ALT_RADIX;
        nchars = print_number(port, SCM_RATNUM_NUMER(obj), flags2, fmt);
        Scm_Putc('/', port);
        nchars++;
        flags2 &= ~SCM_NUMBER_FORMAT_SHOW_PLUS;
        nchars += print_number(port, SCM_RATNUM_DENOM(obj), flags2, fmt);
        return nchars;
    } else if (SCM_COMPNUMP(obj)) {
        ScmDString ds;
        Scm_DStringInit(&ds);
        print_double(&ds, SCM_COMPNUM_REAL(obj), show_plus,
                     fmt->precision,
                     fmt->flags&SCM_NUMBER_FORMAT_ROUND_NOTATIONAL,
                     fmt->exp_lo, fmt->exp_hi, fmt->exp_width);
        Scm_Putz(Scm_DStringGetz(&ds), -1, port);
        nchars += Scm_DStringSize(&ds);
        Scm_DStringTruncate(&ds, 0);
        print_double(&ds, SCM_COMPNUM_IMAG(obj), TRUE,
                     fmt->precision,
                     fmt->flags&SCM_NUMBER_FORMAT_ROUND_NOTATIONAL,
                     fmt->exp_lo, fmt->exp_hi, fmt->exp_width);
        Scm_Putz(Scm_DStringGetz(&ds), -1, port);
        nchars += Scm_DStringSize(&ds);
        Scm_Putc('i', port);
        return nchars+1;
    } else {
        Scm_Error("number required: %S", obj);
        return 0;               /* dummy */
    }
}

/* API */
void Scm_NumberFormatInit(ScmNumberFormat* fmt)
{
    fmt->flags = 0;
    fmt->radix = 10;
    fmt->precision = -1;
    fmt->exp_lo = -3;
    fmt->exp_hi = 10;
    fmt->exp_width = 0;
}

/* API */
ScmObj Scm_NumberToString(ScmObj obj, int radix, u_long flags)
{
    if (radix < SCM_RADIX_MIN || radix > SCM_RADIX_MAX)
        Scm_Error("radix out of range: %d", radix);
    ScmPort *p = SCM_PORT(Scm_MakeOutputStringPort(TRUE));
    ScmNumberFormat fmt;
    Scm_NumberFormatInit(&fmt);
    fmt.flags = flags;
    fmt.radix = radix;
    Scm_PrintNumber(p, obj, &fmt);
    return Scm_GetOutputString(p, 0);
}

/* API.  FMT can be NULL. */
size_t Scm_PrintNumber(ScmPort *port, ScmObj n, ScmNumberFormat *fmt)
{
    ScmNumberFormat defaults;
    if (fmt == NULL) {
        Scm_NumberFormatInit(&defaults);
        fmt = &defaults;
    }
    return print_number(port, n, fmt->flags, fmt);
}

/* API.  FMT can be NULL.  Utility to expose Burger&Dybvig algorithm. */
size_t Scm_PrintDouble(ScmPort *port, double d, ScmNumberFormat *fmt)
{
    ScmNumberFormat defaults;
    if (fmt == NULL) {
        Scm_NumberFormatInit(&defaults);
        fmt = &defaults;
    }
    ScmDString ds;
    Scm_DStringInit(&ds);
    print_double(&ds, d,
                 fmt->flags & SCM_NUMBER_FORMAT_SHOW_PLUS,
                 fmt->precision,
                 fmt->flags & SCM_NUMBER_FORMAT_ROUND_NOTATIONAL,
                 fmt->exp_lo, fmt->exp_hi, fmt->exp_width);
    size_t nchars = Scm_DStringSize(&ds);
    Scm_Putz(Scm_DStringGetz(&ds), (int)nchars, port);
    return nchars;
}

/*
 * Number Parser
 *
 *  <number> : <prefix> <complex>
 *  <prefix> : <radix> <exactness> | <exactness> <radix>
 *  <radix>  : <empty> | '#b' | '#o' | '#d' | '#x'
 *  <exactness> : <empty> | '#e' | '#i'
 *  <complex> : <real>
 *            | <real> '@' <real> ['p' 'i']
 *            | <real> '+' <ureal> 'i'
 *            | <real> '-' <ureal> 'i'
 *            | <real> '+' 'i'
 *            | <real> '-' 'i'
 *            | '+' <ureal> 'i'
 *            | '-' <ureal> 'i'
 *            | '+' 'i'
 *            | '-' 'i'
 *  <real>   : <sign> <ureal>
 *  <sign>   : <empty> | '+' | '-'
 *  <ureal>  : <uinteger>
 *           | <uinteger> '/' <uinteger>
 *           | <decimal>
 *  <uinteger> : <digit>+ '#'*
 *  <decimal> : <digit10>+ '#'* <suffix>
 *            | '.' <digit10>+ '#'* <suffix>
 *            | <digit10>+ '.' <digit10>+ '#'* <suffix>
 *            | <digit10>+ '#'+ '.' '#'* <suffix>
 *  <suffix>  : <empty> | <exponent-marker> <sign> <digit10>+
 *  <exponent-marker> : 'e' | 's' | 'f' | 'd' | 'l'
 *
 * The parser reads characters from on-memory buffer.
 * Multibyte strings are filtered out in the early stage of
 * parsing, so the subroutines assume the buffer contains
 * only ASCII chars.
 */

struct numread_packet {
    const char *buffer;         /* original buffer */
    int buflen;                 /* original length */
    int radix;                  /* radix (can be overridden by prefix) */
    int noradixprefix;          /* do not allow radix prefix */
    int exactness;              /* exactness; see enum below */
    int padread;                /* '#' padding has been read */
    int explicit;               /* explicit prefix is appeared */
    int strict;                 /* reject gauche extension */
    int throwerror;             /* throws error on parse, instead of
                                   returning #f. */
};

enum { /* used in the exactness flag */
    NOEXACT, EXACT, INEXACT
};

/* Max digits D such that all D-digit radix R integers fit in signed
   long, i.e. R^(D+1)-1 <= LONG_MAX */
static long longdigs[SCM_RADIX_MAX-SCM_RADIX_MIN+1] = { 0 };

/* Max integer I such that reading next digit (in radix R) will overflow
   long integer.   floor(LONG_MAX/R - R). */
static u_long longlimit[SCM_RADIX_MAX-SCM_RADIX_MIN+1] = { 0 };

/* An integer table of R^D, which is a "big digit" to be added
   into bignum. */
static u_long bigdig[SCM_RADIX_MAX-SCM_RADIX_MIN+1] = { 0 };

static ScmObj numread_error(const char *msg, struct numread_packet *ctx);

/* Returns either small integer or bignum.
   initval may be a Scheme integer that will be 'concatenated' before
   the integer to be read; it is used to read floating-point number.
   Note that value_big may keep denormalized bignum. */
static ScmObj read_uint(const char **strp, int *lenp,
                        struct numread_packet *ctx,
                        ScmObj initval)
{
    const char *str = *strp;
    int digread = FALSE;
    int len = *lenp;
    int radix = ctx->radix;
    int digits = 0, diglimit = longdigs[radix-SCM_RADIX_MIN];
    u_long limit = longlimit[radix-SCM_RADIX_MIN], bdig = bigdig[radix-SCM_RADIX_MIN];
    u_long value_int = 0;
    ScmBignum *value_big = NULL;
    static const char tab[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (!SCM_FALSEP(initval)) {
        if (SCM_INTP(initval)) {
            if ((u_long)SCM_INT_VALUE(initval) > limit) {
                value_big = Scm_MakeBignumWithSize(4, SCM_INT_VALUE(initval));
            } else {
                value_int = SCM_INT_VALUE(initval);
            }
        } else if (SCM_BIGNUMP(initval)) {
            value_big = SCM_BIGNUM(Scm_BignumCopy(SCM_BIGNUM(initval)));
        }
        digread = TRUE;
    } else if (*str == '0') {
        /* Ignore leading 0's, to avoid unnecessary bignum operations. */
        while (len > 0 && *str == '0') { str++; len--; }
        digread = TRUE;
    }

    while (len--) {
        int digval = -1;
        char c = tolower(*str++);
        if (ctx->explicit && !ctx->strict && c == '_') {
            /* Gauche extension - allow '_' in digits for readability
               when number is expliticly prefixed. */
            continue;
        }
        if (ctx->padread) {
            if (c == '#') digval = 0;
            else break;
        } else if (digread && c == '#') {
            digval = 0;
            ctx->padread = TRUE;
            if (ctx->exactness == NOEXACT) {
                ctx->exactness = INEXACT;
            }
        } else {
            for (const char *ptab = tab; ptab < tab+radix; ptab++) {
                if (c == *ptab) {
                    digval = (int)(ptab-tab);
                    digread = TRUE;
                    break;
                }
            }
        }
        if (digval < 0) break;
        value_int = value_int * radix + digval;
        digits++;
        if (value_big == NULL) {
            if (value_int >= limit) {
                value_big = Scm_MakeBignumWithSize(4, value_int);
                value_int = digits = 0;
            }
        } else if (digits > diglimit) {
            value_big = Scm_BignumAccMultAddUI(value_big, bdig, value_int);
            value_int = digits = 0;
        }
    }
    *strp = str-1;
    *lenp = len+1;

    if (value_big == NULL) return Scm_MakeInteger(value_int);
    if (digits > 0) {
        value_big = Scm_BignumAccMultAddUI(value_big,
                                           ipow(radix, digits),
                                           value_int);
    }
    return Scm_NormalizeBignum(SCM_BIGNUM(value_big));
}

/*
 * Find a double number closest to f * 10^e, using z as the starting
 * approximation.  The algorithm (and its name) is taken from Will Clinger's
 * paper "How to Read Floating Point Numbers Accurately", in the ACM
 * SIGPLAN '90, pp.92--101.
 * The algorithm is modified to take advantage of coherency between loops.
 */
static double algorithmR(ScmObj f, int e, double z)
{
    int k, s;
    ScmObj m = Scm_DecodeFlonum(z, &k, &s);
    ScmObj x, y;
    IEXPT10_INIT();
  retry:
    if (k >= 0) {
        if (e >= 0) {
            x = Scm_Mul(f, iexpt10(e));
            y = Scm_Ash(m, k);
        } else {
            x = f;
            y = Scm_Ash(Scm_Mul(m, iexpt10(-e)), k);
        }
    } else {
        if (e >= 0) {
            x = Scm_Ash(Scm_Mul(f, iexpt10(e)), -k);
            y = m;
        } else {
            x = Scm_Ash(f, -k);
            y = Scm_Mul(m, iexpt10(-e));
        }
    }
    int kprev = k;

    /* compare  */
    for (;;) {
        /*Scm_Printf(SCM_CURERR, "z=%.20lg,\nx=%S,\ny=%S\nf=%S\nm=%S\ne=%d, k=%d\n", z, x, y, f, m, e, k);*/
        /* compare */
        int sign_d = Scm_NumCmp(x, y);
        ScmObj abs_d = (sign_d > 0)? Scm_Sub(x, y) : Scm_Sub(y, x);
        ScmObj d2 = Scm_Ash(Scm_Mul(m, abs_d), 1);
        switch (Scm_NumCmp(d2, y)) {
        case -1: /* d2 < y */
            if (Scm_NumCmp(m, SCM_2_52) == 0
                && sign_d < 0
                && k > -1074
                && Scm_NumCmp(Scm_Ash(d2, 1), y) > 0) {
                goto prevfloat;
            } else {
                return ldexp(Scm_GetDouble(m), k);
            }
        case 0: /* d2 == y */
            if (!Scm_OddP(m)) {
                if (Scm_NumCmp(m, SCM_2_52) == 0
                    && sign_d < 0) {
                    goto prevfloat;
                } else {
                    return ldexp(Scm_GetDouble(m), k);
                }
            } else if (sign_d < 0) {
                goto prevfloat;
            } else {
                goto nextfloat;
            }
        default:
            if (sign_d < 0) goto prevfloat;
            else            goto nextfloat;
        }
      prevfloat:
        m = Scm_Sub(m, SCM_MAKE_INT(1));
        if (k > -1074 && Scm_NumCmp(m, SCM_2_52) < 0) {
            m = Scm_Ash(m, 1);
            k--;
        }
        goto next;
      nextfloat:
        m = Scm_Add(m, SCM_MAKE_INT(1));
        if (Scm_NumCmp(m, SCM_2_53) >= 0) {
            m = Scm_Ash(m, -1);
            k++;
        }
        /*FALLTHROUGH*/
      next:
        if (kprev >= 0) {
            if (k >= 0) {
                /* k stays positive. x is invariant */
                if (e >= 0) {
                    y = Scm_Ash(m, k);
                } else {
                    y = Scm_Ash(Scm_Mul(m, iexpt10(-e)), k);
                }
            } else {
                /* k turned to negative */
                goto retry;
            }
        } else {
            if (k < 0) {
                /* k stays negative. */
                if (e >= 0) {
                    if (k != kprev) x = Scm_Ash(Scm_Mul(f, iexpt10(e)), -k);
                    y = m;
                } else {
                    if (k != kprev) x = Scm_Ash(f, -k);
                    y = Scm_Mul(m, iexpt10(-e));
                }
            } else {
                /* k turned to positive */
                goto retry;
            }
        }
    }
    /*NOTREACHED*/
}

static ScmObj read_real(const char **strp, int *lenp,
                        struct numread_packet *ctx)
{
    int minusp = FALSE, exp_minusp = FALSE, exp_overflow = FALSE;
    int sign_seen = FALSE;
    int fracdigs = 0;
    long exponent = 0;
    ScmObj intpart, fraction;
    const char *mark;           /* will point (*strp)[1] if there's a sign,
                                   otherwise (*strp)[0], to check if the
                                   given str has valid content. */

    switch (**strp) {
    case '-': minusp = TRUE;
        /* FALLTHROUGH */
    case '+':
        (*strp)++; (*lenp)--; sign_seen = TRUE;
    }
    if ((*lenp) <= 0) return SCM_FALSE;
    mark = *strp;

    /* Recognize specials */
    if (sign_seen && (*lenp) >= 5) {
        if (strncasecmp(*strp, "inf.0", 5) == 0) {
            (*strp) += 5; (*lenp) -= 5;
            return minusp?SCM_NEGATIVE_INFINITY:SCM_POSITIVE_INFINITY;
        }
        if (strncasecmp(*strp, "nan.0", 5) == 0) {
            (*strp) += 5; (*lenp) -= 5;
            return SCM_NAN;
        }
    }

    /* Read integral part */
    if (**strp != '.') {
        intpart = read_uint(strp, lenp, ctx, SCM_FALSE);
        if ((*lenp) <= 0) {
            if (minusp) intpart = Scm_Negate(intpart);
            if (ctx->exactness == INEXACT) {
                return Scm_Inexact(intpart);
            } else {
                return intpart;
            }
        }
        if (**strp == '/') {
            /* possibly rational */
            ScmObj denom;
            int lensave;

            if ((*lenp) <= 1 || mark == *strp) return SCM_FALSE;
            (*strp)++; (*lenp)--;
            lensave = *lenp;
            denom = read_uint(strp, lenp, ctx, SCM_FALSE);
            if (SCM_FALSEP(denom)) return SCM_FALSE;
            if (SCM_EXACT_ZERO_P(denom)) {
                if (lensave > *lenp) {
                    if (ctx->exactness != INEXACT) {
                        return numread_error("(exact infinity/nan is not supported.)",
                                             ctx);
                    }
                    if (SCM_EXACT_ZERO_P(intpart)) return SCM_NAN;
                    return minusp? SCM_NEGATIVE_INFINITY:SCM_POSITIVE_INFINITY;
                } else {
                    return SCM_FALSE;
                }
            }
            if (minusp) intpart = Scm_Negate(intpart);
            if (ctx->exactness == INEXACT) {
                return Scm_Inexact(Scm_Div(intpart, denom));
            } else {
                return Scm_MakeRational(intpart, denom);
            }
        }
        /* fallthrough */
    } else {
        intpart = SCM_FALSE; /* indicate there was no intpart */
    }

    /* Read fractional part.
       At this point, simple integer is already eliminated. */
    if (**strp == '.') {
        if (ctx->radix != 10) {
            return numread_error("(only 10-based fraction is supported)", ctx);
        }
        (*strp)++; (*lenp)--;
        int lensave = *lenp;
        fraction = read_uint(strp, lenp, ctx, intpart);
        fracdigs = lensave - *lenp;
    } else {
        fraction = intpart;
    }

    if (SCM_FALSEP(intpart)) {
        if (fracdigs == 0) return SCM_FALSE; /* input was "." */
    }
    if (mark == *strp) return SCM_FALSE;

    /* Read exponent.  */
    if (*lenp > 0 && strchr("eEsSfFdDlL", (int)**strp)) {
        (*strp)++;
        if (--(*lenp) <= 0) return SCM_FALSE;
        switch (**strp) {
        case '-': exp_minusp = TRUE;
            /*FALLTHROUGH*/
        case '+':
            (*strp)++;
            if (--(*lenp) <= 0) return SCM_FALSE;
        }
        while (*lenp > 0) {
            int c = **strp;
            if (!isdigit(c)) break;
            (*strp)++, (*lenp)--;
            if (isdigit(c) && !exp_overflow) {
                exponent = exponent * 10 + (c - '0');
                /* Check obviously wrong exponent range.  More subtle check
                   will be done later. */
                if (exponent >= MAX_EXPONENT) {
                    exp_overflow = TRUE;
                }
            }
        }
        if (exp_minusp) exponent = -exponent;
    }
    if (exp_overflow) {
        if (ctx->exactness == EXACT) {
            /* Although we can represent such a number using bignum and
               ratnum, such large (or small) exponent is highly unusual
               and we assume we can report implementation limitation
               violation. */
            return numread_error("(such an exact number is out of implementation limitation)",
                                 ctx);
        }
        if (exp_minusp || SCM_EQ(fraction, SCM_MAKE_INT(0))) {
            return Scm_MakeFlonum(minusp? -0.0:0.0);
        } else {
            return minusp? SCM_NEGATIVE_INFINITY : SCM_POSITIVE_INFINITY;
        }
    }

    /*Scm_Printf(SCM_CURERR, "fraction=%S, exponent=%d\n", fraction, exponent);*/

    /* Compose the number. */
    if (ctx->exactness == EXACT) {
        /* Explicit exact number.  We can continue exact arithmetic
           (it may end up ratnum) */
        ScmObj e = Scm_Mul(fraction,
                           Scm_ExactIntegerExpt(SCM_MAKE_INT(10),
                                                Scm_MakeInteger(exponent-fracdigs)));
        if (minusp) return Scm_Negate(e);
        else        return e;
    }

    /* Get double approximaiton of fraction.  If fraction >= 2^53 we'll
       only get approximation, but the error will be corrected in
       AlgorithmR.  We have to be careful, however, not to overflow
       the following GetDouble call. */
    int raise_factor = exponent - fracdigs;
    double realnum = Scm_GetDouble(fraction);

    if (SCM_IS_INF(realnum)) {
        /* We have too many digits to fit in double.  We can still get finite
           number if raise_factor is small, but we need to calculate realnum
           via rational. */
        if (raise_factor >= 0) {
            /* Exponent is too big. */
            return (minusp? SCM_NEGATIVE_INFINITY : SCM_POSITIVE_INFINITY);
        }
        IEXPT10_INIT();
        realnum = Scm_GetDouble(Scm_Div(fraction, iexpt10(-raise_factor)));
    } else {
        realnum = raise_pow10(realnum, raise_factor);
    }

    if (SCM_IS_INF(realnum)) {
        /* Exponent is too big. */
        return (minusp? SCM_NEGATIVE_INFINITY : SCM_POSITIVE_INFINITY);
    }

    if (realnum > 0.0
        && (Scm_NumCmp(fraction, SCM_2_52) > 0
            || raise_factor > MAX_EXACT_10_EXP
            || raise_factor < -MAX_EXACT_10_EXP)) {
        realnum = algorithmR(fraction, raise_factor, realnum);
    }
    if (minusp) realnum = -realnum;
    return Scm_MakeFlonum(realnum);
}

/* Entry point */
static ScmObj read_number(struct numread_packet *ctx)
{
    int radix_seen = 0, exactness_seen = 0, sign_seen = 0;

    const char *str = ctx->buffer;
    int len = ctx->buflen;

#define CHK_EXACT_COMPLEX()                                                 \
    do {                                                                    \
        if (ctx->exactness == EXACT) {                                      \
            return numread_error("(exact complex number is not supported)", \
                                 ctx);                                      \
        }                                                                   \
    } while (0)

    /* check suggested radix. */
    if (ctx->radix < SCM_RADIX_MIN || ctx->radix > SCM_RADIX_MAX) {
        return SCM_FALSE;
    }

    /* start from prefix part */
    while (len >= 0) {
        if (*str != '#') break;
        str++;
        switch (*str++) {
        case 'x':; case 'X':;
            if (ctx->noradixprefix || radix_seen) return SCM_FALSE;
            ctx->radix = 16; radix_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case 'o':; case 'O':;
            if (ctx->noradixprefix || radix_seen) return SCM_FALSE;
            ctx->radix = 8; radix_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case 'b':; case 'B':;
            if (ctx->noradixprefix || radix_seen) return SCM_FALSE;
            ctx->radix = 2; radix_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case 'd':; case 'D':;
            if (ctx->noradixprefix || radix_seen) return SCM_FALSE;
            ctx->radix = 10; radix_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case 'e':; case 'E':;
            if (exactness_seen) return SCM_FALSE;
            ctx->exactness = EXACT; exactness_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case 'i':; case 'I':;
            if (exactness_seen) return SCM_FALSE;
            ctx->exactness = INEXACT; exactness_seen++;
            ctx->explicit = TRUE;
            len -= 2;
            continue;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            if (ctx->noradixprefix || ctx->strict || radix_seen) return SCM_FALSE;
            else {
                ScmSize nread = 0;
                long radix = Scm_ParseDigitsAsLong(--str, --len, 10, &nread);
                if (radix < SCM_RADIX_MIN || radix > SCM_RADIX_MAX) return SCM_FALSE;
                str += nread; len -= nread;
                if (len <= 0) return SCM_FALSE;
                if (*str != 'r' && *str != 'R') return SCM_FALSE;
                str++; len--;
                ctx->radix = radix; radix_seen++;
                ctx->explicit = TRUE;
                continue;
            }
        }
        return SCM_FALSE;
    }
    if (len <= 0) return SCM_FALSE;

    /* number body.  need to check the special case of pure imaginary,
       and also eliminates some confusing cases. */
    if (*str == '+' || *str == '-') {
        if (len == 1) return SCM_FALSE;
        if (len == 2 && (str[1] == 'i' || str[1] == 'I')) {
            CHK_EXACT_COMPLEX();
            return Scm_MakeComplex(0.0, (*str == '+')? 1.0 : -1.0);
        }
        sign_seen = TRUE;
    }

    ScmObj realpart = read_real(&str, &len, ctx);
    if (SCM_FALSEP(realpart) || len == 0) return realpart;

    switch (*str) {
    case '@':
        /* polar representation of complex*/
        if (len <= 1) {
            return SCM_FALSE;
        } else {
            str++; len--;
            ScmObj angle = read_real(&str, &len, ctx);
            if (SCM_FALSEP(angle)) return SCM_FALSE;
            /* Gauche extension: X@Ypi */
            int pi_angle = (len == 2 && str[0] == 'p' && str[1] == 'i');
            if (!pi_angle && len != 0) return SCM_FALSE;
            CHK_EXACT_COMPLEX();
            double dmag = Scm_GetDouble(realpart);
            double dangle = Scm_GetDouble(angle);
            return (pi_angle
                    ? Scm_MakeComplexPolarPI(dmag, dangle)
                    : Scm_MakeComplexPolar(dmag, dangle));
        }
    case '+':
    case '-':
        /* rectangular representation of complex */
        if (len <= 1) {
            return SCM_FALSE;
        } else if (len == 2 && (str[1] == 'i' || str[1] == 'I')) {
            return Scm_MakeComplex(Scm_GetDouble(realpart),
                                   (*str == '+' ? 1.0 : -1.0));
        } else {
            ScmObj imagpart = read_real(&str, &len, ctx);
            if (SCM_FALSEP(imagpart) || len != 1
                || (*str != 'i' && *str != 'I')) {
                return SCM_FALSE;
            }
            CHK_EXACT_COMPLEX();
            if (Scm_Sign(imagpart) == 0) return realpart;
            return Scm_MakeComplex(Scm_GetDouble(realpart),
                                   Scm_GetDouble(imagpart));
        }
    case 'i':
    case 'I':
        /* '+' <ureal> 'i'  or '-' <ureal> 'i' */
        if (!sign_seen || len != 1) return SCM_FALSE;
        CHK_EXACT_COMPLEX();
        if (Scm_Sign(realpart) == 0) return Scm_MakeFlonum(0.0);
        else return Scm_MakeComplex(0.0, Scm_GetDouble(realpart));
    default:
        return SCM_FALSE;
    }
}

static ScmObj numread_error(const char *msg, struct numread_packet *ctx)
{
    if (ctx->throwerror) {
        Scm_Error("bad number format %s: %A", msg,
                  Scm_MakeString(ctx->buffer, ctx->buflen,
                                 ctx->buflen, 0));
    }
    return SCM_FALSE;
}

/* FLAGS is enum  ScmNumberFormatFlags (see number.h).  Only some of the
   flags are recognized for printing numbers. */
ScmObj Scm_StringToNumber(ScmString *str, int radix, u_long flags)
{
    ScmSmallInt len, size;
    const char *p = Scm_GetStringContent(str, &size, &len, NULL);
    if (size != len) {
        /* This can't be a proper number. */
        return SCM_FALSE;
    } else {
        struct numread_packet ctx;
        ctx.buffer = p;
        ctx.buflen = size;
        ctx.exactness = ((flags&SCM_NUMBER_FORMAT_EXACT)
                         ? EXACT
                         : ((flags&SCM_NUMBER_FORMAT_INEXACT)
                            ? INEXACT
                            : NOEXACT));
        ctx.padread = FALSE;
        ctx.explicit = FALSE;
        ctx.strict = flags&SCM_NUMBER_FORMAT_STRICT_R7RS;
        ctx.throwerror = FALSE;
        ctx.radix = radix;
        ctx.noradixprefix = flags&SCM_NUMBER_FORMAT_ALT_RADIX;
        return read_number(&ctx);
    }
}

/*===============================================================
 * Initialization
 */

ScmObj Scm__ConstObjs[SCM_NUM_CONST_OBJS] = { SCM_FALSE };

void Scm__InitNumber(void)
{
    ScmModule *mod = Scm_GaucheModule();

    for (int radix = SCM_RADIX_MIN; radix <= SCM_RADIX_MAX; radix++) {
        longlimit[radix-SCM_RADIX_MIN] =
            (u_long)floor((double)LONG_MAX/radix - radix);
        /* Find max D where R^(D+1)-1 <= LONG_MAX */
        u_long n = 1;
        for (int i = 0; ; i++, n *= radix) {
            if (n >= (u_long)(LONG_MAX/radix)) {
                longdigs[radix-SCM_RADIX_MIN] = i-1;
                bigdig[radix-SCM_RADIX_MIN] = n;
                break;
            }
        }
    }

    SCM_2_63 = Scm_Ash(SCM_MAKE_INT(1), 63);
    SCM_2_64 = Scm_Ash(SCM_MAKE_INT(1), 64);
    SCM_2_64_MINUS_1 = Scm_Sub(SCM_2_64, SCM_MAKE_INT(1));
    SCM_2_52 = Scm_Ash(SCM_MAKE_INT(1), 52);
    SCM_2_53 = Scm_Ash(SCM_MAKE_INT(1), 53);
    SCM_MINUS_2_63 = Scm_Negate(SCM_2_63);
    SCM_2_32 = Scm_Ash(SCM_MAKE_INT(1), 32);
    SCM_2_31 = Scm_Ash(SCM_MAKE_INT(1), 31);
    SCM_MINUS_2_31 = Scm_Negate(SCM_2_31);
    SCM_MIN_DENORMALIZED_FLONUM_EXACT =
        Scm_Reciprocal(Scm_Ash(SCM_MAKE_INT(1), 1075));
    SCM_MAX_FINITE_FLONUM_EXACT = Scm_Add(Scm_Sub(Scm_Ash(SCM_MAKE_INT(1), 1024),
                                                  Scm_Ash(SCM_MAKE_INT(1), 971)),
                                          Scm_Sub(Scm_Ash(SCM_MAKE_INT(1), 970),
                                                  SCM_MAKE_INT(1)));

    SCM_POSITIVE_INFINITY = Scm_MakeFlonum(SCM_DBL_POSITIVE_INFINITY);
    SCM_NEGATIVE_INFINITY = Scm_MakeFlonum(SCM_DBL_NEGATIVE_INFINITY);
    SCM_NAN               = Scm_MakeFlonum(SCM_DBL_NAN);

    SCM_SIZE_T_MAX = Scm_MakeIntegerU64(SIZE_MAX);
    SCM_SSIZE_T_MAX = Scm_MakeInteger64(SSIZE_MAX);
    SCM_SSIZE_T_MIN = Scm_MakeInteger64(-(SSIZE_MAX-1));
    SCM_PTRDIFF_T_MAX = Scm_MakeInteger64(PTRDIFF_MAX);
    SCM_PTRDIFF_T_MIN = Scm_MakeInteger64(-(PTRDIFF_MAX-1));

    dexpt2_minus_52 = ldexp(1.0, -52);
    dexpt2_minus_53 = ldexp(1.0, -53);

    Scm_InitBuiltinGeneric(&generic_add, "object-+", mod);
    Scm_InitBuiltinGeneric(&generic_sub, "object--", mod);
    Scm_InitBuiltinGeneric(&generic_mul, "object-*", mod);
    Scm_InitBuiltinGeneric(&generic_div, "object-/", mod);

#ifdef DOUBLE_ARMENDIAN
    check_armendian();
#endif /*DOUBLE_ARMENDIAN*/

    (void)SCM_INTERNAL_MUTEX_INIT(default_endian_mutex);

#ifdef COUNT_FLONUM_ALLOC
    Scm_AddCleanupHandler(report_flonum_count, NULL);
#endif
}
