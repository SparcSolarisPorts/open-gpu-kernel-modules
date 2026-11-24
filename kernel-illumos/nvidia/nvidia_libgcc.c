/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Replacements for the libgcc helpers that gcc calls for __builtin_clzll()
 * and __builtin_ctzll() on CPUs without a count-leading/trailing-zeros
 * instruction (SPARC V9 has neither).  A kernel module cannot link libgcc,
 * so nv-kernel.o would otherwise be left with unresolved __clzdi2 and
 * __ctzdi2.
 *
 * Both are written as plain binary searches so that the compiler has no
 * idiom to turn back into a call to itself.  Like libgcc they take a 64-bit
 * argument; unlike libgcc they return 64 for a zero argument instead of
 * being undefined.
 */

int __clzdi2(unsigned long long x);
int __ctzdi2(unsigned long long x);

int
__clzdi2(unsigned long long x)
{
    int n = 0;

    if (x == 0)
        return 64;

    if ((x >> 32) == 0) { n += 32; x <<= 32; }
    if ((x >> 48) == 0) { n += 16; x <<= 16; }
    if ((x >> 56) == 0) { n += 8;  x <<= 8;  }
    if ((x >> 60) == 0) { n += 4;  x <<= 4;  }
    if ((x >> 62) == 0) { n += 2;  x <<= 2;  }
    if ((x >> 63) == 0) { n += 1; }

    return n;
}

int
__ctzdi2(unsigned long long x)
{
    int n = 0;

    if (x == 0)
        return 64;

    if ((x & 0xffffffffULL) == 0) { n += 32; x >>= 32; }
    if ((x & 0xffffULL) == 0)     { n += 16; x >>= 16; }
    if ((x & 0xffULL) == 0)       { n += 8;  x >>= 8;  }
    if ((x & 0xfULL) == 0)        { n += 4;  x >>= 4;  }
    if ((x & 0x3ULL) == 0)        { n += 2;  x >>= 2;  }
    if ((x & 0x1ULL) == 0)        { n += 1; }

    return n;
}
