/*
 * Copyright (c) 2008-2025 Stefan Krah. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpalloc.h"
#include "mpdecimal.h"
#include "typearith.h"

#include <stdalign.h>

#include "sxml/memory.h"


#if defined(_MSC_VER)
  #pragma warning(disable : 4232)
#endif


/* Guaranteed minimum allocation for a coefficient. May be changed once
   at program start using mpd_setminalloc(). */
mpd_ssize_t MPD_MINALLOC = MPD_MINALLOC_MIN;

/*
 * sxml patch (D42): allocation through a scoped sxml_allocator.
 *
 * Upstream, the four functions below are process-global function pointers
 * defaulting to malloc, realloc, calloc and free. sxml must not have global
 * mutable state, and two item heaps on two threads must not share an
 * allocator (D35), so they are fixed functions that allocate through the
 * allocator of the current decimal scope.
 *
 * The decimal scope is the one thread-local variable of sxml. sxml's
 * decimal module (src/decimal.c) sets it on entry to every operation that
 * may allocate and restores the previous value before returning, so it
 * holds no state between library calls, nests, and is never visible to the
 * host. It lives here rather than in src/ so that libmpdec does not depend
 * on the sxml library (which links libmpdec).
 *
 * Every block starts with a header naming the allocator it came from and its
 * usable size, so mpd_free and mpd_reallocfunc work outside any scope and
 * with a different scope than the one that allocated. The header keeps the
 * alignment malloc guarantees, which libmpdec expects.
 */
#if defined(_MSC_VER) && !defined(__clang__)
  #define MPD_SXML_THREAD_LOCAL __declspec(thread)
#else
  #define MPD_SXML_THREAD_LOCAL _Thread_local
#endif

static MPD_SXML_THREAD_LOCAL const sxml_allocator *mpd_sxml_scope_allocator;

/* The strictest alignment of the scalar types: what malloc guarantees.
   Spelled out because MSVC's C headers do not define max_align_t. */
typedef union mpd_sxml_alignment {
    long double long_double_member;
    double double_member;
    uint64_t uint64_member;
    void *pointer_member;
} mpd_sxml_alignment;

typedef union mpd_sxml_block_header {
    struct {
        sxml_allocator allocator;
        size_t byte_count;
    } block;
    mpd_sxml_alignment alignment;
} mpd_sxml_block_header;

const struct sxml_allocator *
mpd_sxml_exchange_allocator_scope(const struct sxml_allocator *allocator)
{
    const sxml_allocator *previous = mpd_sxml_scope_allocator;
    mpd_sxml_scope_allocator = allocator;
    return previous;
}

/* Allocates size bytes through allocator; NULL on failure. The allocator's
   operations are called directly: libmpdec must not depend on the sxml
   library, so sxml_allocator_allocate is not available here. */
static void *
mpd_sxml_allocate(sxml_allocator allocator, size_t size)
{
    mpd_sxml_block_header *header;
    void *memory = NULL;

    if (allocator.operations == NULL ||
        size > SIZE_MAX - sizeof(mpd_sxml_block_header)) {
        return NULL;
    }
    if (allocator.operations->allocate(
            allocator.state,
            (sxml_allocation_request){
                .byte_count = sizeof(mpd_sxml_block_header) + size,
                .alignment = alignof(mpd_sxml_block_header),
            },
            &memory) != SXML_STATUS_OK ||
        memory == NULL) {
        return NULL;
    }
    header = memory;
    header->block.allocator = allocator;
    header->block.byte_count = size;
    return header + 1;
}

static mpd_sxml_block_header *
mpd_sxml_header_of(void *ptr)
{
    return (mpd_sxml_block_header *)ptr - 1;
}

void *
mpd_mallocfunc(size_t size)
{
    if (mpd_sxml_scope_allocator == NULL) {
        return NULL;
    }
    return mpd_sxml_allocate(*mpd_sxml_scope_allocator, size);
}

void *
mpd_callocfunc(size_t nmemb, size_t size)
{
    return mpd_callocfunc_em(nmemb, size);
}

void
mpd_free(void *ptr)
{
    mpd_sxml_block_header *header;

    if (ptr == NULL) {
        return;
    }
    header = mpd_sxml_header_of(ptr);
    header->block.allocator.operations->release(
        header->block.allocator.state, header);
}

/* The allocator interface has no reallocate (D22): allocate from the
   block's own allocator, copy and release. On failure the old block is
   kept, as realloc does. A smaller size keeps the block as it is. */
void *
mpd_reallocfunc(void *ptr, size_t size)
{
    mpd_sxml_block_header *header;
    void *new_block;

    if (ptr == NULL) {
        return mpd_mallocfunc(size);
    }
    header = mpd_sxml_header_of(ptr);
    if (size <= header->block.byte_count) {
        /* Shrinking keeps the block: it cannot fail, and libmpdec only
           shrinks to give memory back early. */
        return ptr;
    }
    new_block = mpd_sxml_allocate(header->block.allocator, size);
    if (new_block == NULL) {
        return NULL;
    }
    memcpy(new_block, ptr, header->block.byte_count);
    mpd_free(ptr);
    return new_block;
}


/* emulate calloc if it is not available */
void *
mpd_callocfunc_em(size_t nmemb, size_t size)
{
    void *ptr;
    size_t req;
    mpd_size_t overflow;

    req = mul_size_t_overflow((mpd_size_t)nmemb, (mpd_size_t)size,
                              &overflow);
    if (overflow) {
        return NULL;
    }

    ptr = mpd_mallocfunc(req);
    if (ptr == NULL) {
        return NULL;
    }
    /* used on uint32_t or uint64_t */
    memset(ptr, 0, req);

    return ptr;
}


/* malloc with overflow checking */
void *
mpd_alloc(mpd_size_t nmemb, mpd_size_t size)
{
    mpd_size_t req, overflow;

    req = mul_size_t_overflow(nmemb, size, &overflow);
    if (overflow) {
        return NULL;
    }

    return mpd_mallocfunc(req);
}

/* calloc with overflow checking */
void *
mpd_calloc(mpd_size_t nmemb, mpd_size_t size)
{
    mpd_size_t overflow;

    (void)mul_size_t_overflow(nmemb, size, &overflow);
    if (overflow) {
        return NULL;
    }

    return mpd_callocfunc(nmemb, size);
}

/* realloc with overflow checking */
void *
mpd_realloc(void *ptr, mpd_size_t nmemb, mpd_size_t size, uint8_t *err)
{
    void *new;
    mpd_size_t req, overflow;

    req = mul_size_t_overflow(nmemb, size, &overflow);
    if (overflow) {
        *err = 1;
        return ptr;
    }

    new = mpd_reallocfunc(ptr, req);
    if (new == NULL) {
        *err = 1;
        return ptr;
    }

    return new;
}

/* struct hack malloc with overflow checking */
void *
mpd_sh_alloc(mpd_size_t struct_size, mpd_size_t nmemb, mpd_size_t size)
{
    mpd_size_t req, overflow;

    req = mul_size_t_overflow(nmemb, size, &overflow);
    if (overflow) {
        return NULL;
    }

    req = add_size_t_overflow(req, struct_size, &overflow);
    if (overflow) {
        return NULL;
    }

    return mpd_mallocfunc(req);
}


/* Allocate a new decimal with a coefficient of length 'nwords'. In case
   of an error the return value is NULL. */
mpd_t *
mpd_qnew_size(mpd_ssize_t nwords)
{
    mpd_t *result;

    nwords = (nwords < MPD_MINALLOC) ? MPD_MINALLOC : nwords;

    result = mpd_alloc(1, sizeof *result);
    if (result == NULL) {
        return NULL;
    }

    result->data = mpd_alloc(nwords, sizeof *result->data);
    if (result->data == NULL) {
        mpd_free(result);
        return NULL;
    }

    result->flags = 0;
    result->exp = 0;
    result->digits = 0;
    result->len = 0;
    result->alloc = nwords;

    return result;
}

/* Allocate a new decimal with a coefficient of length MPD_MINALLOC.
   In case of an error the return value is NULL. */
mpd_t *
mpd_qnew(void)
{
    return mpd_qnew_size(MPD_MINALLOC);
}

/* Allocate new decimal. Caller can check for NULL or MPD_Malloc_error.
   Raises on error. */
mpd_t *
mpd_new(mpd_context_t *ctx)
{
    mpd_t *result;

    result = mpd_qnew();
    if (result == NULL) {
        mpd_addstatus_raise(ctx, MPD_Malloc_error);
    }
    return result;
}

/*
 * Input: 'result' is a static mpd_t with a static coefficient.
 * Assumption: 'nwords' >= result->alloc.
 *
 * Resize the static coefficient to a larger dynamic one and copy the
 * existing data. If successful, the value of 'result' is unchanged.
 * Otherwise, set 'result' to NaN and update 'status' with MPD_Malloc_error.
 */
int
mpd_switch_to_dyn(mpd_t *result, mpd_ssize_t nwords, uint32_t *status)
{
    mpd_uint_t *p = result->data;

    assert(nwords >= result->alloc);

    result->data = mpd_alloc(nwords, sizeof *result->data);
    if (result->data == NULL) {
        result->data = p;
        mpd_set_qnan(result);
        mpd_set_positive(result);
        result->exp = result->digits = result->len = 0;
        *status |= MPD_Malloc_error;
        return 0;
    }

    memcpy(result->data, p, result->alloc * (sizeof *result->data));
    result->alloc = nwords;
    mpd_set_dynamic_data(result);
    return 1;
}

/*
 * Input: 'result' is a static mpd_t with a static coefficient.
 *
 * Convert the coefficient to a dynamic one that is initialized to zero. If
 * malloc fails, set 'result' to NaN and update 'status' with MPD_Malloc_error.
 */
int
mpd_switch_to_dyn_zero(mpd_t *result, mpd_ssize_t nwords, uint32_t *status)
{
    mpd_uint_t *p = result->data;

    result->data = mpd_calloc(nwords, sizeof *result->data);
    if (result->data == NULL) {
        result->data = p;
        mpd_set_qnan(result);
        mpd_set_positive(result);
        result->exp = result->digits = result->len = 0;
        *status |= MPD_Malloc_error;
        return 0;
    }

    result->alloc = nwords;
    mpd_set_dynamic_data(result);

    return 1;
}

/*
 * Input: 'result' is a static or a dynamic mpd_t with a dynamic coefficient.
 * Resize the coefficient to length 'nwords':
 *   Case nwords > result->alloc:
 *     If realloc is successful:
 *       'result' has a larger coefficient but the same value. Return 1.
 *     Otherwise:
 *       Set 'result' to NaN, update status with MPD_Malloc_error and return 0.
 *   Case nwords < result->alloc:
 *     If realloc is successful:
 *       'result' has a smaller coefficient. result->len is undefined. Return 1.
 *     Otherwise (unlikely):
 *       'result' is unchanged. Reuse the now oversized coefficient. Return 1.
 */
int
mpd_realloc_dyn(mpd_t *result, mpd_ssize_t nwords, uint32_t *status)
{
    uint8_t err = 0;

    result->data = mpd_realloc(result->data, nwords, sizeof *result->data, &err);
    if (!err) {
        result->alloc = nwords;
    }
    else if (nwords > result->alloc) {
        mpd_set_qnan(result);
        mpd_set_positive(result);
        result->exp = result->digits = result->len = 0;
        *status |= MPD_Malloc_error;
        return 0;
    }

    return 1;
}

/*
 * Input: 'result' is a static mpd_t with a static coefficient.
 * Assumption: 'nwords' >= result->alloc.
 *
 * Resize the static coefficient to a larger dynamic one and copy the
 * existing data.
 *
 * On failure the value of 'result' is unchanged.
 */
int
mpd_switch_to_dyn_cxx(mpd_t *result, mpd_ssize_t nwords)
{
    assert(nwords >= result->alloc);

    mpd_uint_t *data = mpd_alloc(nwords, sizeof *result->data);
    if (data == NULL) {
        return 0;
    }

    memcpy(data, result->data, result->alloc * (sizeof *result->data));
    result->data = data;
    result->alloc = nwords;
    mpd_set_dynamic_data(result);
    return 1;
}

/*
 * Input: 'result' is a static or a dynamic mpd_t with a dynamic coefficient.
 * Resize the coefficient to length 'nwords':
 *   Case nwords > result->alloc:
 *     If realloc is successful:
 *       'result' has a larger coefficient but the same value. Return 1.
 *     Otherwise:
 *       'result' has a the same coefficient. Return 0.
 *   Case nwords < result->alloc:
 *     If realloc is successful:
 *       'result' has a smaller coefficient. result->len is undefined. Return 1.
 *     Otherwise (unlikely):
 *       'result' is unchanged. Reuse the now oversized coefficient. Return 1.
 */
int
mpd_realloc_dyn_cxx(mpd_t *result, mpd_ssize_t nwords)
{
    uint8_t err = 0;

    mpd_uint_t *p = mpd_realloc(result->data, nwords, sizeof *result->data, &err);
    if (!err) {
        result->data = p;
        result->alloc = nwords;
    }
    else if (nwords > result->alloc) {
        return 0;
    }

    return 1;
}
