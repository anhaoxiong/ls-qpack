/*
 * lsqpack.c -- LiteSpeed QPACK Compression Library: encoder and decoder.
 */
/*
MIT License

Copyright (c) 2018 LiteSpeed Technologies Inc

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "lsqpack.h"
#include XXH_HEADER_NAME

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define QPACK_STATIC_TABLE_SIZE   61

/* RFC 7541, Section 4.1:
 *
 * " The size of the dynamic table is the sum of the size of its entries.
 * "
 * " The size of an entry is the sum of its name's length in octets (as
 * " defined in Section 5.2), its value's length in octets, and 32.
 */
#define DYNAMIC_ENTRY_OVERHEAD 32u

#define MAX_QUIC_STREAM_ID ((1ull << 62) - 1)

/* Entries in the encoder's dynamic table are hashed 1) by name and 2) by
 * name and value.  Instead of having two arrays of buckets, the encoder
 * keeps just one, but each bucket has two heads.
 */
struct lsqpack_double_enc_head
{
    struct lsqpack_enc_head by_name;
    struct lsqpack_enc_head by_nameval;
};

struct lsqpack_enc_table_entry
{
    /* An entry always lives on all three lists */
    STAILQ_ENTRY(lsqpack_enc_table_entry)
                                    ete_next_nameval,
                                    ete_next_name,
                                    ete_next_all;
    lsqpack_abs_id_t                ete_id;
    unsigned                        ete_n_reffd;
    unsigned                        ete_nameval_hash;
    unsigned                        ete_name_hash;
    unsigned                        ete_name_len;
    unsigned                        ete_val_len;
    char                            ete_buf[0];
};

#define ETE_NAME(ete) ((ete)->ete_buf)
#define ETE_VALUE(ete) (&(ete)->ete_buf[(ete)->ete_name_len])
#define ENTRY_COST(name_len, value_len) (DYNAMIC_ENTRY_OVERHEAD + \
                                                        name_len + value_len)
#define ETE_SIZE(ete) ENTRY_COST((ete)->ete_name_len, (ete)->ete_val_len)


#define N_BUCKETS(n_bits) (1U << (n_bits))
#define BUCKNO(n_bits, hash) ((hash) & (N_BUCKETS(n_bits) - 1))

int
lsqpack_enc_init (struct lsqpack_enc *enc, unsigned dyn_table_size,
    unsigned max_risked_streams)
{
    struct lsqpack_double_enc_head *buckets;
    unsigned nbits = 2;
    unsigned i;

    if (dyn_table_size > LSQPACK_MAX_DYN_TABLE_SIZE ||
        max_risked_streams > LSQPACK_MAX_MAX_RISKED_STREAMS)
    {
        errno = EINVAL;
        return -1;
    }

    buckets = malloc(sizeof(buckets[0]) * N_BUCKETS(nbits));
    if (!buckets)
        return -1;

    for (i = 0; i < N_BUCKETS(nbits); ++i)
    {
        STAILQ_INIT(&buckets[i].by_name);
        STAILQ_INIT(&buckets[i].by_nameval);
    }

    memset(enc, 0, sizeof(*enc));
    STAILQ_INIT(&enc->qpe_all_entries);
    enc->qpe_max_capacity = dyn_table_size;
    enc->qpe_max_risked_streams = max_risked_streams;
    enc->qpe_buckets      = buckets;
    enc->qpe_nbits        = nbits;
    return 0;
}


void
lsqpack_enc_cleanup (struct lsqpack_enc *enc)
{
    struct lsqpack_enc_table_entry *entry, *next;
    for (entry = STAILQ_FIRST(&enc->qpe_all_entries); entry; entry = next)
    {
        next = STAILQ_NEXT(entry, ete_next_all);
        free(entry);
    }
    free(enc->qpe_buckets);
    free(enc->qpe_hinfos_arr);
}


struct static_table_entry
{
    const char       *name;
    const char       *val;
    unsigned          name_len;
    unsigned          val_len;
};

static const struct static_table_entry static_table[QPACK_STATIC_TABLE_SIZE];

//not find return 0, otherwise return the index
static unsigned
lsqpack_enc_get_stx_tab_id (const char *name, unsigned name_len,
                const char *val, unsigned val_len, int *val_matched)
{
    if (name_len < 3)
        return 0;

    *val_matched = 0;

    //check value first
    int i = -1;
    switch (*val)
    {
        case 'G':
            i = 1;
            break;
        case 'P':
            i = 2;
            break;
        case '/':
            if (val_len == 1)
                i = 3;
            else if (val_len == 11)
                i = 4;
            break;
        case 'h':
            if (val_len == 4)
                i = 5;
            else if (val_len == 5)
                i = 6;
            break;
        case '2':
            if (val_len == 3)
            {
                switch (*(val + 2))
                {
                    case '0':
                        i = 7;
                        break;
                    case '4':
                        i = 8;
                        break;
                    case '6':
                        i = 9;
                        break;
                    default:
                        break;
                }
            }
            break;
        case '3':
            i = 10;
            break;
        case '4':
            if (val_len == 3)
            {
                switch (*(val + 2))
                {
                    case '0':
                        i = 11;
                        break;
                    case '4':
                        i = 12;
                    default:
                        break;
                }
            }
            break;
        case '5':
            i = 13;
            break;
        case 'g':
            i = 15;
            break;
        default:
            break;
    }

    if (i > 0 && static_table[i].val_len == val_len
            && static_table[i].name_len == name_len
            && memcmp(val, static_table[i].val, val_len) == 0
            && memcmp(name, static_table[i].name, name_len) == 0)
    {
        *val_matched = 1;
        return i + 1;
    }

    //macth name only checking
    i = -1;
    switch (*name)
    {
        case ':':
            switch (*(name + 1))
            {
                case 'a':
                    i = 0;
                    break;
                case 'm':
                    i = 1;
                    break;
                case 'p':
                    i = 3;
                    break;
                case 's':
                    if (*(name + 2) == 'c') //:scheme
                        i = 5;
                    else
                        i = 7;
                    break;
                default:
                    break;
            }
            break;
        case 'a':
            switch (name_len)
            {
                case 3:
                    i = 20; //age
                    break;
                case 5:
                    i = 21; //allow
                    break;
                case 6:
                    i = 18; //accept
                    break;
                case 13:
                    if (*(name + 1) == 'u')
                        i = 22; //authorization
                    else
                        i = 17; //accept-ranges
                    break;
                case 14:
                    i  = 14; //accept-charset
                    break;
                case 15:
                    if (*(name + 7) == 'l')
                        i = 16; //accept-language,
                    else
                        i = 15;// accept-encoding
                    break;
                case 27:
                    i = 19;//access-control-allow-origin
                    break;
                default:
                    break;
            }
            break;
        case 'c':
            switch (name_len)
            {
                case 6:
                    i = 31; //cookie
                    break;
                case 12:
                    i = 30; //content-type
                    break;
                case 13:
                    if (*(name + 1) == 'a')
                        i = 23; //cache-control
                    else
                        i = 29; //content-range
                    break;
                case 14:
                    i = 27; //content-length
                    break;
                case 16:
                    switch (*(name + 9))
                    {
                        case 'n':
                            i = 25 ;//content-encoding
                            break;
                        case 'a':
                            i = 26; //content-language
                            break;
                        case 'o':
                            i = 28; //content-location
                        default:
                            break;
                    }
                    break;
                case 19:
                    i = 24; //content-disposition
                    break;
            }
            break;
        case 'd':
            i = 32 ;//date
            break;
        case 'e':
            switch (name_len)
            {
                case 4:
                    i = 33; //etag
                    break;
                case 6:
                    i = 34;
                    break;
                case 7:
                    i = 35;
                    break;
                default:
                    break;
            }
            break;
        case 'f':
            i = 36; //from
            break;
        case 'h':
            i = 37; //host
            break;
        case 'i':
            switch (name_len)
            {
                case 8:
                    if (*(name + 3) == 'm')
                        i = 38; //if-match
                    else
                        i = 41; //if-range
                    break;
                case 13:
                    i = 40; //if-none-match
                    break;
                case 17:
                    i = 39; //if-modified-since
                    break;
                case 19:
                    i = 42; //if-unmodified-since
                    break;
                default:
                    break;
            }
            break;
        case 'l':
            switch (name_len)
            {
                case 4:
                    i = 44; //link
                    break;
                case 8:
                    i = 45; //location
                    break;
                case 13:
                    i = 43; //last-modified
                    break;
                default:
                    break;
            }
            break;
        case 'm':
            i = 46; //max-forwards
            break;
        case 'p':
            if (name_len == 18)
                i = 47; //proxy-authenticate
            else
                i = 48; //proxy-authorization
            break;
        case 'r':
            if (name_len >= 5)
            {
                switch (*(name + 4))
                {
                    case 'e':
                        if (name_len == 5)
                            i = 49; //range
                        else
                            i = 51; //refresh
                        break;
                    case 'r':
                        i = 50; //referer
                        break;
                    case 'y':
                        i = 52; //retry-after
                        break;
                    default:
                        break;
                }
            }
            break;
        case 's':
            switch (name_len)
            {
                case 6:
                    i = 53; //server
                    break;
                case 10:
                    i = 54; //set-cookie
                    break;
                case 25:
                    i = 55; //strict-transport-security
                    break;
                default:
                    break;
            }
            break;
        case 't':
            i = 56;//transfer-encoding
            break;
        case 'u':
            i = 57; //user-agent
            break;
        case 'v':
            if (name_len == 4)
                i = 58;
            else
                i = 59;
            break;
        case 'w':
            i = 60;
            break;
        default:
            break;
    }

    if (i >= 0
            && static_table[i].name_len == name_len
            && memcmp(name, static_table[i].name, name_len) == 0)
        return i + 1;

    return 0;
}


enum table_type {
    TT_STATIC = 0,
    TT_DYNAMIC = 1,
};


struct enc_search_result
{
    int                             esr_found;
    enum table_type                 esr_table_type;
    lsqpack_abs_id_t                esr_entry_id;
    int                             esr_value_match;
    struct lsqpack_enc_table_entry *esr_entry;
};


static struct enc_search_result
qenc_find_entry_in_static_table (const char *name, unsigned name_len,
                                 const char *value, unsigned value_len)
{
    unsigned static_table_id;
    int val_matched;

    static_table_id = lsqpack_enc_get_stx_tab_id(name, name_len, value,
                                                    value_len, &val_matched);
    if (static_table_id > 0)
        return (struct enc_search_result) { 1, TT_STATIC, static_table_id,
                                                        val_matched, NULL, };
    else
        return (struct enc_search_result) { 0, 0, 0, 0, NULL, };
}


static struct enc_search_result
qenc_find_entry_in_either_table (struct lsqpack_enc *enc, int risk,
        const char *name, unsigned name_len, const char *value,
        unsigned value_len)
{
    struct lsqpack_enc_table_entry *entry;
    unsigned name_hash, nameval_hash, buckno, static_table_id;
    XXH32_state_t hash_state;
    int val_matched;

    /* First, look for a match in the static table: */
    static_table_id = lsqpack_enc_get_stx_tab_id(name, name_len, value,
                                                    value_len, &val_matched);
    if (static_table_id > 0 && val_matched)
        return (struct enc_search_result) { 1, TT_STATIC,
                                            static_table_id, 1, NULL, };

    /* Search by name and value: */
    XXH32_reset(&hash_state, (uintptr_t) enc);
    XXH32_update(&hash_state, &name_len, sizeof(name_len));
    XXH32_update(&hash_state, name, name_len);
    name_hash = XXH32_digest(&hash_state);
    XXH32_update(&hash_state,  &value_len, sizeof(value_len));
    XXH32_update(&hash_state,  value, value_len);
    nameval_hash = XXH32_digest(&hash_state);
    buckno = BUCKNO(enc->qpe_nbits, nameval_hash);
    /* TODO there could be several matching entries.  We want to pick one
     * that's not too old (eviction blocking) and also not too young
     * (header blocking).
     */
    STAILQ_FOREACH(entry, &enc->qpe_buckets[buckno].by_nameval,
                                                        ete_next_nameval)
        if (nameval_hash == entry->ete_nameval_hash &&
            name_len == entry->ete_name_len &&
            value_len == entry->ete_val_len &&
            (risk || entry->ete_id <= enc->qpe_max_acked_id) &&
            (enc->qpe_cur_header.search_cutoff == 0
                || entry->ete_id > enc->qpe_cur_header.search_cutoff) &&
            0 == memcmp(name, ETE_NAME(entry), name_len) &&
            0 == memcmp(value, ETE_VALUE(entry), value_len))
        {
            return (struct enc_search_result) { 1, TT_DYNAMIC,
                                                entry->ete_id, 1, entry, };
        }

    /* Name/value match is not found, but if the caller found a matching
     * static table entry, no need to continue to search:
     */
    if (static_table_id > 0)
        return (struct enc_search_result) { 1, TT_STATIC,
                                            static_table_id, 0, NULL, };

    /* Search by name only: */
    buckno = BUCKNO(enc->qpe_nbits, name_hash);
    STAILQ_FOREACH(entry, &enc->qpe_buckets[buckno].by_name, ete_next_name)
        if (name_hash == entry->ete_name_hash &&
            name_len == entry->ete_name_len &&
            (risk || entry->ete_id <= enc->qpe_max_acked_id) &&
            (enc->qpe_cur_header.search_cutoff == 0
                || entry->ete_id > enc->qpe_cur_header.search_cutoff) &&
            0 == memcmp(name, ETE_NAME(entry), name_len))
        {
            return (struct enc_search_result) { 1, TT_DYNAMIC,
                                                entry->ete_id, 0, entry, };
        }

    return (struct enc_search_result) { 0, 0, 0, 0, NULL, };
}


static struct enc_search_result
qenc_find_entry (struct lsqpack_enc *enc, int risk, const char *name,
        unsigned name_len, const char *value,
        unsigned value_len)
{
    if (enc->qpe_cur_header.use_dynamic_table)
        return qenc_find_entry_in_either_table(enc, risk, name, name_len, value,
                                                                    value_len);
    else
        return qenc_find_entry_in_static_table(name, name_len, value, value_len);
}


static unsigned
lsqpack_val2len (uint64_t value, unsigned prefix_bits)
{
    uint64_t mask = (1ULL << prefix_bits) - 1;
    return 1
         + (value >=                 mask )
         + (value >= ((1ULL <<  7) + mask))
         + (value >= ((1ULL << 14) + mask))
         + (value >= ((1ULL << 21) + mask))
         + (value >= ((1ULL << 28) + mask))
         + (value >= ((1ULL << 35) + mask))
         + (value >= ((1ULL << 42) + mask))
         + (value >= ((1ULL << 49) + mask))
         + (value >= ((1ULL << 56) + mask))
         + (value >= ((1ULL << 63) + mask))
         ;
}


unsigned char *
lsqpack_enc_int (unsigned char *dst, unsigned char *const end, uint64_t value,
                                                        unsigned prefix_bits)
{
    unsigned char *const dst_orig = dst;

    /* This function assumes that at least one byte is available */
    assert(dst < end);
    if (value < (1u << prefix_bits) - 1)
        *dst++ |= value;
    else
    {
        *dst++ |= (1 << prefix_bits) - 1;
        value -= (1 << prefix_bits) - 1;
        while (value >= 128)
        {
            if (dst < end)
            {
                *dst++ = (0x80 | value);
                value >>= 7;
            }
            else
                return dst_orig;
        }
        if (dst < end)
            *dst++ = value;
        else
            return dst_orig;
    }
    return dst;
}


static void
lsqpack_enc_int_nocheck (unsigned char *dst, uint64_t value,
                                                        unsigned prefix_bits)
{
    if (value < (1u << prefix_bits) - 1)
        *dst++ |= value;
    else
    {
        *dst++ |= (1 << prefix_bits) - 1;
        value -= (1 << prefix_bits) - 1;
        while (value >= 128)
        {
            *dst++ = (0x80 | value);
            value >>= 7;
        }
        *dst++ = value;
    }
}


struct encode_el
{
    uint32_t code;
    int      bits;
};


static const struct encode_el encode_table[257];


static unsigned char *
qenc_huffman_enc (const unsigned char *src, const unsigned char *const src_end,
                                                                unsigned char *dst)
{
    uint64_t bits = 0;
    int bits_left = 40;
    struct encode_el cur_enc_code;

    while (src != src_end)
    {
        cur_enc_code = encode_table[(int) *src++];
        assert(bits_left >= cur_enc_code.bits); //  (possible negative shift, undefined behavior)
        bits |= (uint64_t)cur_enc_code.code << (bits_left - cur_enc_code.bits);
        bits_left -= cur_enc_code.bits;
        while (bits_left <= 32)
        {
            *dst++ = bits >> 32;
            bits <<= 8;
            bits_left += 8;
        }
    }

    if (bits_left != 40)
    {
        assert(bits_left < 40 && bits_left > 0);
        bits |= ((uint64_t)1 << bits_left) - 1;
        *dst++ = bits >> 32;
    }

    return dst;
}


int
lsqpack_enc_enc_str (unsigned prefix_bits, unsigned char *const dst,
        size_t dst_len, const unsigned char *str, unsigned str_len)
{
    unsigned char *p;
    unsigned i, enc_size_bits, enc_size_bytes, len_size;

    enc_size_bits = 0;
    for (i = 0; i < str_len; ++i)
        enc_size_bits += encode_table[ str[i] ].bits;
    enc_size_bytes = enc_size_bits / 8 + ((enc_size_bits & 7) != 0);

    if (enc_size_bytes < str_len)
    {
        len_size = lsqpack_val2len(enc_size_bytes, prefix_bits);
        if (len_size + enc_size_bytes <= dst_len)
        {
            *dst &= ~((1 << (prefix_bits + 1)) - 1);
            *dst |= 1 << prefix_bits;
            lsqpack_enc_int_nocheck(dst, enc_size_bytes, prefix_bits);
            p = qenc_huffman_enc(str, str + str_len, dst + len_size);
            assert(p - dst == len_size + enc_size_bytes);
            return p - dst;
        }
        else
            return -1;
    }
    else
    {
        len_size = lsqpack_val2len(str_len, prefix_bits);
        if (len_size + str_len <= dst_len)
        {
            *dst &= ~((1 << (prefix_bits + 1)) - 1);
            lsqpack_enc_int_nocheck(dst, str_len, prefix_bits);
            memcpy(dst + len_size, str, str_len);
            return len_size + str_len;
        }
        else
            return -1;
    }
}


static void
qenc_drop_oldest_entry (struct lsqpack_enc *enc)
{
    struct lsqpack_enc_table_entry *entry;
    unsigned buckno;

    entry = STAILQ_FIRST(&enc->qpe_all_entries);
    assert(entry);
    STAILQ_REMOVE_HEAD(&enc->qpe_all_entries, ete_next_all);
    buckno = BUCKNO(enc->qpe_nbits, entry->ete_nameval_hash);
    assert(entry == STAILQ_FIRST(&enc->qpe_buckets[buckno].by_nameval));
    STAILQ_REMOVE_HEAD(&enc->qpe_buckets[buckno].by_nameval, ete_next_nameval);
    buckno = BUCKNO(enc->qpe_nbits, entry->ete_name_hash);
    assert(entry == STAILQ_FIRST(&enc->qpe_buckets[buckno].by_name));
    STAILQ_REMOVE_HEAD(&enc->qpe_buckets[buckno].by_name, ete_next_name);

    enc->qpe_cur_capacity -= ETE_SIZE(entry);
    --enc->qpe_nelem;
    free(entry);
}


static void
qenc_remove_overflow_entries (struct lsqpack_enc *enc)
{
    while (enc->qpe_cur_capacity > enc->qpe_max_capacity)
        qenc_drop_oldest_entry(enc);
}


static int
qenc_grow_tables (struct lsqpack_enc *enc)
{
    struct lsqpack_double_enc_head *new_buckets, *new[2];
    struct lsqpack_enc_table_entry *entry;
    unsigned n, old_nbits;
    int idx;

    old_nbits = enc->qpe_nbits;
    new_buckets = malloc(sizeof(enc->qpe_buckets[0])
                                                * N_BUCKETS(old_nbits + 1));
    if (!new_buckets)
        return -1;

    for (n = 0; n < N_BUCKETS(old_nbits); ++n)
    {
        new[0] = &new_buckets[n];
        new[1] = &new_buckets[n + N_BUCKETS(old_nbits)];
        STAILQ_INIT(&new[0]->by_name);
        STAILQ_INIT(&new[1]->by_name);
        STAILQ_INIT(&new[0]->by_nameval);
        STAILQ_INIT(&new[1]->by_nameval);
        while ((entry = STAILQ_FIRST(&enc->qpe_buckets[n].by_name)))
        {
            STAILQ_REMOVE_HEAD(&enc->qpe_buckets[n].by_name, ete_next_name);
            idx = (BUCKNO(old_nbits + 1, entry->ete_name_hash)
                                                        >> old_nbits) & 1;
            STAILQ_INSERT_TAIL(&new[idx]->by_name, entry, ete_next_name);
        }
        while ((entry = STAILQ_FIRST(&enc->qpe_buckets[n].by_nameval)))
        {
            STAILQ_REMOVE_HEAD(&enc->qpe_buckets[n].by_nameval,
                                                        ete_next_nameval);
            idx = (BUCKNO(old_nbits + 1, entry->ete_nameval_hash)
                                                        >> old_nbits) & 1;
            STAILQ_INSERT_TAIL(&new[idx]->by_nameval, entry,
                                                        ete_next_nameval);
        }
    }

    free(enc->qpe_buckets);
    enc->qpe_nbits   = old_nbits + 1;
    enc->qpe_buckets = new_buckets;
    return 0;
}


static struct lsqpack_enc_table_entry *
lsqpack_enc_push_entry (struct lsqpack_enc *enc, const char *name,
                        unsigned name_len, const char *value,
                        unsigned value_len)
{
    unsigned name_hash, nameval_hash, buckno;
    struct lsqpack_enc_table_entry *entry;
    XXH32_state_t hash_state;
    size_t size;

    if (enc->qpe_nelem >= N_BUCKETS(enc->qpe_nbits) / 2 &&
                                                0 != qenc_grow_tables(enc))
        return NULL;

    size = sizeof(*entry) + name_len + value_len;
    entry = malloc(size);
    if (!entry)
        return NULL;

    XXH32_reset(&hash_state, (uintptr_t) enc);
    XXH32_update(&hash_state, &name_len, sizeof(name_len));
    XXH32_update(&hash_state, name, name_len);
    name_hash = XXH32_digest(&hash_state);
    XXH32_update(&hash_state,  &value_len, sizeof(value_len));
    XXH32_update(&hash_state,  value, value_len);
    nameval_hash = XXH32_digest(&hash_state);

    entry->ete_name_hash = name_hash;
    entry->ete_nameval_hash = nameval_hash;
    entry->ete_name_len = name_len;
    entry->ete_val_len = value_len;
    entry->ete_id = 1 + enc->qpe_ins_count++;
    memcpy(ETE_NAME(entry), name, name_len);
    memcpy(ETE_VALUE(entry), value, value_len);

    STAILQ_INSERT_TAIL(&enc->qpe_all_entries, entry, ete_next_all);
    buckno = BUCKNO(enc->qpe_nbits, nameval_hash);
    STAILQ_INSERT_TAIL(&enc->qpe_buckets[buckno].by_nameval, entry,
                                                        ete_next_nameval);
    buckno = BUCKNO(enc->qpe_nbits, name_hash);
    STAILQ_INSERT_TAIL(&enc->qpe_buckets[buckno].by_name, entry,
                                                        ete_next_name);

    enc->qpe_cur_capacity += ENTRY_COST(name_len, value_len);
    ++enc->qpe_nelem;
    qenc_remove_overflow_entries(enc);
    return entry;
}


int
lsqpack_enc_start_header (struct lsqpack_enc *enc, uint64_t stream_id,
                            unsigned seqno)
{
    struct lsqpack_header_info *hinfos_arr;
    unsigned at_risk, nalloc, i;

    if (enc->qpe_flags & LSQPACK_ENC_HEADER)
        return -1;

    enc->qpe_cur_header.hinfo = (struct lsqpack_header_info)
                        { .qhi_stream_id = stream_id, .qhi_seqno = seqno, };
    enc->qpe_cur_header.n_risked = 0;
    enc->qpe_cur_header.base_idx = enc->qpe_ins_count;
    enc->qpe_cur_header.search_cutoff = 0;
    enc->qpe_cur_header.use_dynamic_table = 1;

    if (enc->qpe_hinfos_count == enc->qpe_hinfos_nalloc)
    {
        if (enc->qpe_hinfos_nalloc)
            nalloc = enc->qpe_hinfos_nalloc * 2;
        else
            nalloc = 4;
        hinfos_arr = realloc(enc->qpe_hinfos_arr,
                                    sizeof(enc->qpe_hinfos_arr[0]) * 2);
        if (hinfos_arr)
        {
            free(enc->qpe_hinfos_arr);
            enc->qpe_hinfos_arr = hinfos_arr;
            enc->qpe_hinfos_nalloc = nalloc;
        }
        else
            enc->qpe_cur_header.use_dynamic_table = 0;
    }

    /* Check if there are other header blocks with the same stream ID that
     * are at risk.
     */
    if (seqno)
    {
        for (i = 0; i < enc->qpe_hinfos_count; ++i)
            if (enc->qpe_hinfos_arr[i].qhi_stream_id == stream_id
                                && enc->qpe_hinfos_arr[i].qhi_at_risk)
                break;
        at_risk = i < enc->qpe_hinfos_count;
    }
    else
        at_risk = 0;

    enc->qpe_cur_header.others_at_risk = at_risk > 0;
    enc->qpe_flags |= LSQPACK_ENC_HEADER;

    return 0;
}


ssize_t
lsqpack_enc_end_header (struct lsqpack_enc *enc, unsigned char *buf, size_t sz)
{
    unsigned char *dst, *end;
    lsqpack_abs_id_t diff;
    unsigned sign;

    if (!(enc->qpe_flags & LSQPACK_ENC_HEADER))
        return -1;

    if (enc->qpe_cur_header.hinfo.qhi_max_id)
    {
        end = buf + sz;

        *buf = 0;
        dst = lsqpack_enc_int(buf, end, enc->qpe_cur_header.hinfo.qhi_max_id, 8);
        if (dst <= buf)
            return 0;

        if (dst >= end)
            return 0;

        buf = dst;
        if (enc->qpe_cur_header.base_idx
                                    >= enc->qpe_cur_header.hinfo.qhi_max_id)
        {
            sign = 0;
            diff = enc->qpe_cur_header.base_idx
                                    - enc->qpe_cur_header.hinfo.qhi_max_id;
        }
        else
        {
            sign = 1;
            diff = enc->qpe_cur_header.hinfo.qhi_max_id
                                    - enc->qpe_cur_header.base_idx;
        }
        *buf = sign << 7;
        dst = lsqpack_enc_int(buf, end, diff, 7);
        if (dst <= buf)
            return 0;

        enc->qpe_flags &= ~LSQPACK_ENC_HEADER;
        return dst - end + sz;
    }
    else if (sz >= 2)
    {
        memset(buf, 0, 2);
        enc->qpe_flags &= ~LSQPACK_ENC_HEADER;
        return 2;
    }
    else
        return 0;
}


struct encode_program
{
    enum {
        EEA_NONE,
        EEA_INS_NAMEREF,
        EEA_INS_LIT,
    }           ep_enc_action;
    enum {
        EHA_INDEXED_NEW,
        EHA_INDEXED_STAT,
        EHA_INDEXED_DYN,
        EHA_LIT_WITH_NAME_STAT,
        EHA_LIT_WITH_NAME_DYN,
        EHA_LIT_WITH_NAME_NEW,
        EHA_LIT,
    }           ep_hea_action;
    enum {
        ETA_NOOP,
        ETA_NEW,
    }           ep_tab_action;
    enum {
        EPF_REF_FOUND   = 1 << 1,
        EPF_REF_NEW     = 1 << 2,
    }           ep_flags;
};

/* Factors at play:
 *
 *      - Found or not found
 *      - Table: static or dynamic
 *      - Value matched or not
 *      - Index: yes/no
 *      - Risk blocking: yes/no
 *
 * Effects:
 *
 *      - Output to encoder stream (if index=yes)
 *      - Insert new dynamic entry into the table (if index=yes)
 *      - Output to header stream.  This always happens, but the output
 *        depends on the value of "risk blocking".
 */
static const struct encode_program encode_programs[2][2][2][2][2] =
{
/* "A" is for "Any" (this makes the table narrower) */
#define A 0 ... 1
 /*
  *  ,--------------- Found or not found (bool)
  *  |  ,------------ Static or dynamic (0 or 1, respectively)
  *  |  |  ,--------- Value matched or not (bool)
  *  |  |  |  ,------ To index or not to index (bool)
  *  |  |  |  |  ,--- To risk blocking or not to risk (bool)
  *  |  |  |  |  |
  *  |  |  |  |  |
  *  V  V  V  V  V
  */
    [0][A][A][0][A] = { EEA_NONE,        EHA_LIT,                ETA_NOOP, 0, },
    [0][A][A][1][0] = { EEA_INS_LIT,     EHA_LIT,                ETA_NEW,  0, },
    [0][A][A][1][1] = { EEA_INS_LIT,     EHA_INDEXED_NEW,        ETA_NEW,  EPF_REF_NEW, },
    [1][0][0][0][A] = { EEA_NONE,        EHA_LIT_WITH_NAME_STAT, ETA_NOOP, 0, },
    [1][0][0][1][0] = { EEA_INS_NAMEREF, EHA_LIT_WITH_NAME_STAT, ETA_NEW,  0, },
    [1][0][0][1][1] = { EEA_INS_NAMEREF, EHA_INDEXED_NEW,        ETA_NEW,  EPF_REF_NEW, },
    [1][0][1][A][A] = { EEA_NONE,        EHA_INDEXED_STAT,       ETA_NOOP, 0, },
    [1][1][0][0][A] = { EEA_NONE,        EHA_LIT_WITH_NAME_DYN,  ETA_NOOP, EPF_REF_FOUND, },
    [1][1][0][1][A] = { EEA_INS_NAMEREF, EHA_LIT_WITH_NAME_NEW,  ETA_NEW,  EPF_REF_NEW|EPF_REF_FOUND, },
    [1][1][1][A][A] = { EEA_NONE,        EHA_INDEXED_DYN,        ETA_NOOP, EPF_REF_FOUND, },
#undef A
};


static int
enc_has_or_can_evict_at_least (struct lsqpack_enc *enc, size_t new_entry_size)
{
    const struct lsqpack_enc_table_entry *entry;
    lsqpack_abs_id_t min_id;
    size_t avail;

    avail = enc->qpe_max_capacity - enc->qpe_cur_capacity;
    if (avail >= new_entry_size)
        return 1;

    min_id = 0; /* TODO */
    STAILQ_FOREACH(entry, &enc->qpe_all_entries, ete_next_all)
        if (entry->ete_id < min_id)
        {
            avail += ETE_SIZE(entry);
            if (avail >= new_entry_size)
            {
                enc->qpe_cur_header.search_cutoff = entry->ete_id;
                return 1;
            }
        }
        else
            return 0;

    return 0;
}


enum lsqpack_enc_status
lsqpack_enc_encode (struct lsqpack_enc *enc,
        unsigned char *enc_buf, size_t *enc_sz_p,
        unsigned char *hea_buf, size_t *hea_sz_p,
        const char *name, unsigned name_len,
        const char *value, unsigned value_len,
        enum lsqpack_enc_flags flags)
{
    unsigned char *const enc_buf_end = enc_buf + *enc_sz_p;
    unsigned char *const hea_buf_end = hea_buf + *hea_sz_p;
    struct lsqpack_enc_table_entry *new_entry;
    struct encode_program prog;
    struct enc_search_result esr;
    int index, risk;

    size_t enc_sz, hea_sz;
    unsigned char *dst;
    lsqpack_abs_id_t id;
    int r;

    /* Encoding always outputs at least a byte to the header block.  If
     * no bytes are available, encoding cannot proceed.
     */
    if (hea_buf == hea_buf_end)
        return LQES_NOBUF_HEAD;

    index = !(flags & LQEF_NO_INDEX)
        && enc->qpe_cur_header.use_dynamic_table
        && enc->qpe_ins_count < LSQPACK_MAX_ABS_ID
        && enc_has_or_can_evict_at_least(enc, ENTRY_COST(name_len, value_len));

  restart:
    risk = enc->qpe_cur_header.n_risked > 0
        || enc->qpe_cur_header.others_at_risk
        || enc->qpe_cur_streams_at_risk < enc->qpe_max_risked_streams;

    esr = qenc_find_entry(enc, risk, name, name_len, value, value_len);

    prog = encode_programs
                [esr.esr_found]
                [esr.esr_table_type]
                [esr.esr_value_match]
                [index]
                [risk];

    switch (prog.ep_enc_action)
    {
    case EEA_INS_NAMEREF:
        dst = enc_buf;
        if (TT_STATIC == esr.esr_table_type)
        {
            *dst = 0x80 | 0x40;
            id = esr.esr_entry_id;
        }
        else
        {
            *dst = 0x80;
            id = enc->qpe_ins_count - esr.esr_entry_id;
        }
        dst = lsqpack_enc_int(dst, enc_buf_end, id, 6);
        if (dst <= enc_buf)
            return LQES_NOBUF_ENC;
        r = lsqpack_enc_enc_str(7, dst, enc_buf_end - dst,
                                    (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_ENC;
        dst += (unsigned) r;
        enc_sz = dst - enc_buf;
        break;
    case EEA_INS_LIT:
        dst = enc_buf;
        *dst = 0x40;
        r = lsqpack_enc_enc_str(5, dst, enc_buf_end - dst,
                                (const unsigned char *) name, name_len);
        if (r < 0)
            return LQES_NOBUF_ENC;
        dst += r;
        r = lsqpack_enc_enc_str(7, dst, enc_buf_end - dst,
                                (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_ENC;
        dst += r;
        enc_sz = dst - enc_buf;
        break;
    default:
        assert(EEA_NONE == prog.ep_enc_action);
        enc_sz = 0;
        break;
    }

    dst = hea_buf;
    switch (prog.ep_hea_action)
    {
    case EHA_INDEXED_STAT:
        *dst = 0x80 | 0x40;
        dst = lsqpack_enc_int(dst, hea_buf_end, esr.esr_entry_id, 6);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        hea_sz = dst - hea_buf;
        break;
    case EHA_INDEXED_NEW:
        id = enc->qpe_ins_count + 1;
  post_base_idx:
        *dst = 0x10;
        assert(id > enc->qpe_cur_header.base_idx);
        id -= enc->qpe_cur_header.base_idx;
        dst = lsqpack_enc_int(dst, hea_buf_end, id, 4);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        hea_sz = dst - hea_buf;
        break;
    case EHA_INDEXED_DYN:
        id = esr.esr_entry_id;
        if (id > enc->qpe_cur_header.base_idx)
            goto post_base_idx;
        *dst = 0x80;
        dst = lsqpack_enc_int(dst, hea_buf_end, esr.esr_entry_id, 6);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        hea_sz = dst - hea_buf;
        break;
    case EHA_LIT:
        *dst = 0x20
               | (((flags & LQEF_NO_INDEX) > 0) << 4)
               ;
        r = lsqpack_enc_enc_str(3, dst, hea_buf_end - dst,
                                (const unsigned char *) name, name_len);
        if (r < 0)
            return LQES_NOBUF_HEAD;
        dst += r;
        r = lsqpack_enc_enc_str(7, dst, hea_buf_end - dst,
                                (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_HEAD;
        dst += r;
        hea_sz = dst - hea_buf;
        break;
    case EHA_LIT_WITH_NAME_NEW:
        id = esr.esr_entry_id;
 post_base_name_ref:
        *dst = (((flags & LQEF_NO_INDEX) > 0) << 3);
        assert(id > enc->qpe_cur_header.base_idx);
        id -= enc->qpe_cur_header.base_idx;
        dst = lsqpack_enc_int(dst, hea_buf_end, id, 3);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        r = lsqpack_enc_enc_str(7, dst, hea_buf_end - dst,
                                (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_HEAD;
        dst += (unsigned) r;
        hea_sz = dst - hea_buf;
        break;
    case EHA_LIT_WITH_NAME_DYN:
        id = esr.esr_entry_id;
        if (id > enc->qpe_cur_header.base_idx)
            goto post_base_name_ref;
        *dst = 0x40
               | (((flags & LQEF_NO_INDEX) > 0) << 5)
               ;
        id = enc->qpe_cur_header.base_idx - esr.esr_entry_id;
        dst = lsqpack_enc_int(dst, hea_buf_end, id, 4);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        r = lsqpack_enc_enc_str(7, dst, hea_buf_end - dst,
                                (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_HEAD;
        dst += (unsigned) r;
        hea_sz = dst - hea_buf;
        break;
    default:
        assert(prog.ep_hea_action == EHA_LIT_WITH_NAME_STAT);
        *dst = 0x40
               | (((flags & LQEF_NO_INDEX) > 0) << 5)
               | 0x10
               ;
        dst = lsqpack_enc_int(dst, hea_buf_end, esr.esr_entry_id, 4);
        if (dst <= hea_buf)
            return LQES_NOBUF_HEAD;
        r = lsqpack_enc_enc_str(7, dst, hea_buf_end - dst,
                                (const unsigned char *) value, value_len);
        if (r < 0)
            return LQES_NOBUF_HEAD;
        dst += (unsigned) r;
        hea_sz = dst - hea_buf;
        break;
    }

    switch (prog.ep_tab_action)
    {
    case ETA_NEW:
        new_entry = lsqpack_enc_push_entry(enc, name, name_len, value,
                                                                    value_len);
        if (!new_entry)
        {   /* Push can only fail due to inability to allocate memory.
             * In this case, fall back on encoding without indexing.
             */
            index = 0;
            goto restart;
        }
        if (prog.ep_flags & EPF_REF_NEW)
        {
            ++new_entry->ete_n_reffd;
            assert(new_entry->ete_id > enc->qpe_cur_header.hinfo.qhi_max_id);
            enc->qpe_cur_header.hinfo.qhi_max_id = new_entry->ete_id;
            ++enc->qpe_cur_header.n_risked;
            if (enc->qpe_cur_header.hinfo.qhi_min_id == 0
                    || enc->qpe_cur_header.hinfo.qhi_min_id > new_entry->ete_id)
                enc->qpe_cur_header.hinfo.qhi_min_id = new_entry->ete_id;
        }
        break;
    default:
        assert(prog.ep_tab_action == ETA_NOOP);
        break;
    }

    if (prog.ep_flags & EPF_REF_FOUND)
    {
        assert(esr.esr_table_type == TT_DYNAMIC);
        ++esr.esr_entry->ete_n_reffd;
        enc->qpe_cur_header.n_risked += enc->qpe_max_acked_id < esr.esr_entry_id;
        if (enc->qpe_cur_header.hinfo.qhi_min_id == 0
                || enc->qpe_cur_header.hinfo.qhi_min_id > esr.esr_entry_id)
            enc->qpe_cur_header.hinfo.qhi_min_id = esr.esr_entry_id;
        if (enc->qpe_cur_header.hinfo.qhi_max_id == 0
                || enc->qpe_cur_header.hinfo.qhi_max_id < esr.esr_entry_id)
            enc->qpe_cur_header.hinfo.qhi_max_id = esr.esr_entry_id;
    }

    *enc_sz_p = enc_sz;
    *hea_sz_p = hea_sz;
    return LQES_OK;
}


void
lsqpack_enc_set_max_capacity (struct lsqpack_enc *enc, unsigned max_capacity)
{
    enc->qpe_max_capacity = max_capacity;
    qenc_remove_overflow_entries(enc);
}


static int
enc_proc_header_ack (struct lsqpack_enc *enc, uint64_t stream_id)
{
    if (stream_id > MAX_QUIC_STREAM_ID)
        return -1;
    return -1;  /* TODO */
}


static int
enc_proc_table_synch (struct lsqpack_enc *enc, uint64_t abs_id)
{
    if (abs_id > LSQPACK_MAX_ABS_ID)
        return -1;
    return -1;  /* TODO */
}


static int
enc_proc_stream_cancel (struct lsqpack_enc *enc, uint64_t stream_id)
{
    if (stream_id > MAX_QUIC_STREAM_ID)
        return -1;
    return -1;  /* TODO */
}


/* Assumption: we have at least one byte to work with */
/* Return value:
 *  0   OK
 *  -1  Out of input
 *  -2  Value cannot be represented as 64-bit integer (overflow)
 */
int
lsqpack_dec_int (const unsigned char **src_p, const unsigned char *src_end,
                   unsigned prefix_bits, uint64_t *value_p,
                   struct lsqpack_dec_int_state *state)
{
    const unsigned char *const orig_src = *src_p;
    const unsigned char *src;
    unsigned char prefix_max;
    unsigned M, nread;
    uint64_t val, B;

    src = *src_p;

    if (state->resume)
    {
        val = state->val;
        M = state->M;
        goto resume;
    }

    prefix_max = (1 << prefix_bits) - 1;
    val = *src++;
    val &= prefix_max;

    if (val < prefix_max)
    {
        *src_p = src;
        *value_p = val;
        return 0;
    }

    M = 0;
    do
    {
        if (src < src_end)
        {
  resume:   B = *src++;
            val = val + ((B & 0x7f) << M);
            M += 7;
        }
        else
        {
            nread = (state->resume ? state->nread : 0) + (src - orig_src);
            if (nread < LSQPACK_UINT64_ENC_SZ)
            {
                state->val = val;
                state->M = M;
                state->nread = nread;
                state->resume = 1;
                return -1;
            }
            else
                return -2;
        }
    }
    while (B & 0x80);

    if (M <= 63 || (M == 70 && src[-1] <= 1 && (val & (1ull << 63))))
    {
        *src_p = src;
        *value_p = val;
        return 0;
    }
    else
        return -2;
}




int
lsqpack_enc_decoder_in (struct lsqpack_enc *enc,
                                    const unsigned char *buf, size_t buf_sz)
{
    const unsigned char *const end = buf + buf_sz;
    uint64_t val;
    int r;
    unsigned prefix_bits = -1;  /* This can be any value in a resumed call
                                 * to the integer decoder -- it is only
                                 * used in the first call.
                                 */

    while (buf < end)
    {
        switch (enc->qpe_dec_stream_state.dec_int_state.resume)
        {
        case 0:
            if (buf[0] & 0x80)              /* Header ACK */
            {
                prefix_bits = 7;
                enc->qpe_dec_stream_state.handler = enc_proc_header_ack;
            }
            else if ((buf[0] & 0xC) == 0)   /* Table State Synchronize */
            {
                prefix_bits = 6;
                enc->qpe_dec_stream_state.handler = enc_proc_table_synch;
            }
            else                            /* Stream Cancellation */
            {
                assert((buf[0] & 0xC) == 0x40);
                prefix_bits = 6;
                enc->qpe_dec_stream_state.handler = enc_proc_stream_cancel;
            }
            /* fall through */
        case 1:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &val,
                                &enc->qpe_dec_stream_state.dec_int_state);
            if (r == 0)
            {
                r = enc->qpe_dec_stream_state.handler(enc, val);
                if (r != 0)
                    return -1;
                enc->qpe_dec_stream_state.dec_int_state.resume = 0;
            }
            else if (r == -1)
            {
                enc->qpe_dec_stream_state.dec_int_state.resume = 1;
                return 0;
            }
            else
                return -1;
            break;
        }
    }

    return 0;
}


/* Dynamic table entry: */
struct lsqpack_dec_table_entry
{
    unsigned    dte_name_len;
    unsigned    dte_val_len;
    unsigned    dte_refcnt;
    char        dte_buf[0];     /* Contains both name and value */
};

#define DTE_NAME(dte) ((dte)->dte_buf)
#define DTE_VALUE(dte) (&(dte)->dte_buf[(dte)->dte_name_len])
#define DTE_SIZE(dte) ENTRY_COST((dte)->dte_name_len, (dte)->dte_val_len)

enum
{
    HPACK_HUFFMAN_FLAG_ACCEPTED = 0x01,
    HPACK_HUFFMAN_FLAG_SYM = 0x02,
    HPACK_HUFFMAN_FLAG_FAIL = 0x04,
};

#define lsqpack_arr_init(a) do {                                        \
    memset((a), 0, sizeof(*(a)));                                       \
} while (0)

#define lsqpack_arr_cleanup(a) do {                                     \
    free((a)->els);                                                     \
    memset((a), 0, sizeof(*(a)));                                       \
} while (0)

#define lsqpack_arr_get(a, i) (                                         \
    assert((i) < (a)->nelem),                                           \
    (a)->els[(a)->off + (i)]                                            \
)

#define lsqpack_arr_shift(a) (                                          \
    assert((a)->nelem > 0),                                             \
    (a)->nelem -= 1,                                                    \
    (a)->els[(a)->off++]                                                \
)

#define lsqpack_arr_pop(a) (                                            \
    assert((a)->nelem > 0),                                             \
    (a)->nelem -= 1,                                                    \
    (a)->els[(a)->off + (a)->nelem]                                     \
)

#define lsqpack_arr_count(a) (+(a)->nelem)

static int
lsqpack_arr_push (struct lsqpack_arr *arr, uintptr_t val)
{
    uintptr_t *new_els;
    unsigned n;

    if (arr->off + arr->nelem < arr->nalloc)
    {
        arr->els[arr->off + arr->nelem] = val;
        ++arr->nelem;
        return 0;
    }

    if (arr->off > arr->nalloc / 2)
    {
        memmove(arr->els, arr->els + arr->off,
                                        sizeof(arr->els[0]) * arr->nelem);
        arr->off = 0;
        arr->els[arr->nelem] = val;
        ++arr->nelem;
        return 0;
    }

    if (arr->nalloc)
        n = arr->nalloc * 2;
    else
        n = 64;
    new_els = malloc(n * sizeof(arr->els[0]));
    if (!new_els)
        return -1;
    memcpy(new_els, arr->els + arr->off, sizeof(arr->els[0]) * arr->nelem);
    free(arr->els);
    arr->off = 0;
    arr->els = new_els;
    arr->nalloc = n;
    arr->els[arr->off + arr->nelem] = val;
    ++arr->nelem;
    return 0;
}


struct lsqpack_min_heap_elem
{
    void                *mhe_hbrc;
    lsqpack_abs_id_t     mhe_max_id;
};


#define MHE_PARENT(i) ((i - 1) / 2)
#define MHE_LCHILD(i) (2 * i + 1)
#define MHE_RCHILD(i) (2 * i + 2)

#define mh_count(heap) ((heap)->mh_nelem)

static void
mh_heapify (struct lsqpack_min_heap *heap, unsigned i)
{
    struct lsqpack_min_heap_elem el;
    unsigned smallest;

    assert(i < heap->mh_nelem);

    if (MHE_LCHILD(i) < heap->mh_nelem)
    {
        if (heap->mh_elems[ MHE_LCHILD(i) ].mhe_max_id <
                                    heap->mh_elems[ i ].mhe_max_id)
            smallest = MHE_LCHILD(i);
        else
            smallest = i;
        if (MHE_RCHILD(i) < heap->mh_nelem &&
            heap->mh_elems[ MHE_RCHILD(i) ].mhe_max_id <
                                    heap->mh_elems[ smallest ].mhe_max_id)
            smallest = MHE_RCHILD(i);
    }
    else
        smallest = i;

    if (smallest != i)
    {
        el = heap->mh_elems[ smallest ];
        heap->mh_elems[ smallest ] = heap->mh_elems[ i ];
        heap->mh_elems[ i ] = el;
        mh_heapify(heap, smallest);
    }
}


static void
mh_cleanup (struct lsqpack_min_heap *heap)
{
    free(heap->mh_elems);
}


static int
mh_insert (struct lsqpack_min_heap *heap, void *conn, uint64_t val)
{
    struct lsqpack_min_heap_elem *els;
    struct lsqpack_min_heap_elem el;
    unsigned i;

    if (heap->mh_nelem >= heap->mh_nalloc)
    {
        if (heap->mh_nalloc)
            heap->mh_nalloc *= 2;
        else
            heap->mh_nalloc = 4;
        els = realloc(heap->mh_elems, sizeof(els[0]) * heap->mh_nalloc);
        if (els)
            heap->mh_elems = els;
        else
            return -1;
    }

    heap->mh_elems[ heap->mh_nelem ].mhe_hbrc = conn;
    heap->mh_elems[ heap->mh_nelem ].mhe_max_id  = val;
    ++heap->mh_nelem;

    i = heap->mh_nelem - 1;
    while (i > 0 && heap->mh_elems[ MHE_PARENT(i) ].mhe_max_id >
                                    heap->mh_elems[ i ].mhe_max_id)
    {
        el = heap->mh_elems[ MHE_PARENT(i) ];
        heap->mh_elems[ MHE_PARENT(i) ] = heap->mh_elems[ i ];
        heap->mh_elems[ i ] = el;
        i = MHE_PARENT(i);
    }

    return 0;
}


static void *
mh_pop (struct lsqpack_min_heap *heap, lsqpack_abs_id_t largest_id)
{
    void *hbrc;

    if (heap->mh_nelem == 0 || heap->mh_elems[0].mhe_max_id > largest_id)
        return NULL;

    hbrc = heap->mh_elems[0].mhe_hbrc;
    --heap->mh_nelem;
    if (heap->mh_nelem > 0)
    {
        heap->mh_elems[0] = heap->mh_elems[ heap->mh_nelem ];
        mh_heapify(heap, 0);
    }

    return hbrc;
}


static struct lsqpack_dec_table_entry *
qdec_get_table_entry_rel (const struct lsqpack_dec *dec,
                                            lsqpack_abs_id_t relative_idx)
{
    uintptr_t val;
    unsigned id;

    if (relative_idx < lsqpack_arr_count(&dec->qpd_dyn_table))
    {
        id = lsqpack_arr_count(&dec->qpd_dyn_table) - 1 - relative_idx;
        val = lsqpack_arr_get(&dec->qpd_dyn_table, id);
        return (struct lsqpack_dec_table_entry *) val;
    }
    else
        return NULL;
}


static struct lsqpack_dec_table_entry *
qdec_get_table_entry_abs (const struct lsqpack_dec *dec,
                                                lsqpack_abs_id_t abs_idx)
{
    uintptr_t val;
    unsigned id;

    if (abs_idx > dec->qpd_del_count && abs_idx <= dec->qpd_ins_count)
    {
        id = abs_idx - dec->qpd_del_count - 1;
        assert(id < lsqpack_arr_count(&dec->qpd_dyn_table));
        val = lsqpack_arr_get(&dec->qpd_dyn_table, id);
        return (struct lsqpack_dec_table_entry *) val;
    }
    else
        return NULL;
}


void
lsqpack_dec_init (struct lsqpack_dec *dec, unsigned dyn_table_size,
    unsigned max_risked_streams,
    lsqpack_stream_write_f write_decoder, void *decoder_stream,
    lsqpack_stream_read_f read_header_block,
    lsqpack_stream_wantread_f wantread_header_block,
    void (*header_block_done)(void *, struct lsqpack_header_set *))
{
    memset(dec, 0, sizeof(*dec));
    dec->qpd_max_capacity = dyn_table_size;
    dec->qpd_cur_max_capacity = dyn_table_size;
    dec->qpd_max_risked_streams = max_risked_streams;
    dec->qpd_dec_stream = decoder_stream;
    dec->qpd_write_dec = write_decoder;
    dec->qpd_read_header_block = read_header_block;
    dec->qpd_wantread_header_block = wantread_header_block;
    dec->qpd_header_block_done = header_block_done;
    TAILQ_INIT(&dec->qpd_header_sets);
    TAILQ_INIT(&dec->qpd_hbrcs);
    lsqpack_arr_init(&dec->qpd_dyn_table);
}


static void
qdec_decref_entry (struct lsqpack_dec_table_entry *entry)
{
    --entry->dte_refcnt;
    if (0 == entry->dte_refcnt)
        free(entry);
}


enum {
    DEI_NEXT_INST,
    DEI_WINR_READ_NAME_IDX,
    DEI_WINR_BEGIN_READ_VAL_LEN,
    DEI_WINR_READ_VAL_LEN,
    DEI_WINR_READ_VALUE_PLAIN,
    DEI_WINR_READ_VALUE_HUFFMAN,
    DEI_DUP_READ_IDX,
    DEI_SIZE_UPD_READ_IDX,
    DEI_WONR_READ_NAME_LEN,
    DEI_WONR_READ_NAME_HUFFMAN,
    DEI_WONR_READ_NAME_PLAIN,
    DEI_WONR_BEGIN_READ_VAL_LEN,
    DEI_WONR_READ_VAL_LEN,
    DEI_WONR_READ_VALUE_HUFFMAN,
    DEI_WONR_READ_VALUE_PLAIN,
};


void
lsqpack_dec_cleanup (struct lsqpack_dec *dec)
{
    uintptr_t val;

    /* TODO: free blocked streams */

    /* TODO: mark unreturned header sets */

    if (dec->qpd_enc_state.resume >= DEI_WINR_READ_NAME_IDX
            && dec->qpd_enc_state.resume <= DEI_WINR_READ_VALUE_HUFFMAN)
    {
        if (dec->qpd_enc_state.ctx_u.with_namref.entry)
            free(dec->qpd_enc_state.ctx_u.with_namref.entry);
    }
    else if (dec->qpd_enc_state.resume >= DEI_WONR_READ_NAME_LEN
            && dec->qpd_enc_state.resume <= DEI_WONR_READ_VALUE_PLAIN)
    {
        if (dec->qpd_enc_state.ctx_u.wo_namref.entry)
            free(dec->qpd_enc_state.ctx_u.wo_namref.entry);
    }

    while (lsqpack_arr_count(&dec->qpd_dyn_table) > 0)
    {
        val = lsqpack_arr_pop(&dec->qpd_dyn_table);
        qdec_decref_entry((struct lsqpack_dec_table_entry *) val);
    }
    lsqpack_arr_cleanup(&dec->qpd_dyn_table);
    mh_cleanup(&dec->qpd_blocked_headers);
}


struct header_block_read_ctx
{
    TAILQ_ENTRY(header_block_read_ctx)  hbrc_next;
    void                               *hbrc_stream;
    size_t                              hbrc_size;
    lsqpack_abs_id_t                    hbrc_largest_ref;   /* Parsed from prefix */
    lsqpack_abs_id_t                    hbrc_base_index;    /* Parsed from prefix */

    /* The header set that is returned to the user is built up one entry
     * at a time.
     */
    struct lsqpack_header_set          *hbrc_header_set;
    unsigned                            hbrc_nalloced_headers;

    /* There are two parsing phases: reading the prefix and reading the
     * instruction stream.
     */
    enum read_header_status           (*hbrc_parse) (struct lsqpack_dec *,
            struct header_block_read_ctx *, const unsigned char *, size_t);

    enum {
        HBRC_HAVE_LARGEST_REF   = 1 << 0,
        HBRC_BLOCKED            = 1 << 1,
    }                                   hbrc_flags;

    /* First we read the largest reference to see whether the header block
     * is blocked.  hbrc_lr_min_sz is the minimum number of bytes that a
     * valid largest reference could possibly be expressed in. hbrc_lr_nread
     * is the number of bytes read so far.
     */
    unsigned char                       hbrc_lr_min_sz;
    unsigned char                       hbrc_lr_nread;

    union {
        struct {
            enum {
                PREFIX_STATE_BEGIN_READING_LARGEST_REF,
                PREFIX_STATE_READ_LARGEST_REF,
                PREFIX_STATE_BEGIN_READING_BASE_IDX,
                PREFIX_STATE_READ_DELTA_BASE_IDX,
            }                                               state;
            union {
                /* Largest reference */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    uint64_t                        value;
                }                                               lar_ref;
                /* Delta base index */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    uint64_t                        value;
                    int                             sign;
                }                                               base_idx;
            }                                               u;
        }                                       prefix;
        struct {
            enum {
                DATA_STATE_NEXT_INSTRUCTION,
                DATA_STATE_READ_IHF_IDX,
                DATA_STATE_READ_IPBI_IDX,
                DATA_STATE_READ_LFINR_IDX,
                DATA_STATE_BEGIN_READ_LFINR_VAL_LEN,
                DATA_STATE_READ_LFINR_VAL_LEN,
                DATA_STATE_LFINR_READ_VAL_HUFFMAN,
                DATA_STATE_LFINR_READ_VAL_PLAIN,
                DATA_STATE_READ_LFONR_NAME_LEN,
                DATA_STATE_READ_LFONR_NAME_HUFFMAN,
                DATA_STATE_READ_LFONR_NAME_PLAIN,
                DATA_STATE_BEGIN_READ_LFONR_VAL_LEN,
                DATA_STATE_READ_LFONR_VAL_LEN,
                DATA_STATE_READ_LFONR_VAL_HUFFMAN,
                DATA_STATE_READ_LFONR_VAL_PLAIN,
                DATA_STATE_READ_LFPBNR_IDX,
                DATA_STATE_BEGIN_READ_LFPBNR_VAL_LEN,
                DATA_STATE_READ_LFPBNR_VAL_LEN,
                DATA_STATE_LFPBNR_READ_VAL_HUFFMAN,
                DATA_STATE_LFPBNR_READ_VAL_PLAIN,
            }                                               state;

            union
            {
                /* Indexed Header Field */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    uint64_t                        value;
                    int                             is_static;
                }                                           ihf;

                /* Literal Header Field With Name Reference */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    struct lsqpack_huff_decode_state
                                                    dec_huff_state;
                    union {
                        unsigned                        static_idx;
                        struct lsqpack_dec_table_entry *dyn_entry;
                    }                               name_ref;
                    char                           *value;
                    unsigned                        nalloc;
                    unsigned                        val_len;
                    unsigned                        val_off;
                    unsigned                        nread;
                    int                             is_static;
                    int                             is_never;
                    int                             is_huffman;
                }                                           lfinr;

                /* Literal Header Field Without Name Reference */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    struct lsqpack_huff_decode_state
                                                    dec_huff_state;
                    /* name holds both name and value */
                    char                           *name;
                    unsigned                        nalloc;
                    unsigned                        name_len;
                    unsigned                        value_len;
                    unsigned                        str_off;
                    unsigned                        str_len;
                    unsigned                        nread;
                    int                             is_never;
                    int                             is_huffman;
                }                                           lfonr;

                /* Indexed Header Field With Post-Base Index */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    uint64_t                        value;
                }                                           ipbi;

                /* Literal Header Field With Post-Base Name Reference */
                struct {
                    struct lsqpack_dec_int_state    dec_int_state;
                    struct lsqpack_huff_decode_state
                                                    dec_huff_state;
                    struct lsqpack_dec_table_entry *reffed_entry;
                    char                           *value;
                    unsigned                        nalloc;
                    unsigned                        val_len;
                    unsigned                        val_off;
                    unsigned                        nread;
                    int                             is_never;
                    int                             is_huffman;
                }                                           lfpbnr;
            }                                               u;
        }                                       data;
    }                                   hbrc_parse_ctx_u;
};


struct header_internal
{
    struct lsqpack_header            hi_uhead;
    struct lsqpack_dec_table_entry  *hi_entry;
    enum {
        HI_OWN_NAME     = 1 << 0,
        HI_OWN_VALUE    = 1 << 1,
    }                                hi_flags;
};


static struct header_internal *
allocate_hint (struct header_block_read_ctx *read_ctx)
{
    struct lsqpack_header **headers;
    struct header_internal *hint;
    unsigned idx;

    if (!read_ctx->hbrc_header_set)
    {
        read_ctx->hbrc_header_set
                            = calloc(1, sizeof(*read_ctx->hbrc_header_set));
        if (!read_ctx->hbrc_header_set)
            return NULL;
    }

    if (read_ctx->hbrc_header_set->qhs_count >= read_ctx->hbrc_nalloced_headers)
    {
        if (read_ctx->hbrc_nalloced_headers)
            read_ctx->hbrc_nalloced_headers *= 2;
        else
            read_ctx->hbrc_nalloced_headers = 4;
        headers =
            realloc(read_ctx->hbrc_header_set->qhs_headers,
                read_ctx->hbrc_nalloced_headers
                    * sizeof(read_ctx->hbrc_header_set->qhs_headers[0]));
        if (headers)
            read_ctx->hbrc_header_set->qhs_headers = headers;
        else
            return NULL;
    }

    hint = calloc(1, sizeof(*hint));
    if (!hint)
        return NULL;

    idx = read_ctx->hbrc_header_set->qhs_count++;
    read_ctx->hbrc_header_set->qhs_headers[idx] = &hint->hi_uhead;
    return hint;
}


static int
hset_add_static_entry (struct lsqpack_dec *dec,
                    struct header_block_read_ctx *read_ctx, uint64_t idx)
{
    struct header_internal *hint;

    if (idx > 0 && idx <= QPACK_STATIC_TABLE_SIZE
                                    && (hint = allocate_hint(read_ctx)))
    {
        hint->hi_uhead.qh_name      = static_table[idx - 1].name;
        hint->hi_uhead.qh_value     = static_table[idx - 1].val;
        hint->hi_uhead.qh_name_len  = static_table[idx - 1].name_len;
        hint->hi_uhead.qh_value_len = static_table[idx - 1].val_len;
        hint->hi_uhead.qh_flags     = 0;
        return 0;
    }
    else
        return -1;
}


static int
hset_add_dynamic_entry (struct lsqpack_dec *dec,
                    struct header_block_read_ctx *read_ctx, uint64_t idx)
{
    struct lsqpack_dec_table_entry *entry;
    struct header_internal *hint;

    if ((entry = qdec_get_table_entry_abs(dec, idx))
                                    && (hint = allocate_hint(read_ctx)))
    {
        hint->hi_uhead.qh_name      = DTE_NAME(entry);
        hint->hi_uhead.qh_value     = DTE_VALUE(entry);
        hint->hi_uhead.qh_name_len  = entry->dte_name_len;
        hint->hi_uhead.qh_value_len = entry->dte_val_len;
        hint->hi_uhead.qh_flags     = 0;
        hint->hi_entry = entry;
        ++entry->dte_refcnt;
        return 0;
    }
    else
        return -1;
}


static int
hset_add_static_nameref_entry (struct header_block_read_ctx *read_ctx,
                unsigned idx, char *value, unsigned val_len, int is_never)
{
    struct header_internal *hint;

    if ((hint = allocate_hint(read_ctx)))
    {
        hint->hi_uhead.qh_name      = static_table[ idx - 1 ].name;
        hint->hi_uhead.qh_name_len  = static_table[ idx - 1 ].name_len;
        hint->hi_uhead.qh_value     = value;
        hint->hi_uhead.qh_value_len = val_len;
        if (is_never)
            hint->hi_uhead.qh_flags = QH_NEVER;
        else
            hint->hi_uhead.qh_flags = 0;
        hint->hi_flags = HI_OWN_VALUE;
        return 0;
    }
    else
        return -1;
}


static int
hset_add_dynamic_nameref_entry (struct header_block_read_ctx *read_ctx,
                struct lsqpack_dec_table_entry *entry, char *value,
                unsigned val_len, int is_never)
{
    struct header_internal *hint;

    if ((hint = allocate_hint(read_ctx)))
    {
        hint->hi_uhead.qh_name      = DTE_NAME(entry);
        hint->hi_uhead.qh_name_len  = entry->dte_name_len;
        hint->hi_uhead.qh_value     = value;
        hint->hi_uhead.qh_value_len = val_len;
        if (is_never)
            hint->hi_uhead.qh_flags = QH_NEVER;
        else
            hint->hi_uhead.qh_flags = 0;
        hint->hi_flags = HI_OWN_VALUE;
        ++entry->dte_refcnt;
        return 0;
    }
    else
        return -1;
}


static int
hset_add_literal_entry (struct header_block_read_ctx *read_ctx,
        char *nameandval, unsigned name_len, unsigned val_len, int is_never)
{
    struct header_internal *hint;

    if ((hint = allocate_hint(read_ctx)))
    {
        hint->hi_uhead.qh_name      = nameandval;
        hint->hi_uhead.qh_name_len  = name_len;
        hint->hi_uhead.qh_value     = nameandval + name_len;
        hint->hi_uhead.qh_value_len = val_len;
        if (is_never)
            hint->hi_uhead.qh_flags = QH_NEVER;
        else
            hint->hi_uhead.qh_flags = 0;
        hint->hi_flags = HI_OWN_NAME;
        return 0;
    }
    else
        return -1;
}


struct decode_el
{
    uint8_t state;
    uint8_t flags;
    uint8_t sym;
};

static const struct decode_el decode_tables[256][16];

static unsigned char *
qdec_huff_dec4bits (uint8_t src_4bits, unsigned char *dst,
                                        struct lsqpack_decode_status *status)
{
    const struct decode_el cur_dec_code =
        decode_tables[status->state][src_4bits];
    if (cur_dec_code.flags & HPACK_HUFFMAN_FLAG_FAIL) {
        return NULL; //failed
    }
    if (cur_dec_code.flags & HPACK_HUFFMAN_FLAG_SYM)
    {
        *dst = cur_dec_code.sym;
        dst++;
    }

    status->state = cur_dec_code.state;
    status->eos = ((cur_dec_code.flags & HPACK_HUFFMAN_FLAG_ACCEPTED) != 0);
    return dst;
}


struct huff_decode_retval
{
    enum
    {
        HUFF_DEC_OK,
        HUFF_DEC_END_SRC,
        HUFF_DEC_END_DST,
        HUFF_DEC_ERROR,
    }                       status;
    unsigned                n_dst;
    unsigned                n_src;
};


struct huff_decode_retval
lsqpack_huff_decode (const unsigned char *src, int src_len,
            unsigned char *dst, int dst_len,
            struct lsqpack_huff_decode_state *state, int final)
{
    const unsigned char *p_src = src;
    const unsigned char *const src_end = src + src_len;
    unsigned char *p_dst = dst;
    unsigned char *dst_end = dst + dst_len;

    switch (state->resume)
    {
    case 1: goto ck1;
    case 2: goto ck2;
    case 3: goto ck3;
    }

    state->status.state = 0;
    state->status.eos   = 1;

  ck1:
    while (p_src != src_end)
    {
        if (p_dst == dst_end)
        {
            state->resume = 2;
            return (struct huff_decode_retval) {
                            .status = HUFF_DEC_END_DST,
                            .n_dst  = dst_len,
                            .n_src  = p_src - src,
            };
        }
  ck2:
        if ((p_dst = qdec_huff_dec4bits(*p_src >> 4, p_dst, &state->status))
                == NULL)
            return (struct huff_decode_retval) { .status = HUFF_DEC_ERROR, };
        if (p_dst == dst_end)
        {
            state->resume = 3;
            return (struct huff_decode_retval) {
                            .status = HUFF_DEC_END_DST,
                            .n_dst  = dst_len,
                            .n_src  = p_src - src,
            };
        }
  ck3:
        if ((p_dst = qdec_huff_dec4bits(*p_src & 0xf, p_dst, &state->status))
                == NULL)
            return (struct huff_decode_retval) { .status = HUFF_DEC_ERROR, };
        ++p_src;
    }

    if (final)
        return (struct huff_decode_retval) {
                    .status = state->status.eos ? HUFF_DEC_OK : HUFF_DEC_ERROR,
                    .n_dst  = p_dst - dst,
                    .n_src  = p_src - src,
        };
    else
    {
        state->resume = 1;
        return (struct huff_decode_retval) {
                    .status = HUFF_DEC_END_SRC,
                    .n_dst  = p_dst - dst,
                    .n_src  = p_src - src,
        };
    }
}


enum read_header_status
{
    RHS_DONE,
    RHS_BLOCKED,
    RHS_NEED,
    RHS_ERROR,
};


static enum read_header_status
parse_header_data (struct lsqpack_dec *dec,
        struct header_block_read_ctx *read_ctx, const unsigned char *buf,
                                                                size_t bufsz)
{
    const unsigned char *const end = buf + bufsz;
    struct huff_decode_retval hdr;
    uint64_t value;
    size_t size;
    char *str;
    unsigned prefix_bits = -1;
    int r;

#define IHF read_ctx->hbrc_parse_ctx_u.data.u.ihf
#define IPBI read_ctx->hbrc_parse_ctx_u.data.u.ipbi
#define LFINR read_ctx->hbrc_parse_ctx_u.data.u.lfinr
#define LFONR read_ctx->hbrc_parse_ctx_u.data.u.lfonr
#define LFPBNR read_ctx->hbrc_parse_ctx_u.data.u.lfpbnr

    while (buf < end)
    {
        switch (read_ctx->hbrc_parse_ctx_u.data.state)
        {
        case DATA_STATE_NEXT_INSTRUCTION:
            if (buf[0] & 0x80)
            {
                prefix_bits = 6;
                IHF.is_static = buf[0] & 0x40;
                IHF.dec_int_state.resume = 0;
                read_ctx->hbrc_parse_ctx_u.data.state
                                                = DATA_STATE_READ_IHF_IDX;
                goto data_state_read_ihf_idx;
            }
            /* Literal Header Field With Name Reference */
            else if (buf[0] & 0x40)
            {
                prefix_bits = 4;
                LFINR.is_never = buf[0] & 0x20;
                LFINR.is_static = buf[0] & 0x10;
                LFINR.dec_int_state.resume = 0;
                LFINR.value = NULL;
                LFINR.name_ref.dyn_entry = NULL;
                read_ctx->hbrc_parse_ctx_u.data.state
                                                = DATA_STATE_READ_LFINR_IDX;
                goto data_state_read_lfinr_idx;
            }
            /* Literal Header Field Without Name Reference */
            else if (buf[0] & 0x20)
            {
                prefix_bits = 3;
                LFONR.is_never = buf[0] & 0x10;
                LFONR.is_huffman = buf[0] & 0x04;
                LFONR.dec_int_state.resume = 0;
                LFONR.name = NULL;
                read_ctx->hbrc_parse_ctx_u.data.state
                                            = DATA_STATE_READ_LFONR_NAME_LEN;
                goto data_state_read_lfonr_name_len;
            }
            /* Indexed Header Field With Post-Base Index */
            else if (buf[0] & 0x10)
            {
                prefix_bits = 4;
                IPBI.dec_int_state.resume = 0;
                read_ctx->hbrc_parse_ctx_u.data.state
                                                = DATA_STATE_READ_IPBI_IDX;
                goto data_state_read_ipbi_idx;
            }
            /* Literal Header Field With Post-Base Name Reference */
            else
            {
                prefix_bits = 3;
                LFPBNR.is_never = buf[0] & 0x08;
                LFPBNR.value = NULL;
                LFPBNR.reffed_entry = NULL;
                read_ctx->hbrc_parse_ctx_u.data.state
                                                = DATA_STATE_READ_LFPBNR_IDX;
                goto data_state_read_lfpbnr_idx;
            }
        case DATA_STATE_READ_IHF_IDX:
  data_state_read_ihf_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &IHF.value,
                                                        &IHF.dec_int_state);
            if (r == 0)
            {
                if (IHF.is_static)
                    r = hset_add_static_entry(dec, read_ctx, IHF.value);
                else
                    r = hset_add_dynamic_entry(dec, read_ctx, IHF.value);
                if (r == 0)
                {
                    read_ctx->hbrc_parse_ctx_u.data.state
                                            = DATA_STATE_NEXT_INSTRUCTION;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
#undef IHF
        case DATA_STATE_READ_LFINR_IDX:
  data_state_read_lfinr_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFINR.dec_int_state);
            if (r == 0)
            {
                if (LFINR.is_static)
                {
                    if (value > 0 && value <= QPACK_STATIC_TABLE_SIZE)
                        LFINR.name_ref.static_idx = value;
                    else
                        return RHS_ERROR;
                }
                else
                {
                    LFINR.name_ref.dyn_entry
                        = qdec_get_table_entry_abs(dec,
                            read_ctx->hbrc_base_index - value);
                    if (LFINR.name_ref.dyn_entry)
                        ++LFINR.name_ref.dyn_entry->dte_refcnt;
                    else
                        return RHS_ERROR;
                }
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_BEGIN_READ_LFINR_VAL_LEN;
                break;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_BEGIN_READ_LFINR_VAL_LEN:
            LFINR.is_huffman = buf[0] & 0x80;
            prefix_bits = 7;
            LFINR.dec_int_state.resume = 0;
            read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_READ_LFINR_VAL_LEN;
            /* Fall-through */
        case DATA_STATE_READ_LFINR_VAL_LEN:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFINR.dec_int_state);
            if (r == 0)
            {
                if (LFINR.is_huffman)
                {
                    LFINR.nalloc = value + value / 2;
                    LFINR.dec_huff_state.resume = 0;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_LFINR_READ_VAL_HUFFMAN;
                }
                else
                {
                    LFINR.nalloc = value;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_LFINR_READ_VAL_PLAIN;
                }
                LFINR.val_len = value;
                LFINR.nread = 0;
                LFINR.val_off = 0;
                LFINR.value = malloc(LFINR.nalloc);
                if (LFINR.value)
                    break;
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_LFINR_READ_VAL_HUFFMAN:
            size = MIN((unsigned) (end - buf), LFINR.val_len - LFINR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) LFINR.value + LFINR.val_off,
                    LFINR.nalloc - LFINR.val_off,
                    &LFINR.dec_huff_state, LFINR.nread + size == LFINR.val_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                LFINR.val_off += hdr.n_dst;
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                if (LFINR.is_static)
                    r = hset_add_static_nameref_entry(read_ctx,
                            LFINR.name_ref.static_idx, LFINR.value,
                            LFINR.val_off, LFINR.is_never);
                else
                {
                    r = hset_add_dynamic_nameref_entry(read_ctx,
                            LFINR.name_ref.dyn_entry, LFINR.value,
                            LFINR.val_off, LFINR.is_never);
                    qdec_decref_entry(LFINR.name_ref.dyn_entry);
                    LFINR.name_ref.dyn_entry = NULL;
                }
                if (r == 0)
                {
                    LFINR.value = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                LFINR.nread += hdr.n_src;
                LFINR.val_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                LFINR.nalloc *= 2;
                str = realloc(LFINR.value, LFINR.nalloc);
                if (!str)
                    return RHS_ERROR;
                LFINR.value = str;
                buf += hdr.n_src;
                LFINR.nread += hdr.n_src;
                LFINR.val_off += hdr.n_dst;
                break;
            default:
                return RHS_ERROR;
            }
            break;
        case DATA_STATE_LFINR_READ_VAL_PLAIN:
            size = MIN((unsigned) (end - buf), LFINR.val_len - LFINR.val_off);
            memcpy(LFINR.value + LFINR.val_off, buf, size);
            LFINR.val_off += size;
            buf += size;
            if (LFINR.val_off == LFINR.val_len)
            {
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                if (LFINR.is_static)
                    r = hset_add_static_nameref_entry(read_ctx,
                            LFINR.name_ref.static_idx, LFINR.value,
                            LFINR.val_off, LFINR.is_never);
                else
                {
                    r = hset_add_dynamic_nameref_entry(read_ctx,
                            LFINR.name_ref.dyn_entry, LFINR.value,
                            LFINR.val_off, LFINR.is_never);
                    qdec_decref_entry(LFINR.name_ref.dyn_entry);
                    LFINR.name_ref.dyn_entry = NULL;
                }
                if (r == 0)
                {
                    LFINR.value = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            break;
#undef LFINR
        case DATA_STATE_READ_LFONR_NAME_LEN:
  data_state_read_lfonr_name_len:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFONR.dec_int_state);
            if (r == 0)
            {
                LFONR.nread = 0;
                LFONR.str_off = 0;
                LFONR.str_len = value;
                LFONR.nalloc = value * 2;
                if (LFONR.is_huffman)
                {
                    LFONR.dec_huff_state.resume = 0;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_READ_LFONR_NAME_HUFFMAN;
                }
                else
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_READ_LFONR_NAME_PLAIN;
                LFONR.name = malloc(LFONR.nalloc);
                if (LFONR.name)
                    break;
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_READ_LFONR_NAME_HUFFMAN:
            size = MIN((unsigned) (end - buf), LFONR.str_len - LFONR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) LFONR.name + LFONR.str_off,
                    LFONR.nalloc - LFONR.str_off,
                    &LFONR.dec_huff_state, LFONR.nread + size == LFONR.str_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                LFONR.name_len = LFONR.str_off + hdr.n_dst;
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_BEGIN_READ_LFONR_VAL_LEN;
                break;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                LFONR.nread += hdr.n_src;
                LFONR.str_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                LFONR.nalloc *= 2;
                str = realloc(LFONR.name, LFONR.nalloc);
                if (!str)
                    return RHS_ERROR;
                LFONR.name = str;
                buf += hdr.n_src;
                LFONR.nread += hdr.n_src;
                LFONR.str_off += hdr.n_dst;
                break;
            default:
                return RHS_ERROR;
            }
            break;
        case DATA_STATE_BEGIN_READ_LFONR_VAL_LEN:
            LFONR.is_huffman = buf[0] & 0x80;
            prefix_bits = 7;
            LFONR.dec_int_state.resume = 0;
            read_ctx->hbrc_parse_ctx_u.data.state
                                            = DATA_STATE_READ_LFONR_VAL_LEN;
            /* Fall-through */
        case DATA_STATE_READ_LFONR_VAL_LEN:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFONR.dec_int_state);
            if (r == 0)
            {
                if (LFONR.is_huffman)
                {
                    LFONR.dec_huff_state.resume = 0;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_READ_LFONR_VAL_HUFFMAN;
                }
                else
                {
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_READ_LFONR_VAL_PLAIN;
                }
                LFONR.nread = 0;
                LFONR.str_off = 0;
                LFONR.str_len = value;
                break;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_READ_LFONR_VAL_HUFFMAN:
            size = MIN((unsigned) (end - buf), LFONR.str_len - LFONR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) LFONR.name + LFONR.name_len
                                                            + LFONR.str_off,
                    LFONR.nalloc - LFONR.name_len - LFONR.str_off,
                    &LFONR.dec_huff_state, LFONR.nread + size == LFONR.str_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                r = hset_add_literal_entry(read_ctx,
                        LFONR.name, LFONR.name_len, LFONR.str_off + hdr.n_dst,
                        LFONR.is_never);
                if (r == 0)
                {
                    LFONR.name = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                LFONR.nread += hdr.n_src;
                LFONR.str_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                LFONR.nalloc *= 2;
                str = realloc(LFONR.name, LFONR.nalloc);
                if (!str)
                    return RHS_ERROR;
                LFONR.name = str;
                buf += hdr.n_src;
                LFONR.nread += hdr.n_src;
                LFONR.str_off += hdr.n_dst;
                break;
            default:
                return RHS_ERROR;
            }
            break;
        case DATA_STATE_READ_LFONR_VAL_PLAIN:
            if (LFONR.nalloc < LFONR.name_len + LFONR.str_len)
            {
                LFONR.nalloc = LFONR.name_len + LFONR.str_len;
                str = realloc(LFONR.name, LFONR.nalloc);
                if (str)
                    LFONR.name = str;
                else
                    return RHS_ERROR;
            }
            size = MIN((unsigned) (end - buf), LFONR.str_len - LFONR.str_off);
            memcpy(LFONR.name + LFONR.name_len + LFONR.str_off, buf, size);
            LFONR.str_off += size;
            buf += size;
            if (LFONR.str_off == LFONR.str_len)
            {
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                r = hset_add_literal_entry(read_ctx,
                        LFONR.name, LFONR.name_len, LFONR.str_off,
                        LFONR.is_never);
                if (0 == r)
                {
                    LFONR.name = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            break;
#undef LFONR
        case DATA_STATE_READ_LFPBNR_IDX:
  data_state_read_lfpbnr_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFPBNR.dec_int_state);
            if (r == 0)
            {
                value += read_ctx->hbrc_base_index + 1;
                if (value > read_ctx->hbrc_largest_ref)
                    return RHS_ERROR;
                LFPBNR.reffed_entry = qdec_get_table_entry_abs(dec, value);
                if (LFPBNR.reffed_entry)
                {
                    ++LFPBNR.reffed_entry->dte_refcnt;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_BEGIN_READ_LFPBNR_VAL_LEN;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_BEGIN_READ_LFPBNR_VAL_LEN:
            LFPBNR.is_huffman = buf[0] & 0x80;
            prefix_bits = 7;
            LFPBNR.dec_int_state.resume = 0;
            read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_READ_LFPBNR_VAL_LEN;
            /* Fall-through */
        case DATA_STATE_READ_LFPBNR_VAL_LEN:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &value,
                                                        &LFPBNR.dec_int_state);
            if (r == 0)
            {
                if (LFPBNR.is_huffman)
                {
                    LFPBNR.nalloc = value + value / 2;
                    LFPBNR.dec_huff_state.resume = 0;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_LFPBNR_READ_VAL_HUFFMAN;
                }
                else
                {
                    LFPBNR.nalloc = value;
                    read_ctx->hbrc_parse_ctx_u.data.state
                                        = DATA_STATE_LFPBNR_READ_VAL_PLAIN;
                }
                LFPBNR.val_len = value;
                LFPBNR.nread = 0;
                LFPBNR.val_off = 0;
                LFPBNR.value = malloc(LFPBNR.nalloc);
                if (LFPBNR.value)
                    break;
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
        case DATA_STATE_LFPBNR_READ_VAL_HUFFMAN:
            size = MIN((unsigned) (end - buf), LFPBNR.val_len - LFPBNR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) LFPBNR.value + LFPBNR.val_off,
                    LFPBNR.nalloc - LFPBNR.val_off,
                    &LFPBNR.dec_huff_state, LFPBNR.nread + size == LFPBNR.val_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                LFPBNR.val_off += hdr.n_dst;
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                r = hset_add_dynamic_nameref_entry(read_ctx,
                                LFPBNR.reffed_entry, LFPBNR.value,
                                LFPBNR.val_off, LFPBNR.is_never);
                qdec_decref_entry(LFPBNR.reffed_entry);
                LFPBNR.reffed_entry = NULL;
                if (r == 0)
                {
                    LFPBNR.value = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                LFPBNR.nread += hdr.n_src;
                LFPBNR.val_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                LFPBNR.nalloc *= 2;
                str = realloc(LFPBNR.value, LFPBNR.nalloc);
                if (!str)
                    return RHS_ERROR;
                LFPBNR.value = str;
                buf += hdr.n_src;
                LFPBNR.nread += hdr.n_src;
                LFPBNR.val_off += hdr.n_dst;
                break;
            default:
                return RHS_ERROR;
            }
            break;
        case DATA_STATE_LFPBNR_READ_VAL_PLAIN:
            size = MIN((unsigned) (end - buf), LFPBNR.val_len - LFPBNR.val_off);
            memcpy(LFPBNR.value + LFPBNR.val_off, buf, size);
            LFPBNR.val_off += size;
            buf += size;
            if (LFPBNR.val_off == LFPBNR.val_len)
            {
                read_ctx->hbrc_parse_ctx_u.data.state
                                    = DATA_STATE_NEXT_INSTRUCTION;
                r = hset_add_dynamic_nameref_entry(read_ctx,
                            LFPBNR.reffed_entry, LFPBNR.value,
                            LFPBNR.val_off, LFPBNR.is_never);
                qdec_decref_entry(LFPBNR.reffed_entry);
                LFPBNR.reffed_entry = NULL;
                if (r == 0)
                {
                    LFPBNR.value = NULL;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            break;
#undef LFPBNR
        case DATA_STATE_READ_IPBI_IDX:
  data_state_read_ipbi_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &IPBI.value,
                                                        &IPBI.dec_int_state);
            if (r == 0)
            {
                r = hset_add_dynamic_entry(dec, read_ctx,
                                        IPBI.value + read_ctx->hbrc_base_index);
                if (r == 0)
                {
                    read_ctx->hbrc_parse_ctx_u.data.state
                                            = DATA_STATE_NEXT_INSTRUCTION;
                    break;
                }
                else
                    return RHS_ERROR;
            }
            else if (r == -1)
                return RHS_NEED;
            else
                return RHS_ERROR;
#undef IPBI
        default:
            assert(0);
            return RHS_ERROR;
        }
    }

    if (read_ctx->hbrc_size > 0)
        return RHS_NEED;
    else if (read_ctx->hbrc_parse_ctx_u.data.state
                                            == DATA_STATE_NEXT_INSTRUCTION)
        return RHS_DONE;
    else
        return RHS_ERROR;

}


static enum read_header_status
parse_header_prefix (struct lsqpack_dec *dec,
        struct header_block_read_ctx *read_ctx, const unsigned char *buf,
                                                                size_t bufsz)
{
    const unsigned char *const end = buf + bufsz;
    unsigned prefix_bits = -1;
    int r;

#define LR read_ctx->hbrc_parse_ctx_u.prefix.u.lar_ref
#define BI read_ctx->hbrc_parse_ctx_u.prefix.u.base_idx

    while (buf < end)
    {
        switch (read_ctx->hbrc_parse_ctx_u.prefix.state)
        {
        case PREFIX_STATE_BEGIN_READING_LARGEST_REF:
            prefix_bits = 8;
            BI.dec_int_state.resume = 0;
            read_ctx->hbrc_parse_ctx_u.prefix.state =
                                            PREFIX_STATE_READ_LARGEST_REF;
            /* Fall-through */
        case PREFIX_STATE_READ_LARGEST_REF:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &LR.value,
                                                        &LR.dec_int_state);
            if (r == 0)
            {
                read_ctx->hbrc_flags |= HBRC_HAVE_LARGEST_REF;
                read_ctx->hbrc_largest_ref = LR.value;
                read_ctx->hbrc_parse_ctx_u.prefix.state
                                        = PREFIX_STATE_BEGIN_READING_BASE_IDX;
                if (LR.value > dec->qpd_ins_count)
                    return RHS_BLOCKED;
                else
                    break;
            }
            else if (r == -1)
            {
                read_ctx->hbrc_lr_nread += buf - end;
                if (read_ctx->hbrc_lr_nread < LSQPACK_UINT64_ENC_SZ)
                    return RHS_NEED;
                else
                    return RHS_ERROR;
            }
            else
                return RHS_ERROR;
        case PREFIX_STATE_BEGIN_READING_BASE_IDX:
            BI.sign = (buf[0] & 0x80) > 0;
            BI.dec_int_state.resume = 0;
            prefix_bits = 7;
            read_ctx->hbrc_parse_ctx_u.prefix.state =
                                            PREFIX_STATE_READ_DELTA_BASE_IDX;
            /* Fall-through */
        case PREFIX_STATE_READ_DELTA_BASE_IDX:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &BI.value,
                                                        &BI.dec_int_state);
            if (r == 0)
            {
                if (BI.sign)
                    read_ctx->hbrc_base_index = read_ctx->hbrc_largest_ref
                                                                    - BI.value;
                else
                    read_ctx->hbrc_base_index = read_ctx->hbrc_largest_ref
                                                                    + BI.value;
                read_ctx->hbrc_parse = parse_header_data;
                read_ctx->hbrc_parse_ctx_u.data.state
                                                = DATA_STATE_NEXT_INSTRUCTION;
                if (end - buf)
                    return parse_header_data(dec, read_ctx, buf, end - buf);
                else
                    return RHS_NEED;
            }
            else if (r == -1)
            {
                return RHS_NEED;
            }
            else
                return RHS_ERROR;
        default:
            assert(0);
            return RHS_ERROR;
        }
    }

#undef LR
#undef BI

    if (read_ctx->hbrc_size > 0)
        return RHS_NEED;
    else
        return RHS_ERROR;
}


static size_t
max_to_read (const struct header_block_read_ctx *read_ctx)
{
    size_t sz;

    if (read_ctx->hbrc_flags & HBRC_HAVE_LARGEST_REF)
        return read_ctx->hbrc_size;
    else
    {
        if (read_ctx->hbrc_lr_min_sz > read_ctx->hbrc_lr_nread)
            sz = read_ctx->hbrc_lr_min_sz - read_ctx->hbrc_lr_nread;
        else
            sz = 1;
        return MIN(sz, read_ctx->hbrc_size);
    }
}


static enum read_header_status
qdec_read_header (struct lsqpack_dec *dec,
                                    struct header_block_read_ctx *read_ctx)
{
    const unsigned char *buf;
    enum read_header_status st;
    size_t n_to_read;
    ssize_t buf_sz;

    while (read_ctx->hbrc_size > 0)
    {
        n_to_read = max_to_read(read_ctx);
        buf_sz = dec->qpd_read_header_block(read_ctx->hbrc_stream, &buf,
                                                                    n_to_read);
        if (buf_sz > 0)
        {
            read_ctx->hbrc_size -= buf_sz;
            st = read_ctx->hbrc_parse(dec, read_ctx, buf, buf_sz);
            if (st == RHS_NEED)
            {
                if (read_ctx->hbrc_size == 0)
                    return RHS_ERROR;
            }
            else
                return st;
        }
        else if (buf_sz == 0)
            return RHS_NEED;
        else
            return RHS_ERROR;
    }

    return RHS_DONE;
}


static void
destroy_header_block_read_ctx (struct lsqpack_dec *dec,
                        struct header_block_read_ctx *read_ctx)
{
    TAILQ_REMOVE(&dec->qpd_hbrcs, read_ctx, hbrc_next);
    free(read_ctx);
}


static int
save_header_block_read_ctx (struct lsqpack_dec *dec,
                        struct header_block_read_ctx *read_ctx)
{
    TAILQ_INSERT_TAIL(&dec->qpd_hbrcs, read_ctx, hbrc_next);
    return 0;
}


static int
stash_blocked_header (struct lsqpack_dec *dec,
                        struct header_block_read_ctx *read_ctx)
{
    if (mh_count(&dec->qpd_blocked_headers) >= dec->qpd_max_risked_streams)
        return -1;
    if (0 == mh_insert(&dec->qpd_blocked_headers, read_ctx,
                                        read_ctx->hbrc_largest_ref))
    {
        read_ctx->hbrc_flags |= HBRC_BLOCKED;
        return 0;
    }
    else
        return -1;
}


static struct header_block_read_ctx *
find_header_block_read_ctx (struct lsqpack_dec *dec, void *stream)
{
    struct header_block_read_ctx *read_ctx;

    TAILQ_FOREACH(read_ctx, &dec->qpd_hbrcs, hbrc_next)
        if (read_ctx->hbrc_stream == stream)
            return read_ctx;

    return NULL;
}


int
lsqpack_dec_header_read (struct lsqpack_dec *dec, void *stream)
{
    struct header_block_read_ctx *read_ctx;
    enum read_header_status st;

    read_ctx = find_header_block_read_ctx(dec, stream);
    if (read_ctx)
    {
        st = qdec_read_header(dec, read_ctx);
        switch (st)
        {
        case RHS_DONE:
            dec->qpd_header_block_done(read_ctx->hbrc_stream,
                                                read_ctx->hbrc_header_set);
            destroy_header_block_read_ctx(dec, read_ctx);
            return 0;
        case RHS_NEED:
            dec->qpd_wantread_header_block(read_ctx->hbrc_stream, 1);
            return 0;
        case RHS_BLOCKED:
            if (0 == stash_blocked_header(dec, read_ctx))
            {
                dec->qpd_wantread_header_block(read_ctx->hbrc_stream, 0);
                return 0;
            }
            else
                return -1;
        default:
            assert(st == RHS_ERROR);
            return -1;
        }
    }
    else
        return -1;
}


int
lsqpack_dec_header_in (struct lsqpack_dec *dec, void *stream,
                                                        size_t header_size)
{
    enum read_header_status st;
    struct header_block_read_ctx *read_ctx;
    struct header_block_read_ctx read_ctx_buf = {
        .hbrc_stream    = stream,
        .hbrc_size      = header_size,
        .hbrc_parse     = parse_header_prefix,
        .hbrc_lr_min_sz = lsqpack_val2len(dec->qpd_del_count, 8),
    };

    st = qdec_read_header(dec, &read_ctx_buf);
    switch (st)
    {
    case RHS_DONE:
        dec->qpd_header_block_done(stream, read_ctx_buf.hbrc_header_set);
        return 0;
    case RHS_NEED:
    case RHS_BLOCKED:
        read_ctx = malloc(sizeof(*read_ctx));
        if (!read_ctx)
            return -1;
        memcpy(read_ctx, &read_ctx_buf, sizeof(*read_ctx));
        if (0 != save_header_block_read_ctx(dec, read_ctx))
        {
            free(read_ctx);
            return -1;
        }
        if (st == RHS_BLOCKED && 0 != stash_blocked_header(dec, read_ctx))
            return -1;
        dec->qpd_wantread_header_block(stream, st == RHS_NEED);
        return 0;
    default:
        assert(st == RHS_ERROR);
        return -1;
    }
}


static void
qdec_drop_oldest_entry (struct lsqpack_dec *dec)
{
    struct lsqpack_dec_table_entry *entry;
    entry = (void *) lsqpack_arr_shift(&dec->qpd_dyn_table);
    dec->qpd_cur_capacity -= DTE_SIZE(entry);
    qdec_decref_entry(entry);
    ++dec->qpd_del_count;
}


static void
qdec_remove_overflow_entries (struct lsqpack_dec *dec)
{
    while (dec->qpd_cur_capacity > dec->qpd_cur_max_capacity)
        qdec_drop_oldest_entry(dec);
}


static void
qdec_update_max_capacity (struct lsqpack_dec *dec, unsigned new_capacity)
{
    dec->qpd_cur_max_capacity = new_capacity;
    qdec_remove_overflow_entries(dec);
}


static void
qdec_process_blocked_headers (struct lsqpack_dec *dec)
{
    struct header_block_read_ctx *read_ctx;

    while ((read_ctx = mh_pop(&dec->qpd_blocked_headers, dec->qpd_ins_count)))
    {
        read_ctx->hbrc_flags &= ~HBRC_BLOCKED;
        dec->qpd_wantread_header_block(read_ctx->hbrc_stream, 1);
    }
}


static int
lsqpack_dec_push_entry (struct lsqpack_dec *dec,
                                        struct lsqpack_dec_table_entry *entry)
{
    if (0 == lsqpack_arr_push(&dec->qpd_dyn_table, (uintptr_t) entry))
    {
        dec->qpd_cur_capacity += DTE_SIZE(entry);
        ++dec->qpd_ins_count;
        qdec_remove_overflow_entries(dec);
        qdec_process_blocked_headers(dec);
        return 0;
    }
    else
        return -1;
}


int
lsqpack_dec_enc_in (struct lsqpack_dec *dec, const unsigned char *buf,
                                                                size_t buf_sz)
{
    const unsigned char *const end = buf + buf_sz;
    struct lsqpack_dec_table_entry *entry, *new_entry;
    struct huff_decode_retval hdr;
    unsigned prefix_bits = -1;
    size_t size;
    int r;

#define WINR dec->qpd_enc_state.ctx_u.with_namref
#define WONR dec->qpd_enc_state.ctx_u.wo_namref
#define DUPL dec->qpd_enc_state.ctx_u.duplicate
#define TBSZ dec->qpd_enc_state.ctx_u.size_update

    while (buf < end)
    {
        switch (dec->qpd_enc_state.resume)
        {
        case DEI_NEXT_INST:
            if (buf[0] & 0x80)
            {
                WINR.is_static = (buf[0] & 0x40) > 0;
                WINR.dec_int_state.resume = 0;
                WINR.reffed_entry = NULL;
                WINR.entry = NULL;
                dec->qpd_enc_state.resume = DEI_WINR_READ_NAME_IDX;
                prefix_bits = 6;
                goto dei_winr_read_name_idx;
            }
            else if (buf[0] & 0x40)
            {
                WONR.is_huffman = (buf[0] & 0x20) > 0;
                WONR.dec_int_state.resume = 0;
                WONR.entry = NULL;
                dec->qpd_enc_state.resume = DEI_WONR_READ_NAME_LEN;
                prefix_bits = 5;
                goto dei_wonr_read_name_idx;
            }
            else if (buf[0] & 0x20)
            {
                TBSZ.dec_int_state.resume = 0;
                dec->qpd_enc_state.resume = DEI_SIZE_UPD_READ_IDX;
                prefix_bits = 5;
                goto dei_size_upd_read_idx;
            }
            else
            {
                DUPL.dec_int_state.resume = 0;
                dec->qpd_enc_state.resume = DEI_DUP_READ_IDX;
                prefix_bits = 5;
                goto dei_dup_read_idx;
            }
        case DEI_WINR_READ_NAME_IDX:
  dei_winr_read_name_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits,
                                    &WINR.name_idx, &WINR.dec_int_state);
            if (r == 0)
            {
                if (WINR.is_static)
                {
                    if (WINR.name_idx < 1
                                || WINR.name_idx > QPACK_STATIC_TABLE_SIZE)
                        return -1;
                    WINR.reffed_entry = NULL;
                }
                else
                {
                    WINR.reffed_entry = qdec_get_table_entry_rel(dec,
                                                                WINR.name_idx);
                    if (!WINR.reffed_entry)
                        return -1;
                    ++WINR.reffed_entry->dte_refcnt;
                }
                dec->qpd_enc_state.resume = DEI_WINR_BEGIN_READ_VAL_LEN;
                break;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
        case DEI_WINR_BEGIN_READ_VAL_LEN:
            WINR.is_huffman = (buf[0] & 0x80) > 0;
            WINR.dec_int_state.resume = 0;
            dec->qpd_enc_state.resume = DEI_WINR_READ_VAL_LEN;
            prefix_bits = 7;
            /* fall-through */
        case DEI_WINR_READ_VAL_LEN:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &WINR.val_len,
                                                        &WINR.dec_int_state);
            if (r == 0)
            {
                if (WINR.is_static)
                {
                    WINR.name_len = static_table[WINR.name_idx - 1].name_len;
                    WINR.name = static_table[WINR.name_idx - 1].name;
                }
                else
                {
                    WINR.name_len = WINR.reffed_entry->dte_name_len;
                    WINR.name = DTE_NAME(WINR.reffed_entry);
                }
                if (WINR.is_huffman)
                    WINR.alloced_val_len = WINR.val_len + WINR.val_len / 4;
                else
                    WINR.alloced_val_len = WINR.val_len;
                WINR.entry = malloc(sizeof(*WINR.entry) + WINR.name_len
                                                    + WINR.alloced_val_len);
                if (!WINR.entry)
                    return -1;
                WINR.entry->dte_name_len = WINR.name_len;
                WINR.nread = 0;
                WINR.val_off = 0;
                if (WINR.is_huffman)
                {
                    dec->qpd_enc_state.resume = DEI_WINR_READ_VALUE_HUFFMAN;
                    WINR.dec_huff_state.resume = 0;
                }
                else
                    dec->qpd_enc_state.resume = DEI_WINR_READ_VALUE_PLAIN;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
            break;
        case DEI_WINR_READ_VALUE_HUFFMAN:
            size = MIN((unsigned) (end - buf), WINR.val_len - WINR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) DTE_VALUE(WINR.entry) + WINR.val_off,
                    WINR.alloced_val_len - WINR.val_off,
                    &WINR.dec_huff_state, WINR.nread + size == WINR.val_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                WINR.entry->dte_val_len = WINR.val_off + hdr.n_dst;
                WINR.entry->dte_refcnt = 1;
                memcpy(DTE_NAME(WINR.entry), WINR.name, WINR.name_len);
                if (WINR.reffed_entry)
                {
                    qdec_decref_entry(WINR.reffed_entry);
                    WINR.reffed_entry = NULL;
                }
                r = lsqpack_dec_push_entry(dec, WINR.entry);
                if (0 == r)
                {
                    dec->qpd_enc_state.resume = 0;
                    WINR.entry = NULL;
                    break;
                }
                qdec_decref_entry(WINR.entry);
                WINR.entry = NULL;
                return -1;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                WINR.nread += hdr.n_src;
                WINR.val_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                WINR.alloced_val_len *= 2;
                entry = realloc(WINR.entry, sizeof(*WINR.entry)
                                        + WINR.name_len + WINR.alloced_val_len);
                if (!entry)
                    return -1;
                WINR.entry = entry;
                buf += hdr.n_src;
                WINR.nread += hdr.n_src;
                WINR.val_off += hdr.n_dst;
                break;
            default:
                return -1;
            }
            break;
        case DEI_WINR_READ_VALUE_PLAIN:
            if (WINR.alloced_val_len < WINR.val_len)
            {
                WINR.alloced_val_len = WINR.val_len;
                entry = realloc(WINR.entry, sizeof(*WINR.entry)
                                                        + WINR.alloced_val_len);
                if (entry)
                    WINR.entry = entry;
                else
                    return -1;
            }
            size = MIN((unsigned) (end - buf), WINR.val_len - WINR.val_off);
            memcpy(DTE_VALUE(WINR.entry) + WINR.val_off, buf, size);
            WINR.val_off += size;
            buf += size;
            if (WINR.val_off == WINR.val_len)
            {
                WINR.entry->dte_val_len = WINR.val_off;
                WINR.entry->dte_refcnt = 1;
                memcpy(DTE_NAME(WINR.entry), WINR.name, WINR.name_len);
                if (WINR.reffed_entry)
                {
                    qdec_decref_entry(WINR.reffed_entry);
                    WINR.reffed_entry = NULL;
                }
                r = lsqpack_dec_push_entry(dec, WINR.entry);
                if (0 == r)
                {
                    dec->qpd_enc_state.resume = 0;
                    WINR.entry = NULL;
                    break;
                }
                qdec_decref_entry(WINR.entry);
                WINR.entry = NULL;
                return -1;
            }
            break;
        case DEI_WONR_READ_NAME_LEN:
  dei_wonr_read_name_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &WONR.str_len,
                                                        &DUPL.dec_int_state);
            if (r == 0)
            {
                /* TODO: Check that the name is not larger than the max dynamic
                 * table capacity, for example.
                 */
                WONR.alloced_len = WONR.str_len * 2;
                size = sizeof(*new_entry) + WONR.alloced_len;
                WONR.entry = malloc(size);
                if (!WONR.entry)
                    return -1;
                WONR.nread = 0;
                WONR.str_off = 0;
                if (WONR.is_huffman)
                {
                    dec->qpd_enc_state.resume = DEI_WONR_READ_NAME_HUFFMAN;
                    WONR.dec_huff_state.resume = 0;
                }
                else
                    dec->qpd_enc_state.resume = DEI_WONR_READ_NAME_PLAIN;
                break;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
        case DEI_WONR_READ_NAME_HUFFMAN:
            size = MIN((unsigned) (end - buf), WONR.str_len - WONR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) DTE_NAME(WONR.entry) + WONR.str_off,
                    WONR.alloced_len - WONR.str_off,
                    &WONR.dec_huff_state, WONR.nread + size == WONR.str_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                WONR.entry->dte_name_len = WONR.str_off + hdr.n_dst;
                dec->qpd_enc_state.resume = DEI_WONR_BEGIN_READ_VAL_LEN;
                break;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                WONR.nread += hdr.n_src;
                WONR.str_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                WONR.alloced_len *= 2;
                entry = realloc(WONR.entry, sizeof(*WONR.entry)
                                                        + WONR.alloced_len);
                if (!entry)
                    return -1;
                WONR.entry = entry;
                buf += hdr.n_src;
                WONR.nread += hdr.n_src;
                WONR.str_off += hdr.n_dst;
                break;
            default:
                return -1;
            }
            break;
        case DEI_WONR_READ_NAME_PLAIN:
            if (WONR.alloced_len < WONR.str_len)
            {
                WONR.alloced_len = WONR.str_len * 2;
                entry = realloc(WONR.entry, sizeof(*WONR.entry)
                                                        + WONR.alloced_len);
                if (entry)
                    WONR.entry = entry;
                else
                    return -1;
            }
            size = MIN((unsigned) (end - buf), WONR.str_len - WONR.str_off);
            memcpy(DTE_NAME(WONR.entry) + WONR.str_off, buf, size);
            WONR.str_off += size;
            buf += size;
            if (WONR.str_off == WONR.str_len)
            {
                WONR.entry->dte_name_len = WONR.str_off;
                dec->qpd_enc_state.resume = DEI_WONR_BEGIN_READ_VAL_LEN;
            }
            break;
        case DEI_WONR_BEGIN_READ_VAL_LEN:
            WONR.is_huffman = (buf[0] & 0x80) > 0;
            WONR.dec_int_state.resume = 0;
            dec->qpd_enc_state.resume = DEI_WONR_READ_VAL_LEN;
            prefix_bits = 7;
            /* fall-through */
        case DEI_WONR_READ_VAL_LEN:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &WONR.str_len,
                                                        &WONR.dec_int_state);
            if (r == 0)
            {
                WONR.nread = 0;
                WONR.str_off = 0;
                if (WONR.is_huffman)
                {
                    dec->qpd_enc_state.resume = DEI_WONR_READ_VALUE_HUFFMAN;
                    WONR.dec_huff_state.resume = 0;
                }
                else
                    dec->qpd_enc_state.resume = DEI_WONR_READ_VALUE_PLAIN;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
            break;
        case DEI_WONR_READ_VALUE_HUFFMAN:
            size = MIN((unsigned) (end - buf), WONR.str_len - WONR.nread);
            hdr = lsqpack_huff_decode(buf, size,
                    (unsigned char *) DTE_VALUE(WONR.entry) + WONR.str_off,
                    WONR.alloced_len - WONR.entry->dte_name_len - WONR.str_off,
                    &WONR.dec_huff_state, WONR.nread + size == WONR.str_len);
            switch (hdr.status)
            {
            case HUFF_DEC_OK:
                buf += hdr.n_src;
                WONR.entry->dte_val_len = WONR.str_off + hdr.n_dst;
                WONR.entry->dte_refcnt = 1;
                r = lsqpack_dec_push_entry(dec, WONR.entry);
                if (0 == r)
                {
                    dec->qpd_enc_state.resume = 0;
                    WONR.entry = NULL;
                    break;
                }
                qdec_decref_entry(WONR.entry);
                WONR.entry = NULL;
                return -1;
            case HUFF_DEC_END_SRC:
                buf += hdr.n_src;
                WONR.nread += hdr.n_src;
                WONR.str_off += hdr.n_dst;
                break;
            case HUFF_DEC_END_DST:
                WONR.alloced_len *= 2;
                entry = realloc(WONR.entry, sizeof(*WONR.entry)
                                                        + WONR.alloced_len);
                if (!entry)
                    return -1;
                WONR.entry = entry;
                buf += hdr.n_src;
                WONR.nread += hdr.n_src;
                WONR.str_off += hdr.n_dst;
                break;
            default:
                return -1;
            }
            break;
        case DEI_WONR_READ_VALUE_PLAIN:
            if (WONR.alloced_len < WONR.entry->dte_name_len + WONR.str_len)
            {
                WONR.alloced_len = WONR.entry->dte_name_len + WONR.str_len;
                entry = realloc(WONR.entry, sizeof(*WONR.entry)
                                                        + WONR.alloced_len);
                if (entry)
                    WONR.entry = entry;
                else
                    return -1;
            }
            size = MIN((unsigned) (end - buf), WONR.str_len - WONR.str_off);
            memcpy(DTE_VALUE(WONR.entry) + WONR.str_off, buf, size);
            WONR.str_off += size;
            buf += size;
            if (WONR.str_off == WONR.str_len)
            {
                WONR.entry->dte_val_len = WONR.str_off;
                WONR.entry->dte_refcnt = 1;
                r = lsqpack_dec_push_entry(dec, WONR.entry);
                if (0 == r)
                {
                    dec->qpd_enc_state.resume = 0;
                    WONR.entry = NULL;
                    break;
                }
                qdec_decref_entry(WONR.entry);
                WONR.entry = NULL;
                return -1;
            }
            break;
        case DEI_DUP_READ_IDX:
  dei_dup_read_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &DUPL.index,
                                                        &DUPL.dec_int_state);
            if (r == 0)
            {
                entry = qdec_get_table_entry_rel(dec, DUPL.index);
                if (!entry)
                    return -1;
                size = sizeof(*new_entry) + entry->dte_name_len
                                                        + entry->dte_val_len;
                new_entry = malloc(size);
                if (!new_entry)
                    return -1;
                memcpy(new_entry, entry, size);
                new_entry->dte_refcnt = 1;
                if (0 == lsqpack_dec_push_entry(dec, new_entry))
                {
                    dec->qpd_enc_state.resume = 0;
                    break;
                }
                qdec_decref_entry(new_entry);
                return -1;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
        case DEI_SIZE_UPD_READ_IDX:
  dei_size_upd_read_idx:
            r = lsqpack_dec_int(&buf, end, prefix_bits, &TBSZ.new_size,
                                                        &TBSZ.dec_int_state);
            if (r == 0)
            {
                if (TBSZ.new_size <= dec->qpd_max_capacity)
                {
                    dec->qpd_enc_state.resume = 0;
                    qdec_update_max_capacity(dec, TBSZ.new_size);
                    break;
                }
                else
                    return -1;
            }
            else if (r == -1)
                return 0;
            else
                return -1;
        default:
            assert(0);
        }
    }

#undef WINR
#undef WONR
#undef DUPL
#undef TBSZ

    return 0;
}


void
lsqpack_dec_set_max_capacity (struct lsqpack_dec *dec, unsigned max_capacity)
{
    dec->qpd_max_capacity = max_capacity;
    qdec_update_max_capacity(dec, max_capacity);
}


void
lsqpack_dec_print_table (const struct lsqpack_dec *dec, FILE *out)
{
    const struct lsqpack_dec_table_entry *entry;
    uintptr_t val;
    unsigned n;

    fprintf(out, "Printing decoder table state.\n");
    fprintf(out, "Insertions: %u; deletions: %u.\n", dec->qpd_ins_count,
                                                        dec->qpd_del_count);
    fprintf(out, "Max capacity: %u; current capacity: %u\n",
        dec->qpd_cur_max_capacity, dec->qpd_cur_capacity);
    for (n = 0; n < lsqpack_arr_count(&dec->qpd_dyn_table); ++n)
    {
        val = lsqpack_arr_get(&dec->qpd_dyn_table, n);
        entry = (void *) val;
        fprintf(out, "%u) %.*s: %.*s\n", dec->qpd_del_count + 1 + n,
            entry->dte_name_len, DTE_NAME(entry),
            entry->dte_val_len, DTE_VALUE(entry));
    }
    fprintf(out, "\n");
}


void
lsqpack_dec_destroy_header_set (struct lsqpack_header_set *set)
{
    struct header_internal *hint;
    unsigned n;

    for (n = 0; n < set->qhs_count; ++n)
    {
        hint = (struct header_internal *) set->qhs_headers[n];
        if (hint->hi_entry)
            qdec_decref_entry(hint->hi_entry);
        if (hint->hi_flags & HI_OWN_NAME)
            free((char *) hint->hi_uhead.qh_name);
        if (hint->hi_flags & HI_OWN_VALUE)
            free((char *) hint->hi_uhead.qh_value);
        free(hint);
    }
    free(set->qhs_headers);
    free(set);
}


#define NAME_VAL(a, b) (a), (b), sizeof(a) - 1, sizeof(b) - 1,

static const struct static_table_entry static_table[QPACK_STATIC_TABLE_SIZE] =
{
    { NAME_VAL(":authority",                    "") },
    { NAME_VAL(":method",                       "GET") },
    { NAME_VAL(":method",                       "POST") },
    { NAME_VAL(":path",                         "/") },
    { NAME_VAL(":path",                         "/index.html") },
    { NAME_VAL(":scheme",                       "http") },
    { NAME_VAL(":scheme",                       "https") },
    { NAME_VAL(":status",                       "200") },
    { NAME_VAL(":status",                       "204") },
    { NAME_VAL(":status",                       "206") },
    { NAME_VAL(":status",                       "304") },
    { NAME_VAL(":status",                       "400") },
    { NAME_VAL(":status",                       "404") },
    { NAME_VAL(":status",                       "500") },
    { NAME_VAL("accept-charset",                "") },
    { NAME_VAL("accept-encoding",               "gzip, deflate") },
    { NAME_VAL("accept-language",               "") },
    { NAME_VAL("accept-ranges",                 "") },
    { NAME_VAL("accept",                        "") },
    { NAME_VAL("access-control-allow-origin",   "") },
    { NAME_VAL("age",                           "") },
    { NAME_VAL("allow",                         "") },
    { NAME_VAL("authorization",                 "") },
    { NAME_VAL("cache-control",                 "") },
    { NAME_VAL("content-disposition",           "") },
    { NAME_VAL("content-encoding",              "") },
    { NAME_VAL("content-language",              "") },
    { NAME_VAL("content-length",                "") },
    { NAME_VAL("content-location",              "") },
    { NAME_VAL("content-range",                 "") },
    { NAME_VAL("content-type",                  "") },
    { NAME_VAL("cookie",                        "") },
    { NAME_VAL("date",                          "") },
    { NAME_VAL("etag",                          "") },
    { NAME_VAL("expect",                        "") },
    { NAME_VAL("expires",                       "") },
    { NAME_VAL("from",                          "") },
    { NAME_VAL("host",                          "") },
    { NAME_VAL("if-match",                      "") },
    { NAME_VAL("if-modified-since",             "") },
    { NAME_VAL("if-none-match",                 "") },
    { NAME_VAL("if-range",                      "") },
    { NAME_VAL("if-unmodified-since",           "") },
    { NAME_VAL("last-modified",                 "") },
    { NAME_VAL("link",                          "") },
    { NAME_VAL("location",                      "") },
    { NAME_VAL("max-forwards",                  "") },
    { NAME_VAL("proxy-authenticate",            "") },
    { NAME_VAL("proxy-authorization",           "") },
    { NAME_VAL("range",                         "") },
    { NAME_VAL("referer",                       "") },
    { NAME_VAL("refresh",                       "") },
    { NAME_VAL("retry-after",                   "") },
    { NAME_VAL("server",                        "") },
    { NAME_VAL("set-cookie",                    "") },
    { NAME_VAL("strict-transport-security",     "") },
    { NAME_VAL("transfer-encoding",             "") },
    { NAME_VAL("user-agent",                    "") },
    { NAME_VAL("vary",                          "") },
    { NAME_VAL("via",                           "") },
    { NAME_VAL("www-authenticate",              "") }
};


static const struct encode_el encode_table[257] =
{
    {     0x1ff8,    13},    //        (  0)
    {   0x7fffd8,    23},    //        (  1)
    {  0xfffffe2,    28},    //        (  2)
    {  0xfffffe3,    28},    //        (  3)
    {  0xfffffe4,    28},    //        (  4)
    {  0xfffffe5,    28},    //        (  5)
    {  0xfffffe6,    28},    //        (  6)
    {  0xfffffe7,    28},    //        (  7)
    {  0xfffffe8,    28},    //        (  8)
    {   0xffffea,    24},    //        (  9)
    { 0x3ffffffc,    30},    //        ( 10)
    {  0xfffffe9,    28},    //        ( 11)
    {  0xfffffea,    28},    //        ( 12)
    { 0x3ffffffd,    30},    //        ( 13)
    {  0xfffffeb,    28},    //        ( 14)
    {  0xfffffec,    28},    //        ( 15)
    {  0xfffffed,    28},    //        ( 16)
    {  0xfffffee,    28},    //        ( 17)
    {  0xfffffef,    28},    //        ( 18)
    {  0xffffff0,    28},    //        ( 19)
    {  0xffffff1,    28},    //        ( 20)
    {  0xffffff2,    28},    //        ( 21)
    { 0x3ffffffe,    30},    //        ( 22)
    {  0xffffff3,    28},    //        ( 23)
    {  0xffffff4,    28},    //        ( 24)
    {  0xffffff5,    28},    //        ( 25)
    {  0xffffff6,    28},    //        ( 26)
    {  0xffffff7,    28},    //        ( 27)
    {  0xffffff8,    28},    //        ( 28)
    {  0xffffff9,    28},    //        ( 29)
    {  0xffffffa,    28},    //        ( 30)
    {  0xffffffb,    28},    //        ( 31)
    {       0x14,     6},    //    ' ' ( 32)
    {      0x3f8,    10},    //    '!' ( 33)
    {      0x3f9,    10},    //    '"' ( 34)
    {      0xffa,    12},    //    '#' ( 35)
    {     0x1ff9,    13},    //    '$' ( 36)
    {       0x15,     6},    //    '%' ( 37)
    {       0xf8,     8},    //    '&' ( 38)
    {      0x7fa,    11},    //    ''' ( 39)
    {      0x3fa,    10},    //    '(' ( 40)
    {      0x3fb,    10},    //    ')' ( 41)
    {       0xf9,     8},    //    '*' ( 42)
    {      0x7fb,    11},    //    '+' ( 43)
    {       0xfa,     8},    //    ',' ( 44)
    {       0x16,     6},    //    '-' ( 45)
    {       0x17,     6},    //    '.' ( 46)
    {       0x18,     6},    //    '/' ( 47)
    {        0x0,     5},    //    '0' ( 48)
    {        0x1,     5},    //    '1' ( 49)
    {        0x2,     5},    //    '2' ( 50)
    {       0x19,     6},    //    '3' ( 51)
    {       0x1a,     6},    //    '4' ( 52)
    {       0x1b,     6},    //    '5' ( 53)
    {       0x1c,     6},    //    '6' ( 54)
    {       0x1d,     6},    //    '7' ( 55)
    {       0x1e,     6},    //    '8' ( 56)
    {       0x1f,     6},    //    '9' ( 57)
    {       0x5c,     7},    //    ':' ( 58)
    {       0xfb,     8},    //    ';' ( 59)
    {     0x7ffc,    15},    //    '<' ( 60)
    {       0x20,     6},    //    '=' ( 61)
    {      0xffb,    12},    //    '>' ( 62)
    {      0x3fc,    10},    //    '?' ( 63)
    {     0x1ffa,    13},    //    '@' ( 64)
    {       0x21,     6},    //    'A' ( 65)
    {       0x5d,     7},    //    'B' ( 66)
    {       0x5e,     7},    //    'C' ( 67)
    {       0x5f,     7},    //    'D' ( 68)
    {       0x60,     7},    //    'E' ( 69)
    {       0x61,     7},    //    'F' ( 70)
    {       0x62,     7},    //    'G' ( 71)
    {       0x63,     7},    //    'H' ( 72)
    {       0x64,     7},    //    'I' ( 73)
    {       0x65,     7},    //    'J' ( 74)
    {       0x66,     7},    //    'K' ( 75)
    {       0x67,     7},    //    'L' ( 76)
    {       0x68,     7},    //    'M' ( 77)
    {       0x69,     7},    //    'N' ( 78)
    {       0x6a,     7},    //    'O' ( 79)
    {       0x6b,     7},    //    'P' ( 80)
    {       0x6c,     7},    //    'Q' ( 81)
    {       0x6d,     7},    //    'R' ( 82)
    {       0x6e,     7},    //    'S' ( 83)
    {       0x6f,     7},    //    'T' ( 84)
    {       0x70,     7},    //    'U' ( 85)
    {       0x71,     7},    //    'V' ( 86)
    {       0x72,     7},    //    'W' ( 87)
    {       0xfc,     8},    //    'X' ( 88)
    {       0x73,     7},    //    'Y' ( 89)
    {       0xfd,     8},    //    'Z' ( 90)
    {     0x1ffb,    13},    //    '[' ( 91)
    {    0x7fff0,    19},    //    '\' ( 92)
    {     0x1ffc,    13},    //    ']' ( 93)
    {     0x3ffc,    14},    //    '^' ( 94)
    {       0x22,     6},    //    '_' ( 95)
    {     0x7ffd,    15},    //    '`' ( 96)
    {        0x3,     5},    //    'a' ( 97)
    {       0x23,     6},    //    'b' ( 98)
    {        0x4,     5},    //    'c' ( 99)
    {       0x24,     6},    //    'd' (100)
    {        0x5,     5},    //    'e' (101)
    {       0x25,     6},    //    'f' (102)
    {       0x26,     6},    //    'g' (103)
    {       0x27,     6},    //    'h' (104)
    {        0x6,     5},    //    'i' (105)
    {       0x74,     7},    //    'j' (106)
    {       0x75,     7},    //    'k' (107)
    {       0x28,     6},    //    'l' (108)
    {       0x29,     6},    //    'm' (109)
    {       0x2a,     6},    //    'n' (110)
    {        0x7,     5},    //    'o' (111)
    {       0x2b,     6},    //    'p' (112)
    {       0x76,     7},    //    'q' (113)
    {       0x2c,     6},    //    'r' (114)
    {        0x8,     5},    //    's' (115)
    {        0x9,     5},    //    't' (116)
    {       0x2d,     6},    //    'u' (117)
    {       0x77,     7},    //    'v' (118)
    {       0x78,     7},    //    'w' (119)
    {       0x79,     7},    //    'x' (120)
    {       0x7a,     7},    //    'y' (121)
    {       0x7b,     7},    //    'z' (122)
    {     0x7ffe,    15},    //    '{' (123)
    {      0x7fc,    11},    //    '|' (124)
    {     0x3ffd,    14},    //    '}' (125)
    {     0x1ffd,    13},    //    '~' (126)
    {  0xffffffc,    28},    //        (127)
    {    0xfffe6,    20},    //        (128)
    {   0x3fffd2,    22},    //        (129)
    {    0xfffe7,    20},    //        (130)
    {    0xfffe8,    20},    //        (131)
    {   0x3fffd3,    22},    //        (132)
    {   0x3fffd4,    22},    //        (133)
    {   0x3fffd5,    22},    //        (134)
    {   0x7fffd9,    23},    //        (135)
    {   0x3fffd6,    22},    //        (136)
    {   0x7fffda,    23},    //        (137)
    {   0x7fffdb,    23},    //        (138)
    {   0x7fffdc,    23},    //        (139)
    {   0x7fffdd,    23},    //        (140)
    {   0x7fffde,    23},    //        (141)
    {   0xffffeb,    24},    //        (142)
    {   0x7fffdf,    23},    //        (143)
    {   0xffffec,    24},    //        (144)
    {   0xffffed,    24},    //        (145)
    {   0x3fffd7,    22},    //        (146)
    {   0x7fffe0,    23},    //        (147)
    {   0xffffee,    24},    //        (148)
    {   0x7fffe1,    23},    //        (149)
    {   0x7fffe2,    23},    //        (150)
    {   0x7fffe3,    23},    //        (151)
    {   0x7fffe4,    23},    //        (152)
    {   0x1fffdc,    21},    //        (153)
    {   0x3fffd8,    22},    //        (154)
    {   0x7fffe5,    23},    //        (155)
    {   0x3fffd9,    22},    //        (156)
    {   0x7fffe6,    23},    //        (157)
    {   0x7fffe7,    23},    //        (158)
    {   0xffffef,    24},    //        (159)
    {   0x3fffda,    22},    //        (160)
    {   0x1fffdd,    21},    //        (161)
    {    0xfffe9,    20},    //        (162)
    {   0x3fffdb,    22},    //        (163)
    {   0x3fffdc,    22},    //        (164)
    {   0x7fffe8,    23},    //        (165)
    {   0x7fffe9,    23},    //        (166)
    {   0x1fffde,    21},    //        (167)
    {   0x7fffea,    23},    //        (168)
    {   0x3fffdd,    22},    //        (169)
    {   0x3fffde,    22},    //        (170)
    {   0xfffff0,    24},    //        (171)
    {   0x1fffdf,    21},    //        (172)
    {   0x3fffdf,    22},    //        (173)
    {   0x7fffeb,    23},    //        (174)
    {   0x7fffec,    23},    //        (175)
    {   0x1fffe0,    21},    //        (176)
    {   0x1fffe1,    21},    //        (177)
    {   0x3fffe0,    22},    //        (178)
    {   0x1fffe2,    21},    //        (179)
    {   0x7fffed,    23},    //        (180)
    {   0x3fffe1,    22},    //        (181)
    {   0x7fffee,    23},    //        (182)
    {   0x7fffef,    23},    //        (183)
    {    0xfffea,    20},    //        (184)
    {   0x3fffe2,    22},    //        (185)
    {   0x3fffe3,    22},    //        (186)
    {   0x3fffe4,    22},    //        (187)
    {   0x7ffff0,    23},    //        (188)
    {   0x3fffe5,    22},    //        (189)
    {   0x3fffe6,    22},    //        (190)
    {   0x7ffff1,    23},    //        (191)
    {  0x3ffffe0,    26},    //        (192)
    {  0x3ffffe1,    26},    //        (193)
    {    0xfffeb,    20},    //        (194)
    {    0x7fff1,    19},    //        (195)
    {   0x3fffe7,    22},    //        (196)
    {   0x7ffff2,    23},    //        (197)
    {   0x3fffe8,    22},    //        (198)
    {  0x1ffffec,    25},    //        (199)
    {  0x3ffffe2,    26},    //        (200)
    {  0x3ffffe3,    26},    //        (201)
    {  0x3ffffe4,    26},    //        (202)
    {  0x7ffffde,    27},    //        (203)
    {  0x7ffffdf,    27},    //        (204)
    {  0x3ffffe5,    26},    //        (205)
    {   0xfffff1,    24},    //        (206)
    {  0x1ffffed,    25},    //        (207)
    {    0x7fff2,    19},    //        (208)
    {   0x1fffe3,    21},    //        (209)
    {  0x3ffffe6,    26},    //        (210)
    {  0x7ffffe0,    27},    //        (211)
    {  0x7ffffe1,    27},    //        (212)
    {  0x3ffffe7,    26},    //        (213)
    {  0x7ffffe2,    27},    //        (214)
    {   0xfffff2,    24},    //        (215)
    {   0x1fffe4,    21},    //        (216)
    {   0x1fffe5,    21},    //        (217)
    {  0x3ffffe8,    26},    //        (218)
    {  0x3ffffe9,    26},    //        (219)
    {  0xffffffd,    28},    //        (220)
    {  0x7ffffe3,    27},    //        (221)
    {  0x7ffffe4,    27},    //        (222)
    {  0x7ffffe5,    27},    //        (223)
    {    0xfffec,    20},    //        (224)
    {   0xfffff3,    24},    //        (225)
    {    0xfffed,    20},    //        (226)
    {   0x1fffe6,    21},    //        (227)
    {   0x3fffe9,    22},    //        (228)
    {   0x1fffe7,    21},    //        (229)
    {   0x1fffe8,    21},    //        (230)
    {   0x7ffff3,    23},    //        (231)
    {   0x3fffea,    22},    //        (232)
    {   0x3fffeb,    22},    //        (233)
    {  0x1ffffee,    25},    //        (234)
    {  0x1ffffef,    25},    //        (235)
    {   0xfffff4,    24},    //        (236)
    {   0xfffff5,    24},    //        (237)
    {  0x3ffffea,    26},    //        (238)
    {   0x7ffff4,    23},    //        (239)
    {  0x3ffffeb,    26},    //        (240)
    {  0x7ffffe6,    27},    //        (241)
    {  0x3ffffec,    26},    //        (242)
    {  0x3ffffed,    26},    //        (243)
    {  0x7ffffe7,    27},    //        (244)
    {  0x7ffffe8,    27},    //        (245)
    {  0x7ffffe9,    27},    //        (246)
    {  0x7ffffea,    27},    //        (247)
    {  0x7ffffeb,    27},    //        (248)
    {  0xffffffe,    28},    //        (249)
    {  0x7ffffec,    27},    //        (250)
    {  0x7ffffed,    27},    //        (251)
    {  0x7ffffee,    27},    //        (252)
    {  0x7ffffef,    27},    //        (253)
    {  0x7fffff0,    27},    //        (254)
    {  0x3ffffee,    26},    //        (255)
    { 0x3fffffff,    30}    //    EOS (256)
};


static const struct decode_el decode_tables[256][16] =
{
    /* 0 */
    {
        { 4, 0x00, 0 },
        { 5, 0x00, 0 },
        { 7, 0x00, 0 },
        { 8, 0x00, 0 },
        { 11, 0x00, 0 },
        { 12, 0x00, 0 },
        { 16, 0x00, 0 },
        { 19, 0x00, 0 },
        { 25, 0x00, 0 },
        { 28, 0x00, 0 },
        { 32, 0x00, 0 },
        { 35, 0x00, 0 },
        { 42, 0x00, 0 },
        { 49, 0x00, 0 },
        { 57, 0x00, 0 },
        { 64, 0x01, 0 },
    },
    /* 1 */
    {
        { 0, 0x03, 48 },
        { 0, 0x03, 49 },
        { 0, 0x03, 50 },
        { 0, 0x03, 97 },
        { 0, 0x03, 99 },
        { 0, 0x03, 101 },
        { 0, 0x03, 105 },
        { 0, 0x03, 111 },
        { 0, 0x03, 115 },
        { 0, 0x03, 116 },
        { 13, 0x00, 0 },
        { 14, 0x00, 0 },
        { 17, 0x00, 0 },
        { 18, 0x00, 0 },
        { 20, 0x00, 0 },
        { 21, 0x00, 0 },
    },
    /* 2 */
    {
        { 1, 0x02, 48 },
        { 22, 0x03, 48 },
        { 1, 0x02, 49 },
        { 22, 0x03, 49 },
        { 1, 0x02, 50 },
        { 22, 0x03, 50 },
        { 1, 0x02, 97 },
        { 22, 0x03, 97 },
        { 1, 0x02, 99 },
        { 22, 0x03, 99 },
        { 1, 0x02, 101 },
        { 22, 0x03, 101 },
        { 1, 0x02, 105 },
        { 22, 0x03, 105 },
        { 1, 0x02, 111 },
        { 22, 0x03, 111 },
    },
    /* 3 */
    {
        { 2, 0x02, 48 },
        { 9, 0x02, 48 },
        { 23, 0x02, 48 },
        { 40, 0x03, 48 },
        { 2, 0x02, 49 },
        { 9, 0x02, 49 },
        { 23, 0x02, 49 },
        { 40, 0x03, 49 },
        { 2, 0x02, 50 },
        { 9, 0x02, 50 },
        { 23, 0x02, 50 },
        { 40, 0x03, 50 },
        { 2, 0x02, 97 },
        { 9, 0x02, 97 },
        { 23, 0x02, 97 },
        { 40, 0x03, 97 },
    },
    /* 4 */
    {
        { 3, 0x02, 48 },
        { 6, 0x02, 48 },
        { 10, 0x02, 48 },
        { 15, 0x02, 48 },
        { 24, 0x02, 48 },
        { 31, 0x02, 48 },
        { 41, 0x02, 48 },
        { 56, 0x03, 48 },
        { 3, 0x02, 49 },
        { 6, 0x02, 49 },
        { 10, 0x02, 49 },
        { 15, 0x02, 49 },
        { 24, 0x02, 49 },
        { 31, 0x02, 49 },
        { 41, 0x02, 49 },
        { 56, 0x03, 49 },
    },
    /* 5 */
    {
        { 3, 0x02, 50 },
        { 6, 0x02, 50 },
        { 10, 0x02, 50 },
        { 15, 0x02, 50 },
        { 24, 0x02, 50 },
        { 31, 0x02, 50 },
        { 41, 0x02, 50 },
        { 56, 0x03, 50 },
        { 3, 0x02, 97 },
        { 6, 0x02, 97 },
        { 10, 0x02, 97 },
        { 15, 0x02, 97 },
        { 24, 0x02, 97 },
        { 31, 0x02, 97 },
        { 41, 0x02, 97 },
        { 56, 0x03, 97 },
    },
    /* 6 */
    {
        { 2, 0x02, 99 },
        { 9, 0x02, 99 },
        { 23, 0x02, 99 },
        { 40, 0x03, 99 },
        { 2, 0x02, 101 },
        { 9, 0x02, 101 },
        { 23, 0x02, 101 },
        { 40, 0x03, 101 },
        { 2, 0x02, 105 },
        { 9, 0x02, 105 },
        { 23, 0x02, 105 },
        { 40, 0x03, 105 },
        { 2, 0x02, 111 },
        { 9, 0x02, 111 },
        { 23, 0x02, 111 },
        { 40, 0x03, 111 },
    },
    /* 7 */
    {
        { 3, 0x02, 99 },
        { 6, 0x02, 99 },
        { 10, 0x02, 99 },
        { 15, 0x02, 99 },
        { 24, 0x02, 99 },
        { 31, 0x02, 99 },
        { 41, 0x02, 99 },
        { 56, 0x03, 99 },
        { 3, 0x02, 101 },
        { 6, 0x02, 101 },
        { 10, 0x02, 101 },
        { 15, 0x02, 101 },
        { 24, 0x02, 101 },
        { 31, 0x02, 101 },
        { 41, 0x02, 101 },
        { 56, 0x03, 101 },
    },
    /* 8 */
    {
        { 3, 0x02, 105 },
        { 6, 0x02, 105 },
        { 10, 0x02, 105 },
        { 15, 0x02, 105 },
        { 24, 0x02, 105 },
        { 31, 0x02, 105 },
        { 41, 0x02, 105 },
        { 56, 0x03, 105 },
        { 3, 0x02, 111 },
        { 6, 0x02, 111 },
        { 10, 0x02, 111 },
        { 15, 0x02, 111 },
        { 24, 0x02, 111 },
        { 31, 0x02, 111 },
        { 41, 0x02, 111 },
        { 56, 0x03, 111 },
    },
    /* 9 */
    {
        { 1, 0x02, 115 },
        { 22, 0x03, 115 },
        { 1, 0x02, 116 },
        { 22, 0x03, 116 },
        { 0, 0x03, 32 },
        { 0, 0x03, 37 },
        { 0, 0x03, 45 },
        { 0, 0x03, 46 },
        { 0, 0x03, 47 },
        { 0, 0x03, 51 },
        { 0, 0x03, 52 },
        { 0, 0x03, 53 },
        { 0, 0x03, 54 },
        { 0, 0x03, 55 },
        { 0, 0x03, 56 },
        { 0, 0x03, 57 },
    },
    /* 10 */
    {
        { 2, 0x02, 115 },
        { 9, 0x02, 115 },
        { 23, 0x02, 115 },
        { 40, 0x03, 115 },
        { 2, 0x02, 116 },
        { 9, 0x02, 116 },
        { 23, 0x02, 116 },
        { 40, 0x03, 116 },
        { 1, 0x02, 32 },
        { 22, 0x03, 32 },
        { 1, 0x02, 37 },
        { 22, 0x03, 37 },
        { 1, 0x02, 45 },
        { 22, 0x03, 45 },
        { 1, 0x02, 46 },
        { 22, 0x03, 46 },
    },
    /* 11 */
    {
        { 3, 0x02, 115 },
        { 6, 0x02, 115 },
        { 10, 0x02, 115 },
        { 15, 0x02, 115 },
        { 24, 0x02, 115 },
        { 31, 0x02, 115 },
        { 41, 0x02, 115 },
        { 56, 0x03, 115 },
        { 3, 0x02, 116 },
        { 6, 0x02, 116 },
        { 10, 0x02, 116 },
        { 15, 0x02, 116 },
        { 24, 0x02, 116 },
        { 31, 0x02, 116 },
        { 41, 0x02, 116 },
        { 56, 0x03, 116 },
    },
    /* 12 */
    {
        { 2, 0x02, 32 },
        { 9, 0x02, 32 },
        { 23, 0x02, 32 },
        { 40, 0x03, 32 },
        { 2, 0x02, 37 },
        { 9, 0x02, 37 },
        { 23, 0x02, 37 },
        { 40, 0x03, 37 },
        { 2, 0x02, 45 },
        { 9, 0x02, 45 },
        { 23, 0x02, 45 },
        { 40, 0x03, 45 },
        { 2, 0x02, 46 },
        { 9, 0x02, 46 },
        { 23, 0x02, 46 },
        { 40, 0x03, 46 },
    },
    /* 13 */
    {
        { 3, 0x02, 32 },
        { 6, 0x02, 32 },
        { 10, 0x02, 32 },
        { 15, 0x02, 32 },
        { 24, 0x02, 32 },
        { 31, 0x02, 32 },
        { 41, 0x02, 32 },
        { 56, 0x03, 32 },
        { 3, 0x02, 37 },
        { 6, 0x02, 37 },
        { 10, 0x02, 37 },
        { 15, 0x02, 37 },
        { 24, 0x02, 37 },
        { 31, 0x02, 37 },
        { 41, 0x02, 37 },
        { 56, 0x03, 37 },
    },
    /* 14 */
    {
        { 3, 0x02, 45 },
        { 6, 0x02, 45 },
        { 10, 0x02, 45 },
        { 15, 0x02, 45 },
        { 24, 0x02, 45 },
        { 31, 0x02, 45 },
        { 41, 0x02, 45 },
        { 56, 0x03, 45 },
        { 3, 0x02, 46 },
        { 6, 0x02, 46 },
        { 10, 0x02, 46 },
        { 15, 0x02, 46 },
        { 24, 0x02, 46 },
        { 31, 0x02, 46 },
        { 41, 0x02, 46 },
        { 56, 0x03, 46 },
    },
    /* 15 */
    {
        { 1, 0x02, 47 },
        { 22, 0x03, 47 },
        { 1, 0x02, 51 },
        { 22, 0x03, 51 },
        { 1, 0x02, 52 },
        { 22, 0x03, 52 },
        { 1, 0x02, 53 },
        { 22, 0x03, 53 },
        { 1, 0x02, 54 },
        { 22, 0x03, 54 },
        { 1, 0x02, 55 },
        { 22, 0x03, 55 },
        { 1, 0x02, 56 },
        { 22, 0x03, 56 },
        { 1, 0x02, 57 },
        { 22, 0x03, 57 },
    },
    /* 16 */
    {
        { 2, 0x02, 47 },
        { 9, 0x02, 47 },
        { 23, 0x02, 47 },
        { 40, 0x03, 47 },
        { 2, 0x02, 51 },
        { 9, 0x02, 51 },
        { 23, 0x02, 51 },
        { 40, 0x03, 51 },
        { 2, 0x02, 52 },
        { 9, 0x02, 52 },
        { 23, 0x02, 52 },
        { 40, 0x03, 52 },
        { 2, 0x02, 53 },
        { 9, 0x02, 53 },
        { 23, 0x02, 53 },
        { 40, 0x03, 53 },
    },
    /* 17 */
    {
        { 3, 0x02, 47 },
        { 6, 0x02, 47 },
        { 10, 0x02, 47 },
        { 15, 0x02, 47 },
        { 24, 0x02, 47 },
        { 31, 0x02, 47 },
        { 41, 0x02, 47 },
        { 56, 0x03, 47 },
        { 3, 0x02, 51 },
        { 6, 0x02, 51 },
        { 10, 0x02, 51 },
        { 15, 0x02, 51 },
        { 24, 0x02, 51 },
        { 31, 0x02, 51 },
        { 41, 0x02, 51 },
        { 56, 0x03, 51 },
    },
    /* 18 */
    {
        { 3, 0x02, 52 },
        { 6, 0x02, 52 },
        { 10, 0x02, 52 },
        { 15, 0x02, 52 },
        { 24, 0x02, 52 },
        { 31, 0x02, 52 },
        { 41, 0x02, 52 },
        { 56, 0x03, 52 },
        { 3, 0x02, 53 },
        { 6, 0x02, 53 },
        { 10, 0x02, 53 },
        { 15, 0x02, 53 },
        { 24, 0x02, 53 },
        { 31, 0x02, 53 },
        { 41, 0x02, 53 },
        { 56, 0x03, 53 },
    },
    /* 19 */
    {
        { 2, 0x02, 54 },
        { 9, 0x02, 54 },
        { 23, 0x02, 54 },
        { 40, 0x03, 54 },
        { 2, 0x02, 55 },
        { 9, 0x02, 55 },
        { 23, 0x02, 55 },
        { 40, 0x03, 55 },
        { 2, 0x02, 56 },
        { 9, 0x02, 56 },
        { 23, 0x02, 56 },
        { 40, 0x03, 56 },
        { 2, 0x02, 57 },
        { 9, 0x02, 57 },
        { 23, 0x02, 57 },
        { 40, 0x03, 57 },
    },
    /* 20 */
    {
        { 3, 0x02, 54 },
        { 6, 0x02, 54 },
        { 10, 0x02, 54 },
        { 15, 0x02, 54 },
        { 24, 0x02, 54 },
        { 31, 0x02, 54 },
        { 41, 0x02, 54 },
        { 56, 0x03, 54 },
        { 3, 0x02, 55 },
        { 6, 0x02, 55 },
        { 10, 0x02, 55 },
        { 15, 0x02, 55 },
        { 24, 0x02, 55 },
        { 31, 0x02, 55 },
        { 41, 0x02, 55 },
        { 56, 0x03, 55 },
    },
    /* 21 */
    {
        { 3, 0x02, 56 },
        { 6, 0x02, 56 },
        { 10, 0x02, 56 },
        { 15, 0x02, 56 },
        { 24, 0x02, 56 },
        { 31, 0x02, 56 },
        { 41, 0x02, 56 },
        { 56, 0x03, 56 },
        { 3, 0x02, 57 },
        { 6, 0x02, 57 },
        { 10, 0x02, 57 },
        { 15, 0x02, 57 },
        { 24, 0x02, 57 },
        { 31, 0x02, 57 },
        { 41, 0x02, 57 },
        { 56, 0x03, 57 },
    },
    /* 22 */
    {
        { 26, 0x00, 0 },
        { 27, 0x00, 0 },
        { 29, 0x00, 0 },
        { 30, 0x00, 0 },
        { 33, 0x00, 0 },
        { 34, 0x00, 0 },
        { 36, 0x00, 0 },
        { 37, 0x00, 0 },
        { 43, 0x00, 0 },
        { 46, 0x00, 0 },
        { 50, 0x00, 0 },
        { 53, 0x00, 0 },
        { 58, 0x00, 0 },
        { 61, 0x00, 0 },
        { 65, 0x00, 0 },
        { 68, 0x01, 0 },
    },
    /* 23 */
    {
        { 0, 0x03, 61 },
        { 0, 0x03, 65 },
        { 0, 0x03, 95 },
        { 0, 0x03, 98 },
        { 0, 0x03, 100 },
        { 0, 0x03, 102 },
        { 0, 0x03, 103 },
        { 0, 0x03, 104 },
        { 0, 0x03, 108 },
        { 0, 0x03, 109 },
        { 0, 0x03, 110 },
        { 0, 0x03, 112 },
        { 0, 0x03, 114 },
        { 0, 0x03, 117 },
        { 38, 0x00, 0 },
        { 39, 0x00, 0 },
    },
    /* 24 */
    {
        { 1, 0x02, 61 },
        { 22, 0x03, 61 },
        { 1, 0x02, 65 },
        { 22, 0x03, 65 },
        { 1, 0x02, 95 },
        { 22, 0x03, 95 },
        { 1, 0x02, 98 },
        { 22, 0x03, 98 },
        { 1, 0x02, 100 },
        { 22, 0x03, 100 },
        { 1, 0x02, 102 },
        { 22, 0x03, 102 },
        { 1, 0x02, 103 },
        { 22, 0x03, 103 },
        { 1, 0x02, 104 },
        { 22, 0x03, 104 },
    },
    /* 25 */
    {
        { 2, 0x02, 61 },
        { 9, 0x02, 61 },
        { 23, 0x02, 61 },
        { 40, 0x03, 61 },
        { 2, 0x02, 65 },
        { 9, 0x02, 65 },
        { 23, 0x02, 65 },
        { 40, 0x03, 65 },
        { 2, 0x02, 95 },
        { 9, 0x02, 95 },
        { 23, 0x02, 95 },
        { 40, 0x03, 95 },
        { 2, 0x02, 98 },
        { 9, 0x02, 98 },
        { 23, 0x02, 98 },
        { 40, 0x03, 98 },
    },
    /* 26 */
    {
        { 3, 0x02, 61 },
        { 6, 0x02, 61 },
        { 10, 0x02, 61 },
        { 15, 0x02, 61 },
        { 24, 0x02, 61 },
        { 31, 0x02, 61 },
        { 41, 0x02, 61 },
        { 56, 0x03, 61 },
        { 3, 0x02, 65 },
        { 6, 0x02, 65 },
        { 10, 0x02, 65 },
        { 15, 0x02, 65 },
        { 24, 0x02, 65 },
        { 31, 0x02, 65 },
        { 41, 0x02, 65 },
        { 56, 0x03, 65 },
    },
    /* 27 */
    {
        { 3, 0x02, 95 },
        { 6, 0x02, 95 },
        { 10, 0x02, 95 },
        { 15, 0x02, 95 },
        { 24, 0x02, 95 },
        { 31, 0x02, 95 },
        { 41, 0x02, 95 },
        { 56, 0x03, 95 },
        { 3, 0x02, 98 },
        { 6, 0x02, 98 },
        { 10, 0x02, 98 },
        { 15, 0x02, 98 },
        { 24, 0x02, 98 },
        { 31, 0x02, 98 },
        { 41, 0x02, 98 },
        { 56, 0x03, 98 },
    },
    /* 28 */
    {
        { 2, 0x02, 100 },
        { 9, 0x02, 100 },
        { 23, 0x02, 100 },
        { 40, 0x03, 100 },
        { 2, 0x02, 102 },
        { 9, 0x02, 102 },
        { 23, 0x02, 102 },
        { 40, 0x03, 102 },
        { 2, 0x02, 103 },
        { 9, 0x02, 103 },
        { 23, 0x02, 103 },
        { 40, 0x03, 103 },
        { 2, 0x02, 104 },
        { 9, 0x02, 104 },
        { 23, 0x02, 104 },
        { 40, 0x03, 104 },
    },
    /* 29 */
    {
        { 3, 0x02, 100 },
        { 6, 0x02, 100 },
        { 10, 0x02, 100 },
        { 15, 0x02, 100 },
        { 24, 0x02, 100 },
        { 31, 0x02, 100 },
        { 41, 0x02, 100 },
        { 56, 0x03, 100 },
        { 3, 0x02, 102 },
        { 6, 0x02, 102 },
        { 10, 0x02, 102 },
        { 15, 0x02, 102 },
        { 24, 0x02, 102 },
        { 31, 0x02, 102 },
        { 41, 0x02, 102 },
        { 56, 0x03, 102 },
    },
    /* 30 */
    {
        { 3, 0x02, 103 },
        { 6, 0x02, 103 },
        { 10, 0x02, 103 },
        { 15, 0x02, 103 },
        { 24, 0x02, 103 },
        { 31, 0x02, 103 },
        { 41, 0x02, 103 },
        { 56, 0x03, 103 },
        { 3, 0x02, 104 },
        { 6, 0x02, 104 },
        { 10, 0x02, 104 },
        { 15, 0x02, 104 },
        { 24, 0x02, 104 },
        { 31, 0x02, 104 },
        { 41, 0x02, 104 },
        { 56, 0x03, 104 },
    },
    /* 31 */
    {
        { 1, 0x02, 108 },
        { 22, 0x03, 108 },
        { 1, 0x02, 109 },
        { 22, 0x03, 109 },
        { 1, 0x02, 110 },
        { 22, 0x03, 110 },
        { 1, 0x02, 112 },
        { 22, 0x03, 112 },
        { 1, 0x02, 114 },
        { 22, 0x03, 114 },
        { 1, 0x02, 117 },
        { 22, 0x03, 117 },
        { 0, 0x03, 58 },
        { 0, 0x03, 66 },
        { 0, 0x03, 67 },
        { 0, 0x03, 68 },
    },
    /* 32 */
    {
        { 2, 0x02, 108 },
        { 9, 0x02, 108 },
        { 23, 0x02, 108 },
        { 40, 0x03, 108 },
        { 2, 0x02, 109 },
        { 9, 0x02, 109 },
        { 23, 0x02, 109 },
        { 40, 0x03, 109 },
        { 2, 0x02, 110 },
        { 9, 0x02, 110 },
        { 23, 0x02, 110 },
        { 40, 0x03, 110 },
        { 2, 0x02, 112 },
        { 9, 0x02, 112 },
        { 23, 0x02, 112 },
        { 40, 0x03, 112 },
    },
    /* 33 */
    {
        { 3, 0x02, 108 },
        { 6, 0x02, 108 },
        { 10, 0x02, 108 },
        { 15, 0x02, 108 },
        { 24, 0x02, 108 },
        { 31, 0x02, 108 },
        { 41, 0x02, 108 },
        { 56, 0x03, 108 },
        { 3, 0x02, 109 },
        { 6, 0x02, 109 },
        { 10, 0x02, 109 },
        { 15, 0x02, 109 },
        { 24, 0x02, 109 },
        { 31, 0x02, 109 },
        { 41, 0x02, 109 },
        { 56, 0x03, 109 },
    },
    /* 34 */
    {
        { 3, 0x02, 110 },
        { 6, 0x02, 110 },
        { 10, 0x02, 110 },
        { 15, 0x02, 110 },
        { 24, 0x02, 110 },
        { 31, 0x02, 110 },
        { 41, 0x02, 110 },
        { 56, 0x03, 110 },
        { 3, 0x02, 112 },
        { 6, 0x02, 112 },
        { 10, 0x02, 112 },
        { 15, 0x02, 112 },
        { 24, 0x02, 112 },
        { 31, 0x02, 112 },
        { 41, 0x02, 112 },
        { 56, 0x03, 112 },
    },
    /* 35 */
    {
        { 2, 0x02, 114 },
        { 9, 0x02, 114 },
        { 23, 0x02, 114 },
        { 40, 0x03, 114 },
        { 2, 0x02, 117 },
        { 9, 0x02, 117 },
        { 23, 0x02, 117 },
        { 40, 0x03, 117 },
        { 1, 0x02, 58 },
        { 22, 0x03, 58 },
        { 1, 0x02, 66 },
        { 22, 0x03, 66 },
        { 1, 0x02, 67 },
        { 22, 0x03, 67 },
        { 1, 0x02, 68 },
        { 22, 0x03, 68 },
    },
    /* 36 */
    {
        { 3, 0x02, 114 },
        { 6, 0x02, 114 },
        { 10, 0x02, 114 },
        { 15, 0x02, 114 },
        { 24, 0x02, 114 },
        { 31, 0x02, 114 },
        { 41, 0x02, 114 },
        { 56, 0x03, 114 },
        { 3, 0x02, 117 },
        { 6, 0x02, 117 },
        { 10, 0x02, 117 },
        { 15, 0x02, 117 },
        { 24, 0x02, 117 },
        { 31, 0x02, 117 },
        { 41, 0x02, 117 },
        { 56, 0x03, 117 },
    },
    /* 37 */
    {
        { 2, 0x02, 58 },
        { 9, 0x02, 58 },
        { 23, 0x02, 58 },
        { 40, 0x03, 58 },
        { 2, 0x02, 66 },
        { 9, 0x02, 66 },
        { 23, 0x02, 66 },
        { 40, 0x03, 66 },
        { 2, 0x02, 67 },
        { 9, 0x02, 67 },
        { 23, 0x02, 67 },
        { 40, 0x03, 67 },
        { 2, 0x02, 68 },
        { 9, 0x02, 68 },
        { 23, 0x02, 68 },
        { 40, 0x03, 68 },
    },
    /* 38 */
    {
        { 3, 0x02, 58 },
        { 6, 0x02, 58 },
        { 10, 0x02, 58 },
        { 15, 0x02, 58 },
        { 24, 0x02, 58 },
        { 31, 0x02, 58 },
        { 41, 0x02, 58 },
        { 56, 0x03, 58 },
        { 3, 0x02, 66 },
        { 6, 0x02, 66 },
        { 10, 0x02, 66 },
        { 15, 0x02, 66 },
        { 24, 0x02, 66 },
        { 31, 0x02, 66 },
        { 41, 0x02, 66 },
        { 56, 0x03, 66 },
    },
    /* 39 */
    {
        { 3, 0x02, 67 },
        { 6, 0x02, 67 },
        { 10, 0x02, 67 },
        { 15, 0x02, 67 },
        { 24, 0x02, 67 },
        { 31, 0x02, 67 },
        { 41, 0x02, 67 },
        { 56, 0x03, 67 },
        { 3, 0x02, 68 },
        { 6, 0x02, 68 },
        { 10, 0x02, 68 },
        { 15, 0x02, 68 },
        { 24, 0x02, 68 },
        { 31, 0x02, 68 },
        { 41, 0x02, 68 },
        { 56, 0x03, 68 },
    },
    /* 40 */
    {
        { 44, 0x00, 0 },
        { 45, 0x00, 0 },
        { 47, 0x00, 0 },
        { 48, 0x00, 0 },
        { 51, 0x00, 0 },
        { 52, 0x00, 0 },
        { 54, 0x00, 0 },
        { 55, 0x00, 0 },
        { 59, 0x00, 0 },
        { 60, 0x00, 0 },
        { 62, 0x00, 0 },
        { 63, 0x00, 0 },
        { 66, 0x00, 0 },
        { 67, 0x00, 0 },
        { 69, 0x00, 0 },
        { 72, 0x01, 0 },
    },
    /* 41 */
    {
        { 0, 0x03, 69 },
        { 0, 0x03, 70 },
        { 0, 0x03, 71 },
        { 0, 0x03, 72 },
        { 0, 0x03, 73 },
        { 0, 0x03, 74 },
        { 0, 0x03, 75 },
        { 0, 0x03, 76 },
        { 0, 0x03, 77 },
        { 0, 0x03, 78 },
        { 0, 0x03, 79 },
        { 0, 0x03, 80 },
        { 0, 0x03, 81 },
        { 0, 0x03, 82 },
        { 0, 0x03, 83 },
        { 0, 0x03, 84 },
    },
    /* 42 */
    {
        { 1, 0x02, 69 },
        { 22, 0x03, 69 },
        { 1, 0x02, 70 },
        { 22, 0x03, 70 },
        { 1, 0x02, 71 },
        { 22, 0x03, 71 },
        { 1, 0x02, 72 },
        { 22, 0x03, 72 },
        { 1, 0x02, 73 },
        { 22, 0x03, 73 },
        { 1, 0x02, 74 },
        { 22, 0x03, 74 },
        { 1, 0x02, 75 },
        { 22, 0x03, 75 },
        { 1, 0x02, 76 },
        { 22, 0x03, 76 },
    },
    /* 43 */
    {
        { 2, 0x02, 69 },
        { 9, 0x02, 69 },
        { 23, 0x02, 69 },
        { 40, 0x03, 69 },
        { 2, 0x02, 70 },
        { 9, 0x02, 70 },
        { 23, 0x02, 70 },
        { 40, 0x03, 70 },
        { 2, 0x02, 71 },
        { 9, 0x02, 71 },
        { 23, 0x02, 71 },
        { 40, 0x03, 71 },
        { 2, 0x02, 72 },
        { 9, 0x02, 72 },
        { 23, 0x02, 72 },
        { 40, 0x03, 72 },
    },
    /* 44 */
    {
        { 3, 0x02, 69 },
        { 6, 0x02, 69 },
        { 10, 0x02, 69 },
        { 15, 0x02, 69 },
        { 24, 0x02, 69 },
        { 31, 0x02, 69 },
        { 41, 0x02, 69 },
        { 56, 0x03, 69 },
        { 3, 0x02, 70 },
        { 6, 0x02, 70 },
        { 10, 0x02, 70 },
        { 15, 0x02, 70 },
        { 24, 0x02, 70 },
        { 31, 0x02, 70 },
        { 41, 0x02, 70 },
        { 56, 0x03, 70 },
    },
    /* 45 */
    {
        { 3, 0x02, 71 },
        { 6, 0x02, 71 },
        { 10, 0x02, 71 },
        { 15, 0x02, 71 },
        { 24, 0x02, 71 },
        { 31, 0x02, 71 },
        { 41, 0x02, 71 },
        { 56, 0x03, 71 },
        { 3, 0x02, 72 },
        { 6, 0x02, 72 },
        { 10, 0x02, 72 },
        { 15, 0x02, 72 },
        { 24, 0x02, 72 },
        { 31, 0x02, 72 },
        { 41, 0x02, 72 },
        { 56, 0x03, 72 },
    },
    /* 46 */
    {
        { 2, 0x02, 73 },
        { 9, 0x02, 73 },
        { 23, 0x02, 73 },
        { 40, 0x03, 73 },
        { 2, 0x02, 74 },
        { 9, 0x02, 74 },
        { 23, 0x02, 74 },
        { 40, 0x03, 74 },
        { 2, 0x02, 75 },
        { 9, 0x02, 75 },
        { 23, 0x02, 75 },
        { 40, 0x03, 75 },
        { 2, 0x02, 76 },
        { 9, 0x02, 76 },
        { 23, 0x02, 76 },
        { 40, 0x03, 76 },
    },
    /* 47 */
    {
        { 3, 0x02, 73 },
        { 6, 0x02, 73 },
        { 10, 0x02, 73 },
        { 15, 0x02, 73 },
        { 24, 0x02, 73 },
        { 31, 0x02, 73 },
        { 41, 0x02, 73 },
        { 56, 0x03, 73 },
        { 3, 0x02, 74 },
        { 6, 0x02, 74 },
        { 10, 0x02, 74 },
        { 15, 0x02, 74 },
        { 24, 0x02, 74 },
        { 31, 0x02, 74 },
        { 41, 0x02, 74 },
        { 56, 0x03, 74 },
    },
    /* 48 */
    {
        { 3, 0x02, 75 },
        { 6, 0x02, 75 },
        { 10, 0x02, 75 },
        { 15, 0x02, 75 },
        { 24, 0x02, 75 },
        { 31, 0x02, 75 },
        { 41, 0x02, 75 },
        { 56, 0x03, 75 },
        { 3, 0x02, 76 },
        { 6, 0x02, 76 },
        { 10, 0x02, 76 },
        { 15, 0x02, 76 },
        { 24, 0x02, 76 },
        { 31, 0x02, 76 },
        { 41, 0x02, 76 },
        { 56, 0x03, 76 },
    },
    /* 49 */
    {
        { 1, 0x02, 77 },
        { 22, 0x03, 77 },
        { 1, 0x02, 78 },
        { 22, 0x03, 78 },
        { 1, 0x02, 79 },
        { 22, 0x03, 79 },
        { 1, 0x02, 80 },
        { 22, 0x03, 80 },
        { 1, 0x02, 81 },
        { 22, 0x03, 81 },
        { 1, 0x02, 82 },
        { 22, 0x03, 82 },
        { 1, 0x02, 83 },
        { 22, 0x03, 83 },
        { 1, 0x02, 84 },
        { 22, 0x03, 84 },
    },
    /* 50 */
    {
        { 2, 0x02, 77 },
        { 9, 0x02, 77 },
        { 23, 0x02, 77 },
        { 40, 0x03, 77 },
        { 2, 0x02, 78 },
        { 9, 0x02, 78 },
        { 23, 0x02, 78 },
        { 40, 0x03, 78 },
        { 2, 0x02, 79 },
        { 9, 0x02, 79 },
        { 23, 0x02, 79 },
        { 40, 0x03, 79 },
        { 2, 0x02, 80 },
        { 9, 0x02, 80 },
        { 23, 0x02, 80 },
        { 40, 0x03, 80 },
    },
    /* 51 */
    {
        { 3, 0x02, 77 },
        { 6, 0x02, 77 },
        { 10, 0x02, 77 },
        { 15, 0x02, 77 },
        { 24, 0x02, 77 },
        { 31, 0x02, 77 },
        { 41, 0x02, 77 },
        { 56, 0x03, 77 },
        { 3, 0x02, 78 },
        { 6, 0x02, 78 },
        { 10, 0x02, 78 },
        { 15, 0x02, 78 },
        { 24, 0x02, 78 },
        { 31, 0x02, 78 },
        { 41, 0x02, 78 },
        { 56, 0x03, 78 },
    },
    /* 52 */
    {
        { 3, 0x02, 79 },
        { 6, 0x02, 79 },
        { 10, 0x02, 79 },
        { 15, 0x02, 79 },
        { 24, 0x02, 79 },
        { 31, 0x02, 79 },
        { 41, 0x02, 79 },
        { 56, 0x03, 79 },
        { 3, 0x02, 80 },
        { 6, 0x02, 80 },
        { 10, 0x02, 80 },
        { 15, 0x02, 80 },
        { 24, 0x02, 80 },
        { 31, 0x02, 80 },
        { 41, 0x02, 80 },
        { 56, 0x03, 80 },
    },
    /* 53 */
    {
        { 2, 0x02, 81 },
        { 9, 0x02, 81 },
        { 23, 0x02, 81 },
        { 40, 0x03, 81 },
        { 2, 0x02, 82 },
        { 9, 0x02, 82 },
        { 23, 0x02, 82 },
        { 40, 0x03, 82 },
        { 2, 0x02, 83 },
        { 9, 0x02, 83 },
        { 23, 0x02, 83 },
        { 40, 0x03, 83 },
        { 2, 0x02, 84 },
        { 9, 0x02, 84 },
        { 23, 0x02, 84 },
        { 40, 0x03, 84 },
    },
    /* 54 */
    {
        { 3, 0x02, 81 },
        { 6, 0x02, 81 },
        { 10, 0x02, 81 },
        { 15, 0x02, 81 },
        { 24, 0x02, 81 },
        { 31, 0x02, 81 },
        { 41, 0x02, 81 },
        { 56, 0x03, 81 },
        { 3, 0x02, 82 },
        { 6, 0x02, 82 },
        { 10, 0x02, 82 },
        { 15, 0x02, 82 },
        { 24, 0x02, 82 },
        { 31, 0x02, 82 },
        { 41, 0x02, 82 },
        { 56, 0x03, 82 },
    },
    /* 55 */
    {
        { 3, 0x02, 83 },
        { 6, 0x02, 83 },
        { 10, 0x02, 83 },
        { 15, 0x02, 83 },
        { 24, 0x02, 83 },
        { 31, 0x02, 83 },
        { 41, 0x02, 83 },
        { 56, 0x03, 83 },
        { 3, 0x02, 84 },
        { 6, 0x02, 84 },
        { 10, 0x02, 84 },
        { 15, 0x02, 84 },
        { 24, 0x02, 84 },
        { 31, 0x02, 84 },
        { 41, 0x02, 84 },
        { 56, 0x03, 84 },
    },
    /* 56 */
    {
        { 0, 0x03, 85 },
        { 0, 0x03, 86 },
        { 0, 0x03, 87 },
        { 0, 0x03, 89 },
        { 0, 0x03, 106 },
        { 0, 0x03, 107 },
        { 0, 0x03, 113 },
        { 0, 0x03, 118 },
        { 0, 0x03, 119 },
        { 0, 0x03, 120 },
        { 0, 0x03, 121 },
        { 0, 0x03, 122 },
        { 70, 0x00, 0 },
        { 71, 0x00, 0 },
        { 73, 0x00, 0 },
        { 74, 0x01, 0 },
    },
    /* 57 */
    {
        { 1, 0x02, 85 },
        { 22, 0x03, 85 },
        { 1, 0x02, 86 },
        { 22, 0x03, 86 },
        { 1, 0x02, 87 },
        { 22, 0x03, 87 },
        { 1, 0x02, 89 },
        { 22, 0x03, 89 },
        { 1, 0x02, 106 },
        { 22, 0x03, 106 },
        { 1, 0x02, 107 },
        { 22, 0x03, 107 },
        { 1, 0x02, 113 },
        { 22, 0x03, 113 },
        { 1, 0x02, 118 },
        { 22, 0x03, 118 },
    },
    /* 58 */
    {
        { 2, 0x02, 85 },
        { 9, 0x02, 85 },
        { 23, 0x02, 85 },
        { 40, 0x03, 85 },
        { 2, 0x02, 86 },
        { 9, 0x02, 86 },
        { 23, 0x02, 86 },
        { 40, 0x03, 86 },
        { 2, 0x02, 87 },
        { 9, 0x02, 87 },
        { 23, 0x02, 87 },
        { 40, 0x03, 87 },
        { 2, 0x02, 89 },
        { 9, 0x02, 89 },
        { 23, 0x02, 89 },
        { 40, 0x03, 89 },
    },
    /* 59 */
    {
        { 3, 0x02, 85 },
        { 6, 0x02, 85 },
        { 10, 0x02, 85 },
        { 15, 0x02, 85 },
        { 24, 0x02, 85 },
        { 31, 0x02, 85 },
        { 41, 0x02, 85 },
        { 56, 0x03, 85 },
        { 3, 0x02, 86 },
        { 6, 0x02, 86 },
        { 10, 0x02, 86 },
        { 15, 0x02, 86 },
        { 24, 0x02, 86 },
        { 31, 0x02, 86 },
        { 41, 0x02, 86 },
        { 56, 0x03, 86 },
    },
    /* 60 */
    {
        { 3, 0x02, 87 },
        { 6, 0x02, 87 },
        { 10, 0x02, 87 },
        { 15, 0x02, 87 },
        { 24, 0x02, 87 },
        { 31, 0x02, 87 },
        { 41, 0x02, 87 },
        { 56, 0x03, 87 },
        { 3, 0x02, 89 },
        { 6, 0x02, 89 },
        { 10, 0x02, 89 },
        { 15, 0x02, 89 },
        { 24, 0x02, 89 },
        { 31, 0x02, 89 },
        { 41, 0x02, 89 },
        { 56, 0x03, 89 },
    },
    /* 61 */
    {
        { 2, 0x02, 106 },
        { 9, 0x02, 106 },
        { 23, 0x02, 106 },
        { 40, 0x03, 106 },
        { 2, 0x02, 107 },
        { 9, 0x02, 107 },
        { 23, 0x02, 107 },
        { 40, 0x03, 107 },
        { 2, 0x02, 113 },
        { 9, 0x02, 113 },
        { 23, 0x02, 113 },
        { 40, 0x03, 113 },
        { 2, 0x02, 118 },
        { 9, 0x02, 118 },
        { 23, 0x02, 118 },
        { 40, 0x03, 118 },
    },
    /* 62 */
    {
        { 3, 0x02, 106 },
        { 6, 0x02, 106 },
        { 10, 0x02, 106 },
        { 15, 0x02, 106 },
        { 24, 0x02, 106 },
        { 31, 0x02, 106 },
        { 41, 0x02, 106 },
        { 56, 0x03, 106 },
        { 3, 0x02, 107 },
        { 6, 0x02, 107 },
        { 10, 0x02, 107 },
        { 15, 0x02, 107 },
        { 24, 0x02, 107 },
        { 31, 0x02, 107 },
        { 41, 0x02, 107 },
        { 56, 0x03, 107 },
    },
    /* 63 */
    {
        { 3, 0x02, 113 },
        { 6, 0x02, 113 },
        { 10, 0x02, 113 },
        { 15, 0x02, 113 },
        { 24, 0x02, 113 },
        { 31, 0x02, 113 },
        { 41, 0x02, 113 },
        { 56, 0x03, 113 },
        { 3, 0x02, 118 },
        { 6, 0x02, 118 },
        { 10, 0x02, 118 },
        { 15, 0x02, 118 },
        { 24, 0x02, 118 },
        { 31, 0x02, 118 },
        { 41, 0x02, 118 },
        { 56, 0x03, 118 },
    },
    /* 64 */
    {
        { 1, 0x02, 119 },
        { 22, 0x03, 119 },
        { 1, 0x02, 120 },
        { 22, 0x03, 120 },
        { 1, 0x02, 121 },
        { 22, 0x03, 121 },
        { 1, 0x02, 122 },
        { 22, 0x03, 122 },
        { 0, 0x03, 38 },
        { 0, 0x03, 42 },
        { 0, 0x03, 44 },
        { 0, 0x03, 59 },
        { 0, 0x03, 88 },
        { 0, 0x03, 90 },
        { 75, 0x00, 0 },
        { 78, 0x00, 0 },
    },
    /* 65 */
    {
        { 2, 0x02, 119 },
        { 9, 0x02, 119 },
        { 23, 0x02, 119 },
        { 40, 0x03, 119 },
        { 2, 0x02, 120 },
        { 9, 0x02, 120 },
        { 23, 0x02, 120 },
        { 40, 0x03, 120 },
        { 2, 0x02, 121 },
        { 9, 0x02, 121 },
        { 23, 0x02, 121 },
        { 40, 0x03, 121 },
        { 2, 0x02, 122 },
        { 9, 0x02, 122 },
        { 23, 0x02, 122 },
        { 40, 0x03, 122 },
    },
    /* 66 */
    {
        { 3, 0x02, 119 },
        { 6, 0x02, 119 },
        { 10, 0x02, 119 },
        { 15, 0x02, 119 },
        { 24, 0x02, 119 },
        { 31, 0x02, 119 },
        { 41, 0x02, 119 },
        { 56, 0x03, 119 },
        { 3, 0x02, 120 },
        { 6, 0x02, 120 },
        { 10, 0x02, 120 },
        { 15, 0x02, 120 },
        { 24, 0x02, 120 },
        { 31, 0x02, 120 },
        { 41, 0x02, 120 },
        { 56, 0x03, 120 },
    },
    /* 67 */
    {
        { 3, 0x02, 121 },
        { 6, 0x02, 121 },
        { 10, 0x02, 121 },
        { 15, 0x02, 121 },
        { 24, 0x02, 121 },
        { 31, 0x02, 121 },
        { 41, 0x02, 121 },
        { 56, 0x03, 121 },
        { 3, 0x02, 122 },
        { 6, 0x02, 122 },
        { 10, 0x02, 122 },
        { 15, 0x02, 122 },
        { 24, 0x02, 122 },
        { 31, 0x02, 122 },
        { 41, 0x02, 122 },
        { 56, 0x03, 122 },
    },
    /* 68 */
    {
        { 1, 0x02, 38 },
        { 22, 0x03, 38 },
        { 1, 0x02, 42 },
        { 22, 0x03, 42 },
        { 1, 0x02, 44 },
        { 22, 0x03, 44 },
        { 1, 0x02, 59 },
        { 22, 0x03, 59 },
        { 1, 0x02, 88 },
        { 22, 0x03, 88 },
        { 1, 0x02, 90 },
        { 22, 0x03, 90 },
        { 76, 0x00, 0 },
        { 77, 0x00, 0 },
        { 79, 0x00, 0 },
        { 81, 0x00, 0 },
    },
    /* 69 */
    {
        { 2, 0x02, 38 },
        { 9, 0x02, 38 },
        { 23, 0x02, 38 },
        { 40, 0x03, 38 },
        { 2, 0x02, 42 },
        { 9, 0x02, 42 },
        { 23, 0x02, 42 },
        { 40, 0x03, 42 },
        { 2, 0x02, 44 },
        { 9, 0x02, 44 },
        { 23, 0x02, 44 },
        { 40, 0x03, 44 },
        { 2, 0x02, 59 },
        { 9, 0x02, 59 },
        { 23, 0x02, 59 },
        { 40, 0x03, 59 },
    },
    /* 70 */
    {
        { 3, 0x02, 38 },
        { 6, 0x02, 38 },
        { 10, 0x02, 38 },
        { 15, 0x02, 38 },
        { 24, 0x02, 38 },
        { 31, 0x02, 38 },
        { 41, 0x02, 38 },
        { 56, 0x03, 38 },
        { 3, 0x02, 42 },
        { 6, 0x02, 42 },
        { 10, 0x02, 42 },
        { 15, 0x02, 42 },
        { 24, 0x02, 42 },
        { 31, 0x02, 42 },
        { 41, 0x02, 42 },
        { 56, 0x03, 42 },
    },
    /* 71 */
    {
        { 3, 0x02, 44 },
        { 6, 0x02, 44 },
        { 10, 0x02, 44 },
        { 15, 0x02, 44 },
        { 24, 0x02, 44 },
        { 31, 0x02, 44 },
        { 41, 0x02, 44 },
        { 56, 0x03, 44 },
        { 3, 0x02, 59 },
        { 6, 0x02, 59 },
        { 10, 0x02, 59 },
        { 15, 0x02, 59 },
        { 24, 0x02, 59 },
        { 31, 0x02, 59 },
        { 41, 0x02, 59 },
        { 56, 0x03, 59 },
    },
    /* 72 */
    {
        { 2, 0x02, 88 },
        { 9, 0x02, 88 },
        { 23, 0x02, 88 },
        { 40, 0x03, 88 },
        { 2, 0x02, 90 },
        { 9, 0x02, 90 },
        { 23, 0x02, 90 },
        { 40, 0x03, 90 },
        { 0, 0x03, 33 },
        { 0, 0x03, 34 },
        { 0, 0x03, 40 },
        { 0, 0x03, 41 },
        { 0, 0x03, 63 },
        { 80, 0x00, 0 },
        { 82, 0x00, 0 },
        { 84, 0x00, 0 },
    },
    /* 73 */
    {
        { 3, 0x02, 88 },
        { 6, 0x02, 88 },
        { 10, 0x02, 88 },
        { 15, 0x02, 88 },
        { 24, 0x02, 88 },
        { 31, 0x02, 88 },
        { 41, 0x02, 88 },
        { 56, 0x03, 88 },
        { 3, 0x02, 90 },
        { 6, 0x02, 90 },
        { 10, 0x02, 90 },
        { 15, 0x02, 90 },
        { 24, 0x02, 90 },
        { 31, 0x02, 90 },
        { 41, 0x02, 90 },
        { 56, 0x03, 90 },
    },
    /* 74 */
    {
        { 1, 0x02, 33 },
        { 22, 0x03, 33 },
        { 1, 0x02, 34 },
        { 22, 0x03, 34 },
        { 1, 0x02, 40 },
        { 22, 0x03, 40 },
        { 1, 0x02, 41 },
        { 22, 0x03, 41 },
        { 1, 0x02, 63 },
        { 22, 0x03, 63 },
        { 0, 0x03, 39 },
        { 0, 0x03, 43 },
        { 0, 0x03, 124 },
        { 83, 0x00, 0 },
        { 85, 0x00, 0 },
        { 88, 0x00, 0 },
    },
    /* 75 */
    {
        { 2, 0x02, 33 },
        { 9, 0x02, 33 },
        { 23, 0x02, 33 },
        { 40, 0x03, 33 },
        { 2, 0x02, 34 },
        { 9, 0x02, 34 },
        { 23, 0x02, 34 },
        { 40, 0x03, 34 },
        { 2, 0x02, 40 },
        { 9, 0x02, 40 },
        { 23, 0x02, 40 },
        { 40, 0x03, 40 },
        { 2, 0x02, 41 },
        { 9, 0x02, 41 },
        { 23, 0x02, 41 },
        { 40, 0x03, 41 },
    },
    /* 76 */
    {
        { 3, 0x02, 33 },
        { 6, 0x02, 33 },
        { 10, 0x02, 33 },
        { 15, 0x02, 33 },
        { 24, 0x02, 33 },
        { 31, 0x02, 33 },
        { 41, 0x02, 33 },
        { 56, 0x03, 33 },
        { 3, 0x02, 34 },
        { 6, 0x02, 34 },
        { 10, 0x02, 34 },
        { 15, 0x02, 34 },
        { 24, 0x02, 34 },
        { 31, 0x02, 34 },
        { 41, 0x02, 34 },
        { 56, 0x03, 34 },
    },
    /* 77 */
    {
        { 3, 0x02, 40 },
        { 6, 0x02, 40 },
        { 10, 0x02, 40 },
        { 15, 0x02, 40 },
        { 24, 0x02, 40 },
        { 31, 0x02, 40 },
        { 41, 0x02, 40 },
        { 56, 0x03, 40 },
        { 3, 0x02, 41 },
        { 6, 0x02, 41 },
        { 10, 0x02, 41 },
        { 15, 0x02, 41 },
        { 24, 0x02, 41 },
        { 31, 0x02, 41 },
        { 41, 0x02, 41 },
        { 56, 0x03, 41 },
    },
    /* 78 */
    {
        { 2, 0x02, 63 },
        { 9, 0x02, 63 },
        { 23, 0x02, 63 },
        { 40, 0x03, 63 },
        { 1, 0x02, 39 },
        { 22, 0x03, 39 },
        { 1, 0x02, 43 },
        { 22, 0x03, 43 },
        { 1, 0x02, 124 },
        { 22, 0x03, 124 },
        { 0, 0x03, 35 },
        { 0, 0x03, 62 },
        { 86, 0x00, 0 },
        { 87, 0x00, 0 },
        { 89, 0x00, 0 },
        { 90, 0x00, 0 },
    },
    /* 79 */
    {
        { 3, 0x02, 63 },
        { 6, 0x02, 63 },
        { 10, 0x02, 63 },
        { 15, 0x02, 63 },
        { 24, 0x02, 63 },
        { 31, 0x02, 63 },
        { 41, 0x02, 63 },
        { 56, 0x03, 63 },
        { 2, 0x02, 39 },
        { 9, 0x02, 39 },
        { 23, 0x02, 39 },
        { 40, 0x03, 39 },
        { 2, 0x02, 43 },
        { 9, 0x02, 43 },
        { 23, 0x02, 43 },
        { 40, 0x03, 43 },
    },
    /* 80 */
    {
        { 3, 0x02, 39 },
        { 6, 0x02, 39 },
        { 10, 0x02, 39 },
        { 15, 0x02, 39 },
        { 24, 0x02, 39 },
        { 31, 0x02, 39 },
        { 41, 0x02, 39 },
        { 56, 0x03, 39 },
        { 3, 0x02, 43 },
        { 6, 0x02, 43 },
        { 10, 0x02, 43 },
        { 15, 0x02, 43 },
        { 24, 0x02, 43 },
        { 31, 0x02, 43 },
        { 41, 0x02, 43 },
        { 56, 0x03, 43 },
    },
    /* 81 */
    {
        { 2, 0x02, 124 },
        { 9, 0x02, 124 },
        { 23, 0x02, 124 },
        { 40, 0x03, 124 },
        { 1, 0x02, 35 },
        { 22, 0x03, 35 },
        { 1, 0x02, 62 },
        { 22, 0x03, 62 },
        { 0, 0x03, 0 },
        { 0, 0x03, 36 },
        { 0, 0x03, 64 },
        { 0, 0x03, 91 },
        { 0, 0x03, 93 },
        { 0, 0x03, 126 },
        { 91, 0x00, 0 },
        { 92, 0x00, 0 },
    },
    /* 82 */
    {
        { 3, 0x02, 124 },
        { 6, 0x02, 124 },
        { 10, 0x02, 124 },
        { 15, 0x02, 124 },
        { 24, 0x02, 124 },
        { 31, 0x02, 124 },
        { 41, 0x02, 124 },
        { 56, 0x03, 124 },
        { 2, 0x02, 35 },
        { 9, 0x02, 35 },
        { 23, 0x02, 35 },
        { 40, 0x03, 35 },
        { 2, 0x02, 62 },
        { 9, 0x02, 62 },
        { 23, 0x02, 62 },
        { 40, 0x03, 62 },
    },
    /* 83 */
    {
        { 3, 0x02, 35 },
        { 6, 0x02, 35 },
        { 10, 0x02, 35 },
        { 15, 0x02, 35 },
        { 24, 0x02, 35 },
        { 31, 0x02, 35 },
        { 41, 0x02, 35 },
        { 56, 0x03, 35 },
        { 3, 0x02, 62 },
        { 6, 0x02, 62 },
        { 10, 0x02, 62 },
        { 15, 0x02, 62 },
        { 24, 0x02, 62 },
        { 31, 0x02, 62 },
        { 41, 0x02, 62 },
        { 56, 0x03, 62 },
    },
    /* 84 */
    {
        { 1, 0x02, 0 },
        { 22, 0x03, 0 },
        { 1, 0x02, 36 },
        { 22, 0x03, 36 },
        { 1, 0x02, 64 },
        { 22, 0x03, 64 },
        { 1, 0x02, 91 },
        { 22, 0x03, 91 },
        { 1, 0x02, 93 },
        { 22, 0x03, 93 },
        { 1, 0x02, 126 },
        { 22, 0x03, 126 },
        { 0, 0x03, 94 },
        { 0, 0x03, 125 },
        { 93, 0x00, 0 },
        { 94, 0x00, 0 },
    },
    /* 85 */
    {
        { 2, 0x02, 0 },
        { 9, 0x02, 0 },
        { 23, 0x02, 0 },
        { 40, 0x03, 0 },
        { 2, 0x02, 36 },
        { 9, 0x02, 36 },
        { 23, 0x02, 36 },
        { 40, 0x03, 36 },
        { 2, 0x02, 64 },
        { 9, 0x02, 64 },
        { 23, 0x02, 64 },
        { 40, 0x03, 64 },
        { 2, 0x02, 91 },
        { 9, 0x02, 91 },
        { 23, 0x02, 91 },
        { 40, 0x03, 91 },
    },
    /* 86 */
    {
        { 3, 0x02, 0 },
        { 6, 0x02, 0 },
        { 10, 0x02, 0 },
        { 15, 0x02, 0 },
        { 24, 0x02, 0 },
        { 31, 0x02, 0 },
        { 41, 0x02, 0 },
        { 56, 0x03, 0 },
        { 3, 0x02, 36 },
        { 6, 0x02, 36 },
        { 10, 0x02, 36 },
        { 15, 0x02, 36 },
        { 24, 0x02, 36 },
        { 31, 0x02, 36 },
        { 41, 0x02, 36 },
        { 56, 0x03, 36 },
    },
    /* 87 */
    {
        { 3, 0x02, 64 },
        { 6, 0x02, 64 },
        { 10, 0x02, 64 },
        { 15, 0x02, 64 },
        { 24, 0x02, 64 },
        { 31, 0x02, 64 },
        { 41, 0x02, 64 },
        { 56, 0x03, 64 },
        { 3, 0x02, 91 },
        { 6, 0x02, 91 },
        { 10, 0x02, 91 },
        { 15, 0x02, 91 },
        { 24, 0x02, 91 },
        { 31, 0x02, 91 },
        { 41, 0x02, 91 },
        { 56, 0x03, 91 },
    },
    /* 88 */
    {
        { 2, 0x02, 93 },
        { 9, 0x02, 93 },
        { 23, 0x02, 93 },
        { 40, 0x03, 93 },
        { 2, 0x02, 126 },
        { 9, 0x02, 126 },
        { 23, 0x02, 126 },
        { 40, 0x03, 126 },
        { 1, 0x02, 94 },
        { 22, 0x03, 94 },
        { 1, 0x02, 125 },
        { 22, 0x03, 125 },
        { 0, 0x03, 60 },
        { 0, 0x03, 96 },
        { 0, 0x03, 123 },
        { 95, 0x00, 0 },
    },
    /* 89 */
    {
        { 3, 0x02, 93 },
        { 6, 0x02, 93 },
        { 10, 0x02, 93 },
        { 15, 0x02, 93 },
        { 24, 0x02, 93 },
        { 31, 0x02, 93 },
        { 41, 0x02, 93 },
        { 56, 0x03, 93 },
        { 3, 0x02, 126 },
        { 6, 0x02, 126 },
        { 10, 0x02, 126 },
        { 15, 0x02, 126 },
        { 24, 0x02, 126 },
        { 31, 0x02, 126 },
        { 41, 0x02, 126 },
        { 56, 0x03, 126 },
    },
    /* 90 */
    {
        { 2, 0x02, 94 },
        { 9, 0x02, 94 },
        { 23, 0x02, 94 },
        { 40, 0x03, 94 },
        { 2, 0x02, 125 },
        { 9, 0x02, 125 },
        { 23, 0x02, 125 },
        { 40, 0x03, 125 },
        { 1, 0x02, 60 },
        { 22, 0x03, 60 },
        { 1, 0x02, 96 },
        { 22, 0x03, 96 },
        { 1, 0x02, 123 },
        { 22, 0x03, 123 },
        { 96, 0x00, 0 },
        { 110, 0x00, 0 },
    },
    /* 91 */
    {
        { 3, 0x02, 94 },
        { 6, 0x02, 94 },
        { 10, 0x02, 94 },
        { 15, 0x02, 94 },
        { 24, 0x02, 94 },
        { 31, 0x02, 94 },
        { 41, 0x02, 94 },
        { 56, 0x03, 94 },
        { 3, 0x02, 125 },
        { 6, 0x02, 125 },
        { 10, 0x02, 125 },
        { 15, 0x02, 125 },
        { 24, 0x02, 125 },
        { 31, 0x02, 125 },
        { 41, 0x02, 125 },
        { 56, 0x03, 125 },
    },
    /* 92 */
    {
        { 2, 0x02, 60 },
        { 9, 0x02, 60 },
        { 23, 0x02, 60 },
        { 40, 0x03, 60 },
        { 2, 0x02, 96 },
        { 9, 0x02, 96 },
        { 23, 0x02, 96 },
        { 40, 0x03, 96 },
        { 2, 0x02, 123 },
        { 9, 0x02, 123 },
        { 23, 0x02, 123 },
        { 40, 0x03, 123 },
        { 97, 0x00, 0 },
        { 101, 0x00, 0 },
        { 111, 0x00, 0 },
        { 133, 0x00, 0 },
    },
    /* 93 */
    {
        { 3, 0x02, 60 },
        { 6, 0x02, 60 },
        { 10, 0x02, 60 },
        { 15, 0x02, 60 },
        { 24, 0x02, 60 },
        { 31, 0x02, 60 },
        { 41, 0x02, 60 },
        { 56, 0x03, 60 },
        { 3, 0x02, 96 },
        { 6, 0x02, 96 },
        { 10, 0x02, 96 },
        { 15, 0x02, 96 },
        { 24, 0x02, 96 },
        { 31, 0x02, 96 },
        { 41, 0x02, 96 },
        { 56, 0x03, 96 },
    },
    /* 94 */
    {
        { 3, 0x02, 123 },
        { 6, 0x02, 123 },
        { 10, 0x02, 123 },
        { 15, 0x02, 123 },
        { 24, 0x02, 123 },
        { 31, 0x02, 123 },
        { 41, 0x02, 123 },
        { 56, 0x03, 123 },
        { 98, 0x00, 0 },
        { 99, 0x00, 0 },
        { 102, 0x00, 0 },
        { 105, 0x00, 0 },
        { 112, 0x00, 0 },
        { 119, 0x00, 0 },
        { 134, 0x00, 0 },
        { 153, 0x00, 0 },
    },
    /* 95 */
    {
        { 0, 0x03, 92 },
        { 0, 0x03, 195 },
        { 0, 0x03, 208 },
        { 100, 0x00, 0 },
        { 103, 0x00, 0 },
        { 104, 0x00, 0 },
        { 106, 0x00, 0 },
        { 107, 0x00, 0 },
        { 113, 0x00, 0 },
        { 116, 0x00, 0 },
        { 120, 0x00, 0 },
        { 126, 0x00, 0 },
        { 135, 0x00, 0 },
        { 142, 0x00, 0 },
        { 154, 0x00, 0 },
        { 169, 0x00, 0 },
    },
    /* 96 */
    {
        { 1, 0x02, 92 },
        { 22, 0x03, 92 },
        { 1, 0x02, 195 },
        { 22, 0x03, 195 },
        { 1, 0x02, 208 },
        { 22, 0x03, 208 },
        { 0, 0x03, 128 },
        { 0, 0x03, 130 },
        { 0, 0x03, 131 },
        { 0, 0x03, 162 },
        { 0, 0x03, 184 },
        { 0, 0x03, 194 },
        { 0, 0x03, 224 },
        { 0, 0x03, 226 },
        { 108, 0x00, 0 },
        { 109, 0x00, 0 },
    },
    /* 97 */
    {
        { 2, 0x02, 92 },
        { 9, 0x02, 92 },
        { 23, 0x02, 92 },
        { 40, 0x03, 92 },
        { 2, 0x02, 195 },
        { 9, 0x02, 195 },
        { 23, 0x02, 195 },
        { 40, 0x03, 195 },
        { 2, 0x02, 208 },
        { 9, 0x02, 208 },
        { 23, 0x02, 208 },
        { 40, 0x03, 208 },
        { 1, 0x02, 128 },
        { 22, 0x03, 128 },
        { 1, 0x02, 130 },
        { 22, 0x03, 130 },
    },
    /* 98 */
    {
        { 3, 0x02, 92 },
        { 6, 0x02, 92 },
        { 10, 0x02, 92 },
        { 15, 0x02, 92 },
        { 24, 0x02, 92 },
        { 31, 0x02, 92 },
        { 41, 0x02, 92 },
        { 56, 0x03, 92 },
        { 3, 0x02, 195 },
        { 6, 0x02, 195 },
        { 10, 0x02, 195 },
        { 15, 0x02, 195 },
        { 24, 0x02, 195 },
        { 31, 0x02, 195 },
        { 41, 0x02, 195 },
        { 56, 0x03, 195 },
    },
    /* 99 */
    {
        { 3, 0x02, 208 },
        { 6, 0x02, 208 },
        { 10, 0x02, 208 },
        { 15, 0x02, 208 },
        { 24, 0x02, 208 },
        { 31, 0x02, 208 },
        { 41, 0x02, 208 },
        { 56, 0x03, 208 },
        { 2, 0x02, 128 },
        { 9, 0x02, 128 },
        { 23, 0x02, 128 },
        { 40, 0x03, 128 },
        { 2, 0x02, 130 },
        { 9, 0x02, 130 },
        { 23, 0x02, 130 },
        { 40, 0x03, 130 },
    },
    /* 100 */
    {
        { 3, 0x02, 128 },
        { 6, 0x02, 128 },
        { 10, 0x02, 128 },
        { 15, 0x02, 128 },
        { 24, 0x02, 128 },
        { 31, 0x02, 128 },
        { 41, 0x02, 128 },
        { 56, 0x03, 128 },
        { 3, 0x02, 130 },
        { 6, 0x02, 130 },
        { 10, 0x02, 130 },
        { 15, 0x02, 130 },
        { 24, 0x02, 130 },
        { 31, 0x02, 130 },
        { 41, 0x02, 130 },
        { 56, 0x03, 130 },
    },
    /* 101 */
    {
        { 1, 0x02, 131 },
        { 22, 0x03, 131 },
        { 1, 0x02, 162 },
        { 22, 0x03, 162 },
        { 1, 0x02, 184 },
        { 22, 0x03, 184 },
        { 1, 0x02, 194 },
        { 22, 0x03, 194 },
        { 1, 0x02, 224 },
        { 22, 0x03, 224 },
        { 1, 0x02, 226 },
        { 22, 0x03, 226 },
        { 0, 0x03, 153 },
        { 0, 0x03, 161 },
        { 0, 0x03, 167 },
        { 0, 0x03, 172 },
    },
    /* 102 */
    {
        { 2, 0x02, 131 },
        { 9, 0x02, 131 },
        { 23, 0x02, 131 },
        { 40, 0x03, 131 },
        { 2, 0x02, 162 },
        { 9, 0x02, 162 },
        { 23, 0x02, 162 },
        { 40, 0x03, 162 },
        { 2, 0x02, 184 },
        { 9, 0x02, 184 },
        { 23, 0x02, 184 },
        { 40, 0x03, 184 },
        { 2, 0x02, 194 },
        { 9, 0x02, 194 },
        { 23, 0x02, 194 },
        { 40, 0x03, 194 },
    },
    /* 103 */
    {
        { 3, 0x02, 131 },
        { 6, 0x02, 131 },
        { 10, 0x02, 131 },
        { 15, 0x02, 131 },
        { 24, 0x02, 131 },
        { 31, 0x02, 131 },
        { 41, 0x02, 131 },
        { 56, 0x03, 131 },
        { 3, 0x02, 162 },
        { 6, 0x02, 162 },
        { 10, 0x02, 162 },
        { 15, 0x02, 162 },
        { 24, 0x02, 162 },
        { 31, 0x02, 162 },
        { 41, 0x02, 162 },
        { 56, 0x03, 162 },
    },
    /* 104 */
    {
        { 3, 0x02, 184 },
        { 6, 0x02, 184 },
        { 10, 0x02, 184 },
        { 15, 0x02, 184 },
        { 24, 0x02, 184 },
        { 31, 0x02, 184 },
        { 41, 0x02, 184 },
        { 56, 0x03, 184 },
        { 3, 0x02, 194 },
        { 6, 0x02, 194 },
        { 10, 0x02, 194 },
        { 15, 0x02, 194 },
        { 24, 0x02, 194 },
        { 31, 0x02, 194 },
        { 41, 0x02, 194 },
        { 56, 0x03, 194 },
    },
    /* 105 */
    {
        { 2, 0x02, 224 },
        { 9, 0x02, 224 },
        { 23, 0x02, 224 },
        { 40, 0x03, 224 },
        { 2, 0x02, 226 },
        { 9, 0x02, 226 },
        { 23, 0x02, 226 },
        { 40, 0x03, 226 },
        { 1, 0x02, 153 },
        { 22, 0x03, 153 },
        { 1, 0x02, 161 },
        { 22, 0x03, 161 },
        { 1, 0x02, 167 },
        { 22, 0x03, 167 },
        { 1, 0x02, 172 },
        { 22, 0x03, 172 },
    },
    /* 106 */
    {
        { 3, 0x02, 224 },
        { 6, 0x02, 224 },
        { 10, 0x02, 224 },
        { 15, 0x02, 224 },
        { 24, 0x02, 224 },
        { 31, 0x02, 224 },
        { 41, 0x02, 224 },
        { 56, 0x03, 224 },
        { 3, 0x02, 226 },
        { 6, 0x02, 226 },
        { 10, 0x02, 226 },
        { 15, 0x02, 226 },
        { 24, 0x02, 226 },
        { 31, 0x02, 226 },
        { 41, 0x02, 226 },
        { 56, 0x03, 226 },
    },
    /* 107 */
    {
        { 2, 0x02, 153 },
        { 9, 0x02, 153 },
        { 23, 0x02, 153 },
        { 40, 0x03, 153 },
        { 2, 0x02, 161 },
        { 9, 0x02, 161 },
        { 23, 0x02, 161 },
        { 40, 0x03, 161 },
        { 2, 0x02, 167 },
        { 9, 0x02, 167 },
        { 23, 0x02, 167 },
        { 40, 0x03, 167 },
        { 2, 0x02, 172 },
        { 9, 0x02, 172 },
        { 23, 0x02, 172 },
        { 40, 0x03, 172 },
    },
    /* 108 */
    {
        { 3, 0x02, 153 },
        { 6, 0x02, 153 },
        { 10, 0x02, 153 },
        { 15, 0x02, 153 },
        { 24, 0x02, 153 },
        { 31, 0x02, 153 },
        { 41, 0x02, 153 },
        { 56, 0x03, 153 },
        { 3, 0x02, 161 },
        { 6, 0x02, 161 },
        { 10, 0x02, 161 },
        { 15, 0x02, 161 },
        { 24, 0x02, 161 },
        { 31, 0x02, 161 },
        { 41, 0x02, 161 },
        { 56, 0x03, 161 },
    },
    /* 109 */
    {
        { 3, 0x02, 167 },
        { 6, 0x02, 167 },
        { 10, 0x02, 167 },
        { 15, 0x02, 167 },
        { 24, 0x02, 167 },
        { 31, 0x02, 167 },
        { 41, 0x02, 167 },
        { 56, 0x03, 167 },
        { 3, 0x02, 172 },
        { 6, 0x02, 172 },
        { 10, 0x02, 172 },
        { 15, 0x02, 172 },
        { 24, 0x02, 172 },
        { 31, 0x02, 172 },
        { 41, 0x02, 172 },
        { 56, 0x03, 172 },
    },
    /* 110 */
    {
        { 114, 0x00, 0 },
        { 115, 0x00, 0 },
        { 117, 0x00, 0 },
        { 118, 0x00, 0 },
        { 121, 0x00, 0 },
        { 123, 0x00, 0 },
        { 127, 0x00, 0 },
        { 130, 0x00, 0 },
        { 136, 0x00, 0 },
        { 139, 0x00, 0 },
        { 143, 0x00, 0 },
        { 146, 0x00, 0 },
        { 155, 0x00, 0 },
        { 162, 0x00, 0 },
        { 170, 0x00, 0 },
        { 180, 0x00, 0 },
    },
    /* 111 */
    {
        { 0, 0x03, 176 },
        { 0, 0x03, 177 },
        { 0, 0x03, 179 },
        { 0, 0x03, 209 },
        { 0, 0x03, 216 },
        { 0, 0x03, 217 },
        { 0, 0x03, 227 },
        { 0, 0x03, 229 },
        { 0, 0x03, 230 },
        { 122, 0x00, 0 },
        { 124, 0x00, 0 },
        { 125, 0x00, 0 },
        { 128, 0x00, 0 },
        { 129, 0x00, 0 },
        { 131, 0x00, 0 },
        { 132, 0x00, 0 },
    },
    /* 112 */
    {
        { 1, 0x02, 176 },
        { 22, 0x03, 176 },
        { 1, 0x02, 177 },
        { 22, 0x03, 177 },
        { 1, 0x02, 179 },
        { 22, 0x03, 179 },
        { 1, 0x02, 209 },
        { 22, 0x03, 209 },
        { 1, 0x02, 216 },
        { 22, 0x03, 216 },
        { 1, 0x02, 217 },
        { 22, 0x03, 217 },
        { 1, 0x02, 227 },
        { 22, 0x03, 227 },
        { 1, 0x02, 229 },
        { 22, 0x03, 229 },
    },
    /* 113 */
    {
        { 2, 0x02, 176 },
        { 9, 0x02, 176 },
        { 23, 0x02, 176 },
        { 40, 0x03, 176 },
        { 2, 0x02, 177 },
        { 9, 0x02, 177 },
        { 23, 0x02, 177 },
        { 40, 0x03, 177 },
        { 2, 0x02, 179 },
        { 9, 0x02, 179 },
        { 23, 0x02, 179 },
        { 40, 0x03, 179 },
        { 2, 0x02, 209 },
        { 9, 0x02, 209 },
        { 23, 0x02, 209 },
        { 40, 0x03, 209 },
    },
    /* 114 */
    {
        { 3, 0x02, 176 },
        { 6, 0x02, 176 },
        { 10, 0x02, 176 },
        { 15, 0x02, 176 },
        { 24, 0x02, 176 },
        { 31, 0x02, 176 },
        { 41, 0x02, 176 },
        { 56, 0x03, 176 },
        { 3, 0x02, 177 },
        { 6, 0x02, 177 },
        { 10, 0x02, 177 },
        { 15, 0x02, 177 },
        { 24, 0x02, 177 },
        { 31, 0x02, 177 },
        { 41, 0x02, 177 },
        { 56, 0x03, 177 },
    },
    /* 115 */
    {
        { 3, 0x02, 179 },
        { 6, 0x02, 179 },
        { 10, 0x02, 179 },
        { 15, 0x02, 179 },
        { 24, 0x02, 179 },
        { 31, 0x02, 179 },
        { 41, 0x02, 179 },
        { 56, 0x03, 179 },
        { 3, 0x02, 209 },
        { 6, 0x02, 209 },
        { 10, 0x02, 209 },
        { 15, 0x02, 209 },
        { 24, 0x02, 209 },
        { 31, 0x02, 209 },
        { 41, 0x02, 209 },
        { 56, 0x03, 209 },
    },
    /* 116 */
    {
        { 2, 0x02, 216 },
        { 9, 0x02, 216 },
        { 23, 0x02, 216 },
        { 40, 0x03, 216 },
        { 2, 0x02, 217 },
        { 9, 0x02, 217 },
        { 23, 0x02, 217 },
        { 40, 0x03, 217 },
        { 2, 0x02, 227 },
        { 9, 0x02, 227 },
        { 23, 0x02, 227 },
        { 40, 0x03, 227 },
        { 2, 0x02, 229 },
        { 9, 0x02, 229 },
        { 23, 0x02, 229 },
        { 40, 0x03, 229 },
    },
    /* 117 */
    {
        { 3, 0x02, 216 },
        { 6, 0x02, 216 },
        { 10, 0x02, 216 },
        { 15, 0x02, 216 },
        { 24, 0x02, 216 },
        { 31, 0x02, 216 },
        { 41, 0x02, 216 },
        { 56, 0x03, 216 },
        { 3, 0x02, 217 },
        { 6, 0x02, 217 },
        { 10, 0x02, 217 },
        { 15, 0x02, 217 },
        { 24, 0x02, 217 },
        { 31, 0x02, 217 },
        { 41, 0x02, 217 },
        { 56, 0x03, 217 },
    },
    /* 118 */
    {
        { 3, 0x02, 227 },
        { 6, 0x02, 227 },
        { 10, 0x02, 227 },
        { 15, 0x02, 227 },
        { 24, 0x02, 227 },
        { 31, 0x02, 227 },
        { 41, 0x02, 227 },
        { 56, 0x03, 227 },
        { 3, 0x02, 229 },
        { 6, 0x02, 229 },
        { 10, 0x02, 229 },
        { 15, 0x02, 229 },
        { 24, 0x02, 229 },
        { 31, 0x02, 229 },
        { 41, 0x02, 229 },
        { 56, 0x03, 229 },
    },
    /* 119 */
    {
        { 1, 0x02, 230 },
        { 22, 0x03, 230 },
        { 0, 0x03, 129 },
        { 0, 0x03, 132 },
        { 0, 0x03, 133 },
        { 0, 0x03, 134 },
        { 0, 0x03, 136 },
        { 0, 0x03, 146 },
        { 0, 0x03, 154 },
        { 0, 0x03, 156 },
        { 0, 0x03, 160 },
        { 0, 0x03, 163 },
        { 0, 0x03, 164 },
        { 0, 0x03, 169 },
        { 0, 0x03, 170 },
        { 0, 0x03, 173 },
    },
    /* 120 */
    {
        { 2, 0x02, 230 },
        { 9, 0x02, 230 },
        { 23, 0x02, 230 },
        { 40, 0x03, 230 },
        { 1, 0x02, 129 },
        { 22, 0x03, 129 },
        { 1, 0x02, 132 },
        { 22, 0x03, 132 },
        { 1, 0x02, 133 },
        { 22, 0x03, 133 },
        { 1, 0x02, 134 },
        { 22, 0x03, 134 },
        { 1, 0x02, 136 },
        { 22, 0x03, 136 },
        { 1, 0x02, 146 },
        { 22, 0x03, 146 },
    },
    /* 121 */
    {
        { 3, 0x02, 230 },
        { 6, 0x02, 230 },
        { 10, 0x02, 230 },
        { 15, 0x02, 230 },
        { 24, 0x02, 230 },
        { 31, 0x02, 230 },
        { 41, 0x02, 230 },
        { 56, 0x03, 230 },
        { 2, 0x02, 129 },
        { 9, 0x02, 129 },
        { 23, 0x02, 129 },
        { 40, 0x03, 129 },
        { 2, 0x02, 132 },
        { 9, 0x02, 132 },
        { 23, 0x02, 132 },
        { 40, 0x03, 132 },
    },
    /* 122 */
    {
        { 3, 0x02, 129 },
        { 6, 0x02, 129 },
        { 10, 0x02, 129 },
        { 15, 0x02, 129 },
        { 24, 0x02, 129 },
        { 31, 0x02, 129 },
        { 41, 0x02, 129 },
        { 56, 0x03, 129 },
        { 3, 0x02, 132 },
        { 6, 0x02, 132 },
        { 10, 0x02, 132 },
        { 15, 0x02, 132 },
        { 24, 0x02, 132 },
        { 31, 0x02, 132 },
        { 41, 0x02, 132 },
        { 56, 0x03, 132 },
    },
    /* 123 */
    {
        { 2, 0x02, 133 },
        { 9, 0x02, 133 },
        { 23, 0x02, 133 },
        { 40, 0x03, 133 },
        { 2, 0x02, 134 },
        { 9, 0x02, 134 },
        { 23, 0x02, 134 },
        { 40, 0x03, 134 },
        { 2, 0x02, 136 },
        { 9, 0x02, 136 },
        { 23, 0x02, 136 },
        { 40, 0x03, 136 },
        { 2, 0x02, 146 },
        { 9, 0x02, 146 },
        { 23, 0x02, 146 },
        { 40, 0x03, 146 },
    },
    /* 124 */
    {
        { 3, 0x02, 133 },
        { 6, 0x02, 133 },
        { 10, 0x02, 133 },
        { 15, 0x02, 133 },
        { 24, 0x02, 133 },
        { 31, 0x02, 133 },
        { 41, 0x02, 133 },
        { 56, 0x03, 133 },
        { 3, 0x02, 134 },
        { 6, 0x02, 134 },
        { 10, 0x02, 134 },
        { 15, 0x02, 134 },
        { 24, 0x02, 134 },
        { 31, 0x02, 134 },
        { 41, 0x02, 134 },
        { 56, 0x03, 134 },
    },
    /* 125 */
    {
        { 3, 0x02, 136 },
        { 6, 0x02, 136 },
        { 10, 0x02, 136 },
        { 15, 0x02, 136 },
        { 24, 0x02, 136 },
        { 31, 0x02, 136 },
        { 41, 0x02, 136 },
        { 56, 0x03, 136 },
        { 3, 0x02, 146 },
        { 6, 0x02, 146 },
        { 10, 0x02, 146 },
        { 15, 0x02, 146 },
        { 24, 0x02, 146 },
        { 31, 0x02, 146 },
        { 41, 0x02, 146 },
        { 56, 0x03, 146 },
    },
    /* 126 */
    {
        { 1, 0x02, 154 },
        { 22, 0x03, 154 },
        { 1, 0x02, 156 },
        { 22, 0x03, 156 },
        { 1, 0x02, 160 },
        { 22, 0x03, 160 },
        { 1, 0x02, 163 },
        { 22, 0x03, 163 },
        { 1, 0x02, 164 },
        { 22, 0x03, 164 },
        { 1, 0x02, 169 },
        { 22, 0x03, 169 },
        { 1, 0x02, 170 },
        { 22, 0x03, 170 },
        { 1, 0x02, 173 },
        { 22, 0x03, 173 },
    },
    /* 127 */
    {
        { 2, 0x02, 154 },
        { 9, 0x02, 154 },
        { 23, 0x02, 154 },
        { 40, 0x03, 154 },
        { 2, 0x02, 156 },
        { 9, 0x02, 156 },
        { 23, 0x02, 156 },
        { 40, 0x03, 156 },
        { 2, 0x02, 160 },
        { 9, 0x02, 160 },
        { 23, 0x02, 160 },
        { 40, 0x03, 160 },
        { 2, 0x02, 163 },
        { 9, 0x02, 163 },
        { 23, 0x02, 163 },
        { 40, 0x03, 163 },
    },
    /* 128 */
    {
        { 3, 0x02, 154 },
        { 6, 0x02, 154 },
        { 10, 0x02, 154 },
        { 15, 0x02, 154 },
        { 24, 0x02, 154 },
        { 31, 0x02, 154 },
        { 41, 0x02, 154 },
        { 56, 0x03, 154 },
        { 3, 0x02, 156 },
        { 6, 0x02, 156 },
        { 10, 0x02, 156 },
        { 15, 0x02, 156 },
        { 24, 0x02, 156 },
        { 31, 0x02, 156 },
        { 41, 0x02, 156 },
        { 56, 0x03, 156 },
    },
    /* 129 */
    {
        { 3, 0x02, 160 },
        { 6, 0x02, 160 },
        { 10, 0x02, 160 },
        { 15, 0x02, 160 },
        { 24, 0x02, 160 },
        { 31, 0x02, 160 },
        { 41, 0x02, 160 },
        { 56, 0x03, 160 },
        { 3, 0x02, 163 },
        { 6, 0x02, 163 },
        { 10, 0x02, 163 },
        { 15, 0x02, 163 },
        { 24, 0x02, 163 },
        { 31, 0x02, 163 },
        { 41, 0x02, 163 },
        { 56, 0x03, 163 },
    },
    /* 130 */
    {
        { 2, 0x02, 164 },
        { 9, 0x02, 164 },
        { 23, 0x02, 164 },
        { 40, 0x03, 164 },
        { 2, 0x02, 169 },
        { 9, 0x02, 169 },
        { 23, 0x02, 169 },
        { 40, 0x03, 169 },
        { 2, 0x02, 170 },
        { 9, 0x02, 170 },
        { 23, 0x02, 170 },
        { 40, 0x03, 170 },
        { 2, 0x02, 173 },
        { 9, 0x02, 173 },
        { 23, 0x02, 173 },
        { 40, 0x03, 173 },
    },
    /* 131 */
    {
        { 3, 0x02, 164 },
        { 6, 0x02, 164 },
        { 10, 0x02, 164 },
        { 15, 0x02, 164 },
        { 24, 0x02, 164 },
        { 31, 0x02, 164 },
        { 41, 0x02, 164 },
        { 56, 0x03, 164 },
        { 3, 0x02, 169 },
        { 6, 0x02, 169 },
        { 10, 0x02, 169 },
        { 15, 0x02, 169 },
        { 24, 0x02, 169 },
        { 31, 0x02, 169 },
        { 41, 0x02, 169 },
        { 56, 0x03, 169 },
    },
    /* 132 */
    {
        { 3, 0x02, 170 },
        { 6, 0x02, 170 },
        { 10, 0x02, 170 },
        { 15, 0x02, 170 },
        { 24, 0x02, 170 },
        { 31, 0x02, 170 },
        { 41, 0x02, 170 },
        { 56, 0x03, 170 },
        { 3, 0x02, 173 },
        { 6, 0x02, 173 },
        { 10, 0x02, 173 },
        { 15, 0x02, 173 },
        { 24, 0x02, 173 },
        { 31, 0x02, 173 },
        { 41, 0x02, 173 },
        { 56, 0x03, 173 },
    },
    /* 133 */
    {
        { 137, 0x00, 0 },
        { 138, 0x00, 0 },
        { 140, 0x00, 0 },
        { 141, 0x00, 0 },
        { 144, 0x00, 0 },
        { 145, 0x00, 0 },
        { 147, 0x00, 0 },
        { 150, 0x00, 0 },
        { 156, 0x00, 0 },
        { 159, 0x00, 0 },
        { 163, 0x00, 0 },
        { 166, 0x00, 0 },
        { 171, 0x00, 0 },
        { 174, 0x00, 0 },
        { 181, 0x00, 0 },
        { 190, 0x00, 0 },
    },
    /* 134 */
    {
        { 0, 0x03, 178 },
        { 0, 0x03, 181 },
        { 0, 0x03, 185 },
        { 0, 0x03, 186 },
        { 0, 0x03, 187 },
        { 0, 0x03, 189 },
        { 0, 0x03, 190 },
        { 0, 0x03, 196 },
        { 0, 0x03, 198 },
        { 0, 0x03, 228 },
        { 0, 0x03, 232 },
        { 0, 0x03, 233 },
        { 148, 0x00, 0 },
        { 149, 0x00, 0 },
        { 151, 0x00, 0 },
        { 152, 0x00, 0 },
    },
    /* 135 */
    {
        { 1, 0x02, 178 },
        { 22, 0x03, 178 },
        { 1, 0x02, 181 },
        { 22, 0x03, 181 },
        { 1, 0x02, 185 },
        { 22, 0x03, 185 },
        { 1, 0x02, 186 },
        { 22, 0x03, 186 },
        { 1, 0x02, 187 },
        { 22, 0x03, 187 },
        { 1, 0x02, 189 },
        { 22, 0x03, 189 },
        { 1, 0x02, 190 },
        { 22, 0x03, 190 },
        { 1, 0x02, 196 },
        { 22, 0x03, 196 },
    },
    /* 136 */
    {
        { 2, 0x02, 178 },
        { 9, 0x02, 178 },
        { 23, 0x02, 178 },
        { 40, 0x03, 178 },
        { 2, 0x02, 181 },
        { 9, 0x02, 181 },
        { 23, 0x02, 181 },
        { 40, 0x03, 181 },
        { 2, 0x02, 185 },
        { 9, 0x02, 185 },
        { 23, 0x02, 185 },
        { 40, 0x03, 185 },
        { 2, 0x02, 186 },
        { 9, 0x02, 186 },
        { 23, 0x02, 186 },
        { 40, 0x03, 186 },
    },
    /* 137 */
    {
        { 3, 0x02, 178 },
        { 6, 0x02, 178 },
        { 10, 0x02, 178 },
        { 15, 0x02, 178 },
        { 24, 0x02, 178 },
        { 31, 0x02, 178 },
        { 41, 0x02, 178 },
        { 56, 0x03, 178 },
        { 3, 0x02, 181 },
        { 6, 0x02, 181 },
        { 10, 0x02, 181 },
        { 15, 0x02, 181 },
        { 24, 0x02, 181 },
        { 31, 0x02, 181 },
        { 41, 0x02, 181 },
        { 56, 0x03, 181 },
    },
    /* 138 */
    {
        { 3, 0x02, 185 },
        { 6, 0x02, 185 },
        { 10, 0x02, 185 },
        { 15, 0x02, 185 },
        { 24, 0x02, 185 },
        { 31, 0x02, 185 },
        { 41, 0x02, 185 },
        { 56, 0x03, 185 },
        { 3, 0x02, 186 },
        { 6, 0x02, 186 },
        { 10, 0x02, 186 },
        { 15, 0x02, 186 },
        { 24, 0x02, 186 },
        { 31, 0x02, 186 },
        { 41, 0x02, 186 },
        { 56, 0x03, 186 },
    },
    /* 139 */
    {
        { 2, 0x02, 187 },
        { 9, 0x02, 187 },
        { 23, 0x02, 187 },
        { 40, 0x03, 187 },
        { 2, 0x02, 189 },
        { 9, 0x02, 189 },
        { 23, 0x02, 189 },
        { 40, 0x03, 189 },
        { 2, 0x02, 190 },
        { 9, 0x02, 190 },
        { 23, 0x02, 190 },
        { 40, 0x03, 190 },
        { 2, 0x02, 196 },
        { 9, 0x02, 196 },
        { 23, 0x02, 196 },
        { 40, 0x03, 196 },
    },
    /* 140 */
    {
        { 3, 0x02, 187 },
        { 6, 0x02, 187 },
        { 10, 0x02, 187 },
        { 15, 0x02, 187 },
        { 24, 0x02, 187 },
        { 31, 0x02, 187 },
        { 41, 0x02, 187 },
        { 56, 0x03, 187 },
        { 3, 0x02, 189 },
        { 6, 0x02, 189 },
        { 10, 0x02, 189 },
        { 15, 0x02, 189 },
        { 24, 0x02, 189 },
        { 31, 0x02, 189 },
        { 41, 0x02, 189 },
        { 56, 0x03, 189 },
    },
    /* 141 */
    {
        { 3, 0x02, 190 },
        { 6, 0x02, 190 },
        { 10, 0x02, 190 },
        { 15, 0x02, 190 },
        { 24, 0x02, 190 },
        { 31, 0x02, 190 },
        { 41, 0x02, 190 },
        { 56, 0x03, 190 },
        { 3, 0x02, 196 },
        { 6, 0x02, 196 },
        { 10, 0x02, 196 },
        { 15, 0x02, 196 },
        { 24, 0x02, 196 },
        { 31, 0x02, 196 },
        { 41, 0x02, 196 },
        { 56, 0x03, 196 },
    },
    /* 142 */
    {
        { 1, 0x02, 198 },
        { 22, 0x03, 198 },
        { 1, 0x02, 228 },
        { 22, 0x03, 228 },
        { 1, 0x02, 232 },
        { 22, 0x03, 232 },
        { 1, 0x02, 233 },
        { 22, 0x03, 233 },
        { 0, 0x03, 1 },
        { 0, 0x03, 135 },
        { 0, 0x03, 137 },
        { 0, 0x03, 138 },
        { 0, 0x03, 139 },
        { 0, 0x03, 140 },
        { 0, 0x03, 141 },
        { 0, 0x03, 143 },
    },
    /* 143 */
    {
        { 2, 0x02, 198 },
        { 9, 0x02, 198 },
        { 23, 0x02, 198 },
        { 40, 0x03, 198 },
        { 2, 0x02, 228 },
        { 9, 0x02, 228 },
        { 23, 0x02, 228 },
        { 40, 0x03, 228 },
        { 2, 0x02, 232 },
        { 9, 0x02, 232 },
        { 23, 0x02, 232 },
        { 40, 0x03, 232 },
        { 2, 0x02, 233 },
        { 9, 0x02, 233 },
        { 23, 0x02, 233 },
        { 40, 0x03, 233 },
    },
    /* 144 */
    {
        { 3, 0x02, 198 },
        { 6, 0x02, 198 },
        { 10, 0x02, 198 },
        { 15, 0x02, 198 },
        { 24, 0x02, 198 },
        { 31, 0x02, 198 },
        { 41, 0x02, 198 },
        { 56, 0x03, 198 },
        { 3, 0x02, 228 },
        { 6, 0x02, 228 },
        { 10, 0x02, 228 },
        { 15, 0x02, 228 },
        { 24, 0x02, 228 },
        { 31, 0x02, 228 },
        { 41, 0x02, 228 },
        { 56, 0x03, 228 },
    },
    /* 145 */
    {
        { 3, 0x02, 232 },
        { 6, 0x02, 232 },
        { 10, 0x02, 232 },
        { 15, 0x02, 232 },
        { 24, 0x02, 232 },
        { 31, 0x02, 232 },
        { 41, 0x02, 232 },
        { 56, 0x03, 232 },
        { 3, 0x02, 233 },
        { 6, 0x02, 233 },
        { 10, 0x02, 233 },
        { 15, 0x02, 233 },
        { 24, 0x02, 233 },
        { 31, 0x02, 233 },
        { 41, 0x02, 233 },
        { 56, 0x03, 233 },
    },
    /* 146 */
    {
        { 1, 0x02, 1 },
        { 22, 0x03, 1 },
        { 1, 0x02, 135 },
        { 22, 0x03, 135 },
        { 1, 0x02, 137 },
        { 22, 0x03, 137 },
        { 1, 0x02, 138 },
        { 22, 0x03, 138 },
        { 1, 0x02, 139 },
        { 22, 0x03, 139 },
        { 1, 0x02, 140 },
        { 22, 0x03, 140 },
        { 1, 0x02, 141 },
        { 22, 0x03, 141 },
        { 1, 0x02, 143 },
        { 22, 0x03, 143 },
    },
    /* 147 */
    {
        { 2, 0x02, 1 },
        { 9, 0x02, 1 },
        { 23, 0x02, 1 },
        { 40, 0x03, 1 },
        { 2, 0x02, 135 },
        { 9, 0x02, 135 },
        { 23, 0x02, 135 },
        { 40, 0x03, 135 },
        { 2, 0x02, 137 },
        { 9, 0x02, 137 },
        { 23, 0x02, 137 },
        { 40, 0x03, 137 },
        { 2, 0x02, 138 },
        { 9, 0x02, 138 },
        { 23, 0x02, 138 },
        { 40, 0x03, 138 },
    },
    /* 148 */
    {
        { 3, 0x02, 1 },
        { 6, 0x02, 1 },
        { 10, 0x02, 1 },
        { 15, 0x02, 1 },
        { 24, 0x02, 1 },
        { 31, 0x02, 1 },
        { 41, 0x02, 1 },
        { 56, 0x03, 1 },
        { 3, 0x02, 135 },
        { 6, 0x02, 135 },
        { 10, 0x02, 135 },
        { 15, 0x02, 135 },
        { 24, 0x02, 135 },
        { 31, 0x02, 135 },
        { 41, 0x02, 135 },
        { 56, 0x03, 135 },
    },
    /* 149 */
    {
        { 3, 0x02, 137 },
        { 6, 0x02, 137 },
        { 10, 0x02, 137 },
        { 15, 0x02, 137 },
        { 24, 0x02, 137 },
        { 31, 0x02, 137 },
        { 41, 0x02, 137 },
        { 56, 0x03, 137 },
        { 3, 0x02, 138 },
        { 6, 0x02, 138 },
        { 10, 0x02, 138 },
        { 15, 0x02, 138 },
        { 24, 0x02, 138 },
        { 31, 0x02, 138 },
        { 41, 0x02, 138 },
        { 56, 0x03, 138 },
    },
    /* 150 */
    {
        { 2, 0x02, 139 },
        { 9, 0x02, 139 },
        { 23, 0x02, 139 },
        { 40, 0x03, 139 },
        { 2, 0x02, 140 },
        { 9, 0x02, 140 },
        { 23, 0x02, 140 },
        { 40, 0x03, 140 },
        { 2, 0x02, 141 },
        { 9, 0x02, 141 },
        { 23, 0x02, 141 },
        { 40, 0x03, 141 },
        { 2, 0x02, 143 },
        { 9, 0x02, 143 },
        { 23, 0x02, 143 },
        { 40, 0x03, 143 },
    },
    /* 151 */
    {
        { 3, 0x02, 139 },
        { 6, 0x02, 139 },
        { 10, 0x02, 139 },
        { 15, 0x02, 139 },
        { 24, 0x02, 139 },
        { 31, 0x02, 139 },
        { 41, 0x02, 139 },
        { 56, 0x03, 139 },
        { 3, 0x02, 140 },
        { 6, 0x02, 140 },
        { 10, 0x02, 140 },
        { 15, 0x02, 140 },
        { 24, 0x02, 140 },
        { 31, 0x02, 140 },
        { 41, 0x02, 140 },
        { 56, 0x03, 140 },
    },
    /* 152 */
    {
        { 3, 0x02, 141 },
        { 6, 0x02, 141 },
        { 10, 0x02, 141 },
        { 15, 0x02, 141 },
        { 24, 0x02, 141 },
        { 31, 0x02, 141 },
        { 41, 0x02, 141 },
        { 56, 0x03, 141 },
        { 3, 0x02, 143 },
        { 6, 0x02, 143 },
        { 10, 0x02, 143 },
        { 15, 0x02, 143 },
        { 24, 0x02, 143 },
        { 31, 0x02, 143 },
        { 41, 0x02, 143 },
        { 56, 0x03, 143 },
    },
    /* 153 */
    {
        { 157, 0x00, 0 },
        { 158, 0x00, 0 },
        { 160, 0x00, 0 },
        { 161, 0x00, 0 },
        { 164, 0x00, 0 },
        { 165, 0x00, 0 },
        { 167, 0x00, 0 },
        { 168, 0x00, 0 },
        { 172, 0x00, 0 },
        { 173, 0x00, 0 },
        { 175, 0x00, 0 },
        { 177, 0x00, 0 },
        { 182, 0x00, 0 },
        { 185, 0x00, 0 },
        { 191, 0x00, 0 },
        { 207, 0x00, 0 },
    },
    /* 154 */
    {
        { 0, 0x03, 147 },
        { 0, 0x03, 149 },
        { 0, 0x03, 150 },
        { 0, 0x03, 151 },
        { 0, 0x03, 152 },
        { 0, 0x03, 155 },
        { 0, 0x03, 157 },
        { 0, 0x03, 158 },
        { 0, 0x03, 165 },
        { 0, 0x03, 166 },
        { 0, 0x03, 168 },
        { 0, 0x03, 174 },
        { 0, 0x03, 175 },
        { 0, 0x03, 180 },
        { 0, 0x03, 182 },
        { 0, 0x03, 183 },
    },
    /* 155 */
    {
        { 1, 0x02, 147 },
        { 22, 0x03, 147 },
        { 1, 0x02, 149 },
        { 22, 0x03, 149 },
        { 1, 0x02, 150 },
        { 22, 0x03, 150 },
        { 1, 0x02, 151 },
        { 22, 0x03, 151 },
        { 1, 0x02, 152 },
        { 22, 0x03, 152 },
        { 1, 0x02, 155 },
        { 22, 0x03, 155 },
        { 1, 0x02, 157 },
        { 22, 0x03, 157 },
        { 1, 0x02, 158 },
        { 22, 0x03, 158 },
    },
    /* 156 */
    {
        { 2, 0x02, 147 },
        { 9, 0x02, 147 },
        { 23, 0x02, 147 },
        { 40, 0x03, 147 },
        { 2, 0x02, 149 },
        { 9, 0x02, 149 },
        { 23, 0x02, 149 },
        { 40, 0x03, 149 },
        { 2, 0x02, 150 },
        { 9, 0x02, 150 },
        { 23, 0x02, 150 },
        { 40, 0x03, 150 },
        { 2, 0x02, 151 },
        { 9, 0x02, 151 },
        { 23, 0x02, 151 },
        { 40, 0x03, 151 },
    },
    /* 157 */
    {
        { 3, 0x02, 147 },
        { 6, 0x02, 147 },
        { 10, 0x02, 147 },
        { 15, 0x02, 147 },
        { 24, 0x02, 147 },
        { 31, 0x02, 147 },
        { 41, 0x02, 147 },
        { 56, 0x03, 147 },
        { 3, 0x02, 149 },
        { 6, 0x02, 149 },
        { 10, 0x02, 149 },
        { 15, 0x02, 149 },
        { 24, 0x02, 149 },
        { 31, 0x02, 149 },
        { 41, 0x02, 149 },
        { 56, 0x03, 149 },
    },
    /* 158 */
    {
        { 3, 0x02, 150 },
        { 6, 0x02, 150 },
        { 10, 0x02, 150 },
        { 15, 0x02, 150 },
        { 24, 0x02, 150 },
        { 31, 0x02, 150 },
        { 41, 0x02, 150 },
        { 56, 0x03, 150 },
        { 3, 0x02, 151 },
        { 6, 0x02, 151 },
        { 10, 0x02, 151 },
        { 15, 0x02, 151 },
        { 24, 0x02, 151 },
        { 31, 0x02, 151 },
        { 41, 0x02, 151 },
        { 56, 0x03, 151 },
    },
    /* 159 */
    {
        { 2, 0x02, 152 },
        { 9, 0x02, 152 },
        { 23, 0x02, 152 },
        { 40, 0x03, 152 },
        { 2, 0x02, 155 },
        { 9, 0x02, 155 },
        { 23, 0x02, 155 },
        { 40, 0x03, 155 },
        { 2, 0x02, 157 },
        { 9, 0x02, 157 },
        { 23, 0x02, 157 },
        { 40, 0x03, 157 },
        { 2, 0x02, 158 },
        { 9, 0x02, 158 },
        { 23, 0x02, 158 },
        { 40, 0x03, 158 },
    },
    /* 160 */
    {
        { 3, 0x02, 152 },
        { 6, 0x02, 152 },
        { 10, 0x02, 152 },
        { 15, 0x02, 152 },
        { 24, 0x02, 152 },
        { 31, 0x02, 152 },
        { 41, 0x02, 152 },
        { 56, 0x03, 152 },
        { 3, 0x02, 155 },
        { 6, 0x02, 155 },
        { 10, 0x02, 155 },
        { 15, 0x02, 155 },
        { 24, 0x02, 155 },
        { 31, 0x02, 155 },
        { 41, 0x02, 155 },
        { 56, 0x03, 155 },
    },
    /* 161 */
    {
        { 3, 0x02, 157 },
        { 6, 0x02, 157 },
        { 10, 0x02, 157 },
        { 15, 0x02, 157 },
        { 24, 0x02, 157 },
        { 31, 0x02, 157 },
        { 41, 0x02, 157 },
        { 56, 0x03, 157 },
        { 3, 0x02, 158 },
        { 6, 0x02, 158 },
        { 10, 0x02, 158 },
        { 15, 0x02, 158 },
        { 24, 0x02, 158 },
        { 31, 0x02, 158 },
        { 41, 0x02, 158 },
        { 56, 0x03, 158 },
    },
    /* 162 */
    {
        { 1, 0x02, 165 },
        { 22, 0x03, 165 },
        { 1, 0x02, 166 },
        { 22, 0x03, 166 },
        { 1, 0x02, 168 },
        { 22, 0x03, 168 },
        { 1, 0x02, 174 },
        { 22, 0x03, 174 },
        { 1, 0x02, 175 },
        { 22, 0x03, 175 },
        { 1, 0x02, 180 },
        { 22, 0x03, 180 },
        { 1, 0x02, 182 },
        { 22, 0x03, 182 },
        { 1, 0x02, 183 },
        { 22, 0x03, 183 },
    },
    /* 163 */
    {
        { 2, 0x02, 165 },
        { 9, 0x02, 165 },
        { 23, 0x02, 165 },
        { 40, 0x03, 165 },
        { 2, 0x02, 166 },
        { 9, 0x02, 166 },
        { 23, 0x02, 166 },
        { 40, 0x03, 166 },
        { 2, 0x02, 168 },
        { 9, 0x02, 168 },
        { 23, 0x02, 168 },
        { 40, 0x03, 168 },
        { 2, 0x02, 174 },
        { 9, 0x02, 174 },
        { 23, 0x02, 174 },
        { 40, 0x03, 174 },
    },
    /* 164 */
    {
        { 3, 0x02, 165 },
        { 6, 0x02, 165 },
        { 10, 0x02, 165 },
        { 15, 0x02, 165 },
        { 24, 0x02, 165 },
        { 31, 0x02, 165 },
        { 41, 0x02, 165 },
        { 56, 0x03, 165 },
        { 3, 0x02, 166 },
        { 6, 0x02, 166 },
        { 10, 0x02, 166 },
        { 15, 0x02, 166 },
        { 24, 0x02, 166 },
        { 31, 0x02, 166 },
        { 41, 0x02, 166 },
        { 56, 0x03, 166 },
    },
    /* 165 */
    {
        { 3, 0x02, 168 },
        { 6, 0x02, 168 },
        { 10, 0x02, 168 },
        { 15, 0x02, 168 },
        { 24, 0x02, 168 },
        { 31, 0x02, 168 },
        { 41, 0x02, 168 },
        { 56, 0x03, 168 },
        { 3, 0x02, 174 },
        { 6, 0x02, 174 },
        { 10, 0x02, 174 },
        { 15, 0x02, 174 },
        { 24, 0x02, 174 },
        { 31, 0x02, 174 },
        { 41, 0x02, 174 },
        { 56, 0x03, 174 },
    },
    /* 166 */
    {
        { 2, 0x02, 175 },
        { 9, 0x02, 175 },
        { 23, 0x02, 175 },
        { 40, 0x03, 175 },
        { 2, 0x02, 180 },
        { 9, 0x02, 180 },
        { 23, 0x02, 180 },
        { 40, 0x03, 180 },
        { 2, 0x02, 182 },
        { 9, 0x02, 182 },
        { 23, 0x02, 182 },
        { 40, 0x03, 182 },
        { 2, 0x02, 183 },
        { 9, 0x02, 183 },
        { 23, 0x02, 183 },
        { 40, 0x03, 183 },
    },
    /* 167 */
    {
        { 3, 0x02, 175 },
        { 6, 0x02, 175 },
        { 10, 0x02, 175 },
        { 15, 0x02, 175 },
        { 24, 0x02, 175 },
        { 31, 0x02, 175 },
        { 41, 0x02, 175 },
        { 56, 0x03, 175 },
        { 3, 0x02, 180 },
        { 6, 0x02, 180 },
        { 10, 0x02, 180 },
        { 15, 0x02, 180 },
        { 24, 0x02, 180 },
        { 31, 0x02, 180 },
        { 41, 0x02, 180 },
        { 56, 0x03, 180 },
    },
    /* 168 */
    {
        { 3, 0x02, 182 },
        { 6, 0x02, 182 },
        { 10, 0x02, 182 },
        { 15, 0x02, 182 },
        { 24, 0x02, 182 },
        { 31, 0x02, 182 },
        { 41, 0x02, 182 },
        { 56, 0x03, 182 },
        { 3, 0x02, 183 },
        { 6, 0x02, 183 },
        { 10, 0x02, 183 },
        { 15, 0x02, 183 },
        { 24, 0x02, 183 },
        { 31, 0x02, 183 },
        { 41, 0x02, 183 },
        { 56, 0x03, 183 },
    },
    /* 169 */
    {
        { 0, 0x03, 188 },
        { 0, 0x03, 191 },
        { 0, 0x03, 197 },
        { 0, 0x03, 231 },
        { 0, 0x03, 239 },
        { 176, 0x00, 0 },
        { 178, 0x00, 0 },
        { 179, 0x00, 0 },
        { 183, 0x00, 0 },
        { 184, 0x00, 0 },
        { 186, 0x00, 0 },
        { 187, 0x00, 0 },
        { 192, 0x00, 0 },
        { 199, 0x00, 0 },
        { 208, 0x00, 0 },
        { 223, 0x00, 0 },
    },
    /* 170 */
    {
        { 1, 0x02, 188 },
        { 22, 0x03, 188 },
        { 1, 0x02, 191 },
        { 22, 0x03, 191 },
        { 1, 0x02, 197 },
        { 22, 0x03, 197 },
        { 1, 0x02, 231 },
        { 22, 0x03, 231 },
        { 1, 0x02, 239 },
        { 22, 0x03, 239 },
        { 0, 0x03, 9 },
        { 0, 0x03, 142 },
        { 0, 0x03, 144 },
        { 0, 0x03, 145 },
        { 0, 0x03, 148 },
        { 0, 0x03, 159 },
    },
    /* 171 */
    {
        { 2, 0x02, 188 },
        { 9, 0x02, 188 },
        { 23, 0x02, 188 },
        { 40, 0x03, 188 },
        { 2, 0x02, 191 },
        { 9, 0x02, 191 },
        { 23, 0x02, 191 },
        { 40, 0x03, 191 },
        { 2, 0x02, 197 },
        { 9, 0x02, 197 },
        { 23, 0x02, 197 },
        { 40, 0x03, 197 },
        { 2, 0x02, 231 },
        { 9, 0x02, 231 },
        { 23, 0x02, 231 },
        { 40, 0x03, 231 },
    },
    /* 172 */
    {
        { 3, 0x02, 188 },
        { 6, 0x02, 188 },
        { 10, 0x02, 188 },
        { 15, 0x02, 188 },
        { 24, 0x02, 188 },
        { 31, 0x02, 188 },
        { 41, 0x02, 188 },
        { 56, 0x03, 188 },
        { 3, 0x02, 191 },
        { 6, 0x02, 191 },
        { 10, 0x02, 191 },
        { 15, 0x02, 191 },
        { 24, 0x02, 191 },
        { 31, 0x02, 191 },
        { 41, 0x02, 191 },
        { 56, 0x03, 191 },
    },
    /* 173 */
    {
        { 3, 0x02, 197 },
        { 6, 0x02, 197 },
        { 10, 0x02, 197 },
        { 15, 0x02, 197 },
        { 24, 0x02, 197 },
        { 31, 0x02, 197 },
        { 41, 0x02, 197 },
        { 56, 0x03, 197 },
        { 3, 0x02, 231 },
        { 6, 0x02, 231 },
        { 10, 0x02, 231 },
        { 15, 0x02, 231 },
        { 24, 0x02, 231 },
        { 31, 0x02, 231 },
        { 41, 0x02, 231 },
        { 56, 0x03, 231 },
    },
    /* 174 */
    {
        { 2, 0x02, 239 },
        { 9, 0x02, 239 },
        { 23, 0x02, 239 },
        { 40, 0x03, 239 },
        { 1, 0x02, 9 },
        { 22, 0x03, 9 },
        { 1, 0x02, 142 },
        { 22, 0x03, 142 },
        { 1, 0x02, 144 },
        { 22, 0x03, 144 },
        { 1, 0x02, 145 },
        { 22, 0x03, 145 },
        { 1, 0x02, 148 },
        { 22, 0x03, 148 },
        { 1, 0x02, 159 },
        { 22, 0x03, 159 },
    },
    /* 175 */
    {
        { 3, 0x02, 239 },
        { 6, 0x02, 239 },
        { 10, 0x02, 239 },
        { 15, 0x02, 239 },
        { 24, 0x02, 239 },
        { 31, 0x02, 239 },
        { 41, 0x02, 239 },
        { 56, 0x03, 239 },
        { 2, 0x02, 9 },
        { 9, 0x02, 9 },
        { 23, 0x02, 9 },
        { 40, 0x03, 9 },
        { 2, 0x02, 142 },
        { 9, 0x02, 142 },
        { 23, 0x02, 142 },
        { 40, 0x03, 142 },
    },
    /* 176 */
    {
        { 3, 0x02, 9 },
        { 6, 0x02, 9 },
        { 10, 0x02, 9 },
        { 15, 0x02, 9 },
        { 24, 0x02, 9 },
        { 31, 0x02, 9 },
        { 41, 0x02, 9 },
        { 56, 0x03, 9 },
        { 3, 0x02, 142 },
        { 6, 0x02, 142 },
        { 10, 0x02, 142 },
        { 15, 0x02, 142 },
        { 24, 0x02, 142 },
        { 31, 0x02, 142 },
        { 41, 0x02, 142 },
        { 56, 0x03, 142 },
    },
    /* 177 */
    {
        { 2, 0x02, 144 },
        { 9, 0x02, 144 },
        { 23, 0x02, 144 },
        { 40, 0x03, 144 },
        { 2, 0x02, 145 },
        { 9, 0x02, 145 },
        { 23, 0x02, 145 },
        { 40, 0x03, 145 },
        { 2, 0x02, 148 },
        { 9, 0x02, 148 },
        { 23, 0x02, 148 },
        { 40, 0x03, 148 },
        { 2, 0x02, 159 },
        { 9, 0x02, 159 },
        { 23, 0x02, 159 },
        { 40, 0x03, 159 },
    },
    /* 178 */
    {
        { 3, 0x02, 144 },
        { 6, 0x02, 144 },
        { 10, 0x02, 144 },
        { 15, 0x02, 144 },
        { 24, 0x02, 144 },
        { 31, 0x02, 144 },
        { 41, 0x02, 144 },
        { 56, 0x03, 144 },
        { 3, 0x02, 145 },
        { 6, 0x02, 145 },
        { 10, 0x02, 145 },
        { 15, 0x02, 145 },
        { 24, 0x02, 145 },
        { 31, 0x02, 145 },
        { 41, 0x02, 145 },
        { 56, 0x03, 145 },
    },
    /* 179 */
    {
        { 3, 0x02, 148 },
        { 6, 0x02, 148 },
        { 10, 0x02, 148 },
        { 15, 0x02, 148 },
        { 24, 0x02, 148 },
        { 31, 0x02, 148 },
        { 41, 0x02, 148 },
        { 56, 0x03, 148 },
        { 3, 0x02, 159 },
        { 6, 0x02, 159 },
        { 10, 0x02, 159 },
        { 15, 0x02, 159 },
        { 24, 0x02, 159 },
        { 31, 0x02, 159 },
        { 41, 0x02, 159 },
        { 56, 0x03, 159 },
    },
    /* 180 */
    {
        { 0, 0x03, 171 },
        { 0, 0x03, 206 },
        { 0, 0x03, 215 },
        { 0, 0x03, 225 },
        { 0, 0x03, 236 },
        { 0, 0x03, 237 },
        { 188, 0x00, 0 },
        { 189, 0x00, 0 },
        { 193, 0x00, 0 },
        { 196, 0x00, 0 },
        { 200, 0x00, 0 },
        { 203, 0x00, 0 },
        { 209, 0x00, 0 },
        { 216, 0x00, 0 },
        { 224, 0x00, 0 },
        { 238, 0x00, 0 },
    },
    /* 181 */
    {
        { 1, 0x02, 171 },
        { 22, 0x03, 171 },
        { 1, 0x02, 206 },
        { 22, 0x03, 206 },
        { 1, 0x02, 215 },
        { 22, 0x03, 215 },
        { 1, 0x02, 225 },
        { 22, 0x03, 225 },
        { 1, 0x02, 236 },
        { 22, 0x03, 236 },
        { 1, 0x02, 237 },
        { 22, 0x03, 237 },
        { 0, 0x03, 199 },
        { 0, 0x03, 207 },
        { 0, 0x03, 234 },
        { 0, 0x03, 235 },
    },
    /* 182 */
    {
        { 2, 0x02, 171 },
        { 9, 0x02, 171 },
        { 23, 0x02, 171 },
        { 40, 0x03, 171 },
        { 2, 0x02, 206 },
        { 9, 0x02, 206 },
        { 23, 0x02, 206 },
        { 40, 0x03, 206 },
        { 2, 0x02, 215 },
        { 9, 0x02, 215 },
        { 23, 0x02, 215 },
        { 40, 0x03, 215 },
        { 2, 0x02, 225 },
        { 9, 0x02, 225 },
        { 23, 0x02, 225 },
        { 40, 0x03, 225 },
    },
    /* 183 */
    {
        { 3, 0x02, 171 },
        { 6, 0x02, 171 },
        { 10, 0x02, 171 },
        { 15, 0x02, 171 },
        { 24, 0x02, 171 },
        { 31, 0x02, 171 },
        { 41, 0x02, 171 },
        { 56, 0x03, 171 },
        { 3, 0x02, 206 },
        { 6, 0x02, 206 },
        { 10, 0x02, 206 },
        { 15, 0x02, 206 },
        { 24, 0x02, 206 },
        { 31, 0x02, 206 },
        { 41, 0x02, 206 },
        { 56, 0x03, 206 },
    },
    /* 184 */
    {
        { 3, 0x02, 215 },
        { 6, 0x02, 215 },
        { 10, 0x02, 215 },
        { 15, 0x02, 215 },
        { 24, 0x02, 215 },
        { 31, 0x02, 215 },
        { 41, 0x02, 215 },
        { 56, 0x03, 215 },
        { 3, 0x02, 225 },
        { 6, 0x02, 225 },
        { 10, 0x02, 225 },
        { 15, 0x02, 225 },
        { 24, 0x02, 225 },
        { 31, 0x02, 225 },
        { 41, 0x02, 225 },
        { 56, 0x03, 225 },
    },
    /* 185 */
    {
        { 2, 0x02, 236 },
        { 9, 0x02, 236 },
        { 23, 0x02, 236 },
        { 40, 0x03, 236 },
        { 2, 0x02, 237 },
        { 9, 0x02, 237 },
        { 23, 0x02, 237 },
        { 40, 0x03, 237 },
        { 1, 0x02, 199 },
        { 22, 0x03, 199 },
        { 1, 0x02, 207 },
        { 22, 0x03, 207 },
        { 1, 0x02, 234 },
        { 22, 0x03, 234 },
        { 1, 0x02, 235 },
        { 22, 0x03, 235 },
    },
    /* 186 */
    {
        { 3, 0x02, 236 },
        { 6, 0x02, 236 },
        { 10, 0x02, 236 },
        { 15, 0x02, 236 },
        { 24, 0x02, 236 },
        { 31, 0x02, 236 },
        { 41, 0x02, 236 },
        { 56, 0x03, 236 },
        { 3, 0x02, 237 },
        { 6, 0x02, 237 },
        { 10, 0x02, 237 },
        { 15, 0x02, 237 },
        { 24, 0x02, 237 },
        { 31, 0x02, 237 },
        { 41, 0x02, 237 },
        { 56, 0x03, 237 },
    },
    /* 187 */
    {
        { 2, 0x02, 199 },
        { 9, 0x02, 199 },
        { 23, 0x02, 199 },
        { 40, 0x03, 199 },
        { 2, 0x02, 207 },
        { 9, 0x02, 207 },
        { 23, 0x02, 207 },
        { 40, 0x03, 207 },
        { 2, 0x02, 234 },
        { 9, 0x02, 234 },
        { 23, 0x02, 234 },
        { 40, 0x03, 234 },
        { 2, 0x02, 235 },
        { 9, 0x02, 235 },
        { 23, 0x02, 235 },
        { 40, 0x03, 235 },
    },
    /* 188 */
    {
        { 3, 0x02, 199 },
        { 6, 0x02, 199 },
        { 10, 0x02, 199 },
        { 15, 0x02, 199 },
        { 24, 0x02, 199 },
        { 31, 0x02, 199 },
        { 41, 0x02, 199 },
        { 56, 0x03, 199 },
        { 3, 0x02, 207 },
        { 6, 0x02, 207 },
        { 10, 0x02, 207 },
        { 15, 0x02, 207 },
        { 24, 0x02, 207 },
        { 31, 0x02, 207 },
        { 41, 0x02, 207 },
        { 56, 0x03, 207 },
    },
    /* 189 */
    {
        { 3, 0x02, 234 },
        { 6, 0x02, 234 },
        { 10, 0x02, 234 },
        { 15, 0x02, 234 },
        { 24, 0x02, 234 },
        { 31, 0x02, 234 },
        { 41, 0x02, 234 },
        { 56, 0x03, 234 },
        { 3, 0x02, 235 },
        { 6, 0x02, 235 },
        { 10, 0x02, 235 },
        { 15, 0x02, 235 },
        { 24, 0x02, 235 },
        { 31, 0x02, 235 },
        { 41, 0x02, 235 },
        { 56, 0x03, 235 },
    },
    /* 190 */
    {
        { 194, 0x00, 0 },
        { 195, 0x00, 0 },
        { 197, 0x00, 0 },
        { 198, 0x00, 0 },
        { 201, 0x00, 0 },
        { 202, 0x00, 0 },
        { 204, 0x00, 0 },
        { 205, 0x00, 0 },
        { 210, 0x00, 0 },
        { 213, 0x00, 0 },
        { 217, 0x00, 0 },
        { 220, 0x00, 0 },
        { 225, 0x00, 0 },
        { 231, 0x00, 0 },
        { 239, 0x00, 0 },
        { 246, 0x00, 0 },
    },
    /* 191 */
    {
        { 0, 0x03, 192 },
        { 0, 0x03, 193 },
        { 0, 0x03, 200 },
        { 0, 0x03, 201 },
        { 0, 0x03, 202 },
        { 0, 0x03, 205 },
        { 0, 0x03, 210 },
        { 0, 0x03, 213 },
        { 0, 0x03, 218 },
        { 0, 0x03, 219 },
        { 0, 0x03, 238 },
        { 0, 0x03, 240 },
        { 0, 0x03, 242 },
        { 0, 0x03, 243 },
        { 0, 0x03, 255 },
        { 206, 0x00, 0 },
    },
    /* 192 */
    {
        { 1, 0x02, 192 },
        { 22, 0x03, 192 },
        { 1, 0x02, 193 },
        { 22, 0x03, 193 },
        { 1, 0x02, 200 },
        { 22, 0x03, 200 },
        { 1, 0x02, 201 },
        { 22, 0x03, 201 },
        { 1, 0x02, 202 },
        { 22, 0x03, 202 },
        { 1, 0x02, 205 },
        { 22, 0x03, 205 },
        { 1, 0x02, 210 },
        { 22, 0x03, 210 },
        { 1, 0x02, 213 },
        { 22, 0x03, 213 },
    },
    /* 193 */
    {
        { 2, 0x02, 192 },
        { 9, 0x02, 192 },
        { 23, 0x02, 192 },
        { 40, 0x03, 192 },
        { 2, 0x02, 193 },
        { 9, 0x02, 193 },
        { 23, 0x02, 193 },
        { 40, 0x03, 193 },
        { 2, 0x02, 200 },
        { 9, 0x02, 200 },
        { 23, 0x02, 200 },
        { 40, 0x03, 200 },
        { 2, 0x02, 201 },
        { 9, 0x02, 201 },
        { 23, 0x02, 201 },
        { 40, 0x03, 201 },
    },
    /* 194 */
    {
        { 3, 0x02, 192 },
        { 6, 0x02, 192 },
        { 10, 0x02, 192 },
        { 15, 0x02, 192 },
        { 24, 0x02, 192 },
        { 31, 0x02, 192 },
        { 41, 0x02, 192 },
        { 56, 0x03, 192 },
        { 3, 0x02, 193 },
        { 6, 0x02, 193 },
        { 10, 0x02, 193 },
        { 15, 0x02, 193 },
        { 24, 0x02, 193 },
        { 31, 0x02, 193 },
        { 41, 0x02, 193 },
        { 56, 0x03, 193 },
    },
    /* 195 */
    {
        { 3, 0x02, 200 },
        { 6, 0x02, 200 },
        { 10, 0x02, 200 },
        { 15, 0x02, 200 },
        { 24, 0x02, 200 },
        { 31, 0x02, 200 },
        { 41, 0x02, 200 },
        { 56, 0x03, 200 },
        { 3, 0x02, 201 },
        { 6, 0x02, 201 },
        { 10, 0x02, 201 },
        { 15, 0x02, 201 },
        { 24, 0x02, 201 },
        { 31, 0x02, 201 },
        { 41, 0x02, 201 },
        { 56, 0x03, 201 },
    },
    /* 196 */
    {
        { 2, 0x02, 202 },
        { 9, 0x02, 202 },
        { 23, 0x02, 202 },
        { 40, 0x03, 202 },
        { 2, 0x02, 205 },
        { 9, 0x02, 205 },
        { 23, 0x02, 205 },
        { 40, 0x03, 205 },
        { 2, 0x02, 210 },
        { 9, 0x02, 210 },
        { 23, 0x02, 210 },
        { 40, 0x03, 210 },
        { 2, 0x02, 213 },
        { 9, 0x02, 213 },
        { 23, 0x02, 213 },
        { 40, 0x03, 213 },
    },
    /* 197 */
    {
        { 3, 0x02, 202 },
        { 6, 0x02, 202 },
        { 10, 0x02, 202 },
        { 15, 0x02, 202 },
        { 24, 0x02, 202 },
        { 31, 0x02, 202 },
        { 41, 0x02, 202 },
        { 56, 0x03, 202 },
        { 3, 0x02, 205 },
        { 6, 0x02, 205 },
        { 10, 0x02, 205 },
        { 15, 0x02, 205 },
        { 24, 0x02, 205 },
        { 31, 0x02, 205 },
        { 41, 0x02, 205 },
        { 56, 0x03, 205 },
    },
    /* 198 */
    {
        { 3, 0x02, 210 },
        { 6, 0x02, 210 },
        { 10, 0x02, 210 },
        { 15, 0x02, 210 },
        { 24, 0x02, 210 },
        { 31, 0x02, 210 },
        { 41, 0x02, 210 },
        { 56, 0x03, 210 },
        { 3, 0x02, 213 },
        { 6, 0x02, 213 },
        { 10, 0x02, 213 },
        { 15, 0x02, 213 },
        { 24, 0x02, 213 },
        { 31, 0x02, 213 },
        { 41, 0x02, 213 },
        { 56, 0x03, 213 },
    },
    /* 199 */
    {
        { 1, 0x02, 218 },
        { 22, 0x03, 218 },
        { 1, 0x02, 219 },
        { 22, 0x03, 219 },
        { 1, 0x02, 238 },
        { 22, 0x03, 238 },
        { 1, 0x02, 240 },
        { 22, 0x03, 240 },
        { 1, 0x02, 242 },
        { 22, 0x03, 242 },
        { 1, 0x02, 243 },
        { 22, 0x03, 243 },
        { 1, 0x02, 255 },
        { 22, 0x03, 255 },
        { 0, 0x03, 203 },
        { 0, 0x03, 204 },
    },
    /* 200 */
    {
        { 2, 0x02, 218 },
        { 9, 0x02, 218 },
        { 23, 0x02, 218 },
        { 40, 0x03, 218 },
        { 2, 0x02, 219 },
        { 9, 0x02, 219 },
        { 23, 0x02, 219 },
        { 40, 0x03, 219 },
        { 2, 0x02, 238 },
        { 9, 0x02, 238 },
        { 23, 0x02, 238 },
        { 40, 0x03, 238 },
        { 2, 0x02, 240 },
        { 9, 0x02, 240 },
        { 23, 0x02, 240 },
        { 40, 0x03, 240 },
    },
    /* 201 */
    {
        { 3, 0x02, 218 },
        { 6, 0x02, 218 },
        { 10, 0x02, 218 },
        { 15, 0x02, 218 },
        { 24, 0x02, 218 },
        { 31, 0x02, 218 },
        { 41, 0x02, 218 },
        { 56, 0x03, 218 },
        { 3, 0x02, 219 },
        { 6, 0x02, 219 },
        { 10, 0x02, 219 },
        { 15, 0x02, 219 },
        { 24, 0x02, 219 },
        { 31, 0x02, 219 },
        { 41, 0x02, 219 },
        { 56, 0x03, 219 },
    },
    /* 202 */
    {
        { 3, 0x02, 238 },
        { 6, 0x02, 238 },
        { 10, 0x02, 238 },
        { 15, 0x02, 238 },
        { 24, 0x02, 238 },
        { 31, 0x02, 238 },
        { 41, 0x02, 238 },
        { 56, 0x03, 238 },
        { 3, 0x02, 240 },
        { 6, 0x02, 240 },
        { 10, 0x02, 240 },
        { 15, 0x02, 240 },
        { 24, 0x02, 240 },
        { 31, 0x02, 240 },
        { 41, 0x02, 240 },
        { 56, 0x03, 240 },
    },
    /* 203 */
    {
        { 2, 0x02, 242 },
        { 9, 0x02, 242 },
        { 23, 0x02, 242 },
        { 40, 0x03, 242 },
        { 2, 0x02, 243 },
        { 9, 0x02, 243 },
        { 23, 0x02, 243 },
        { 40, 0x03, 243 },
        { 2, 0x02, 255 },
        { 9, 0x02, 255 },
        { 23, 0x02, 255 },
        { 40, 0x03, 255 },
        { 1, 0x02, 203 },
        { 22, 0x03, 203 },
        { 1, 0x02, 204 },
        { 22, 0x03, 204 },
    },
    /* 204 */
    {
        { 3, 0x02, 242 },
        { 6, 0x02, 242 },
        { 10, 0x02, 242 },
        { 15, 0x02, 242 },
        { 24, 0x02, 242 },
        { 31, 0x02, 242 },
        { 41, 0x02, 242 },
        { 56, 0x03, 242 },
        { 3, 0x02, 243 },
        { 6, 0x02, 243 },
        { 10, 0x02, 243 },
        { 15, 0x02, 243 },
        { 24, 0x02, 243 },
        { 31, 0x02, 243 },
        { 41, 0x02, 243 },
        { 56, 0x03, 243 },
    },
    /* 205 */
    {
        { 3, 0x02, 255 },
        { 6, 0x02, 255 },
        { 10, 0x02, 255 },
        { 15, 0x02, 255 },
        { 24, 0x02, 255 },
        { 31, 0x02, 255 },
        { 41, 0x02, 255 },
        { 56, 0x03, 255 },
        { 2, 0x02, 203 },
        { 9, 0x02, 203 },
        { 23, 0x02, 203 },
        { 40, 0x03, 203 },
        { 2, 0x02, 204 },
        { 9, 0x02, 204 },
        { 23, 0x02, 204 },
        { 40, 0x03, 204 },
    },
    /* 206 */
    {
        { 3, 0x02, 203 },
        { 6, 0x02, 203 },
        { 10, 0x02, 203 },
        { 15, 0x02, 203 },
        { 24, 0x02, 203 },
        { 31, 0x02, 203 },
        { 41, 0x02, 203 },
        { 56, 0x03, 203 },
        { 3, 0x02, 204 },
        { 6, 0x02, 204 },
        { 10, 0x02, 204 },
        { 15, 0x02, 204 },
        { 24, 0x02, 204 },
        { 31, 0x02, 204 },
        { 41, 0x02, 204 },
        { 56, 0x03, 204 },
    },
    /* 207 */
    {
        { 211, 0x00, 0 },
        { 212, 0x00, 0 },
        { 214, 0x00, 0 },
        { 215, 0x00, 0 },
        { 218, 0x00, 0 },
        { 219, 0x00, 0 },
        { 221, 0x00, 0 },
        { 222, 0x00, 0 },
        { 226, 0x00, 0 },
        { 228, 0x00, 0 },
        { 232, 0x00, 0 },
        { 235, 0x00, 0 },
        { 240, 0x00, 0 },
        { 243, 0x00, 0 },
        { 247, 0x00, 0 },
        { 250, 0x00, 0 },
    },
    /* 208 */
    {
        { 0, 0x03, 211 },
        { 0, 0x03, 212 },
        { 0, 0x03, 214 },
        { 0, 0x03, 221 },
        { 0, 0x03, 222 },
        { 0, 0x03, 223 },
        { 0, 0x03, 241 },
        { 0, 0x03, 244 },
        { 0, 0x03, 245 },
        { 0, 0x03, 246 },
        { 0, 0x03, 247 },
        { 0, 0x03, 248 },
        { 0, 0x03, 250 },
        { 0, 0x03, 251 },
        { 0, 0x03, 252 },
        { 0, 0x03, 253 },
    },
    /* 209 */
    {
        { 1, 0x02, 211 },
        { 22, 0x03, 211 },
        { 1, 0x02, 212 },
        { 22, 0x03, 212 },
        { 1, 0x02, 214 },
        { 22, 0x03, 214 },
        { 1, 0x02, 221 },
        { 22, 0x03, 221 },
        { 1, 0x02, 222 },
        { 22, 0x03, 222 },
        { 1, 0x02, 223 },
        { 22, 0x03, 223 },
        { 1, 0x02, 241 },
        { 22, 0x03, 241 },
        { 1, 0x02, 244 },
        { 22, 0x03, 244 },
    },
    /* 210 */
    {
        { 2, 0x02, 211 },
        { 9, 0x02, 211 },
        { 23, 0x02, 211 },
        { 40, 0x03, 211 },
        { 2, 0x02, 212 },
        { 9, 0x02, 212 },
        { 23, 0x02, 212 },
        { 40, 0x03, 212 },
        { 2, 0x02, 214 },
        { 9, 0x02, 214 },
        { 23, 0x02, 214 },
        { 40, 0x03, 214 },
        { 2, 0x02, 221 },
        { 9, 0x02, 221 },
        { 23, 0x02, 221 },
        { 40, 0x03, 221 },
    },
    /* 211 */
    {
        { 3, 0x02, 211 },
        { 6, 0x02, 211 },
        { 10, 0x02, 211 },
        { 15, 0x02, 211 },
        { 24, 0x02, 211 },
        { 31, 0x02, 211 },
        { 41, 0x02, 211 },
        { 56, 0x03, 211 },
        { 3, 0x02, 212 },
        { 6, 0x02, 212 },
        { 10, 0x02, 212 },
        { 15, 0x02, 212 },
        { 24, 0x02, 212 },
        { 31, 0x02, 212 },
        { 41, 0x02, 212 },
        { 56, 0x03, 212 },
    },
    /* 212 */
    {
        { 3, 0x02, 214 },
        { 6, 0x02, 214 },
        { 10, 0x02, 214 },
        { 15, 0x02, 214 },
        { 24, 0x02, 214 },
        { 31, 0x02, 214 },
        { 41, 0x02, 214 },
        { 56, 0x03, 214 },
        { 3, 0x02, 221 },
        { 6, 0x02, 221 },
        { 10, 0x02, 221 },
        { 15, 0x02, 221 },
        { 24, 0x02, 221 },
        { 31, 0x02, 221 },
        { 41, 0x02, 221 },
        { 56, 0x03, 221 },
    },
    /* 213 */
    {
        { 2, 0x02, 222 },
        { 9, 0x02, 222 },
        { 23, 0x02, 222 },
        { 40, 0x03, 222 },
        { 2, 0x02, 223 },
        { 9, 0x02, 223 },
        { 23, 0x02, 223 },
        { 40, 0x03, 223 },
        { 2, 0x02, 241 },
        { 9, 0x02, 241 },
        { 23, 0x02, 241 },
        { 40, 0x03, 241 },
        { 2, 0x02, 244 },
        { 9, 0x02, 244 },
        { 23, 0x02, 244 },
        { 40, 0x03, 244 },
    },
    /* 214 */
    {
        { 3, 0x02, 222 },
        { 6, 0x02, 222 },
        { 10, 0x02, 222 },
        { 15, 0x02, 222 },
        { 24, 0x02, 222 },
        { 31, 0x02, 222 },
        { 41, 0x02, 222 },
        { 56, 0x03, 222 },
        { 3, 0x02, 223 },
        { 6, 0x02, 223 },
        { 10, 0x02, 223 },
        { 15, 0x02, 223 },
        { 24, 0x02, 223 },
        { 31, 0x02, 223 },
        { 41, 0x02, 223 },
        { 56, 0x03, 223 },
    },
    /* 215 */
    {
        { 3, 0x02, 241 },
        { 6, 0x02, 241 },
        { 10, 0x02, 241 },
        { 15, 0x02, 241 },
        { 24, 0x02, 241 },
        { 31, 0x02, 241 },
        { 41, 0x02, 241 },
        { 56, 0x03, 241 },
        { 3, 0x02, 244 },
        { 6, 0x02, 244 },
        { 10, 0x02, 244 },
        { 15, 0x02, 244 },
        { 24, 0x02, 244 },
        { 31, 0x02, 244 },
        { 41, 0x02, 244 },
        { 56, 0x03, 244 },
    },
    /* 216 */
    {
        { 1, 0x02, 245 },
        { 22, 0x03, 245 },
        { 1, 0x02, 246 },
        { 22, 0x03, 246 },
        { 1, 0x02, 247 },
        { 22, 0x03, 247 },
        { 1, 0x02, 248 },
        { 22, 0x03, 248 },
        { 1, 0x02, 250 },
        { 22, 0x03, 250 },
        { 1, 0x02, 251 },
        { 22, 0x03, 251 },
        { 1, 0x02, 252 },
        { 22, 0x03, 252 },
        { 1, 0x02, 253 },
        { 22, 0x03, 253 },
    },
    /* 217 */
    {
        { 2, 0x02, 245 },
        { 9, 0x02, 245 },
        { 23, 0x02, 245 },
        { 40, 0x03, 245 },
        { 2, 0x02, 246 },
        { 9, 0x02, 246 },
        { 23, 0x02, 246 },
        { 40, 0x03, 246 },
        { 2, 0x02, 247 },
        { 9, 0x02, 247 },
        { 23, 0x02, 247 },
        { 40, 0x03, 247 },
        { 2, 0x02, 248 },
        { 9, 0x02, 248 },
        { 23, 0x02, 248 },
        { 40, 0x03, 248 },
    },
    /* 218 */
    {
        { 3, 0x02, 245 },
        { 6, 0x02, 245 },
        { 10, 0x02, 245 },
        { 15, 0x02, 245 },
        { 24, 0x02, 245 },
        { 31, 0x02, 245 },
        { 41, 0x02, 245 },
        { 56, 0x03, 245 },
        { 3, 0x02, 246 },
        { 6, 0x02, 246 },
        { 10, 0x02, 246 },
        { 15, 0x02, 246 },
        { 24, 0x02, 246 },
        { 31, 0x02, 246 },
        { 41, 0x02, 246 },
        { 56, 0x03, 246 },
    },
    /* 219 */
    {
        { 3, 0x02, 247 },
        { 6, 0x02, 247 },
        { 10, 0x02, 247 },
        { 15, 0x02, 247 },
        { 24, 0x02, 247 },
        { 31, 0x02, 247 },
        { 41, 0x02, 247 },
        { 56, 0x03, 247 },
        { 3, 0x02, 248 },
        { 6, 0x02, 248 },
        { 10, 0x02, 248 },
        { 15, 0x02, 248 },
        { 24, 0x02, 248 },
        { 31, 0x02, 248 },
        { 41, 0x02, 248 },
        { 56, 0x03, 248 },
    },
    /* 220 */
    {
        { 2, 0x02, 250 },
        { 9, 0x02, 250 },
        { 23, 0x02, 250 },
        { 40, 0x03, 250 },
        { 2, 0x02, 251 },
        { 9, 0x02, 251 },
        { 23, 0x02, 251 },
        { 40, 0x03, 251 },
        { 2, 0x02, 252 },
        { 9, 0x02, 252 },
        { 23, 0x02, 252 },
        { 40, 0x03, 252 },
        { 2, 0x02, 253 },
        { 9, 0x02, 253 },
        { 23, 0x02, 253 },
        { 40, 0x03, 253 },
    },
    /* 221 */
    {
        { 3, 0x02, 250 },
        { 6, 0x02, 250 },
        { 10, 0x02, 250 },
        { 15, 0x02, 250 },
        { 24, 0x02, 250 },
        { 31, 0x02, 250 },
        { 41, 0x02, 250 },
        { 56, 0x03, 250 },
        { 3, 0x02, 251 },
        { 6, 0x02, 251 },
        { 10, 0x02, 251 },
        { 15, 0x02, 251 },
        { 24, 0x02, 251 },
        { 31, 0x02, 251 },
        { 41, 0x02, 251 },
        { 56, 0x03, 251 },
    },
    /* 222 */
    {
        { 3, 0x02, 252 },
        { 6, 0x02, 252 },
        { 10, 0x02, 252 },
        { 15, 0x02, 252 },
        { 24, 0x02, 252 },
        { 31, 0x02, 252 },
        { 41, 0x02, 252 },
        { 56, 0x03, 252 },
        { 3, 0x02, 253 },
        { 6, 0x02, 253 },
        { 10, 0x02, 253 },
        { 15, 0x02, 253 },
        { 24, 0x02, 253 },
        { 31, 0x02, 253 },
        { 41, 0x02, 253 },
        { 56, 0x03, 253 },
    },
    /* 223 */
    {
        { 0, 0x03, 254 },
        { 227, 0x00, 0 },
        { 229, 0x00, 0 },
        { 230, 0x00, 0 },
        { 233, 0x00, 0 },
        { 234, 0x00, 0 },
        { 236, 0x00, 0 },
        { 237, 0x00, 0 },
        { 241, 0x00, 0 },
        { 242, 0x00, 0 },
        { 244, 0x00, 0 },
        { 245, 0x00, 0 },
        { 248, 0x00, 0 },
        { 249, 0x00, 0 },
        { 251, 0x00, 0 },
        { 252, 0x00, 0 },
    },
    /* 224 */
    {
        { 1, 0x02, 254 },
        { 22, 0x03, 254 },
        { 0, 0x03, 2 },
        { 0, 0x03, 3 },
        { 0, 0x03, 4 },
        { 0, 0x03, 5 },
        { 0, 0x03, 6 },
        { 0, 0x03, 7 },
        { 0, 0x03, 8 },
        { 0, 0x03, 11 },
        { 0, 0x03, 12 },
        { 0, 0x03, 14 },
        { 0, 0x03, 15 },
        { 0, 0x03, 16 },
        { 0, 0x03, 17 },
        { 0, 0x03, 18 },
    },
    /* 225 */
    {
        { 2, 0x02, 254 },
        { 9, 0x02, 254 },
        { 23, 0x02, 254 },
        { 40, 0x03, 254 },
        { 1, 0x02, 2 },
        { 22, 0x03, 2 },
        { 1, 0x02, 3 },
        { 22, 0x03, 3 },
        { 1, 0x02, 4 },
        { 22, 0x03, 4 },
        { 1, 0x02, 5 },
        { 22, 0x03, 5 },
        { 1, 0x02, 6 },
        { 22, 0x03, 6 },
        { 1, 0x02, 7 },
        { 22, 0x03, 7 },
    },
    /* 226 */
    {
        { 3, 0x02, 254 },
        { 6, 0x02, 254 },
        { 10, 0x02, 254 },
        { 15, 0x02, 254 },
        { 24, 0x02, 254 },
        { 31, 0x02, 254 },
        { 41, 0x02, 254 },
        { 56, 0x03, 254 },
        { 2, 0x02, 2 },
        { 9, 0x02, 2 },
        { 23, 0x02, 2 },
        { 40, 0x03, 2 },
        { 2, 0x02, 3 },
        { 9, 0x02, 3 },
        { 23, 0x02, 3 },
        { 40, 0x03, 3 },
    },
    /* 227 */
    {
        { 3, 0x02, 2 },
        { 6, 0x02, 2 },
        { 10, 0x02, 2 },
        { 15, 0x02, 2 },
        { 24, 0x02, 2 },
        { 31, 0x02, 2 },
        { 41, 0x02, 2 },
        { 56, 0x03, 2 },
        { 3, 0x02, 3 },
        { 6, 0x02, 3 },
        { 10, 0x02, 3 },
        { 15, 0x02, 3 },
        { 24, 0x02, 3 },
        { 31, 0x02, 3 },
        { 41, 0x02, 3 },
        { 56, 0x03, 3 },
    },
    /* 228 */
    {
        { 2, 0x02, 4 },
        { 9, 0x02, 4 },
        { 23, 0x02, 4 },
        { 40, 0x03, 4 },
        { 2, 0x02, 5 },
        { 9, 0x02, 5 },
        { 23, 0x02, 5 },
        { 40, 0x03, 5 },
        { 2, 0x02, 6 },
        { 9, 0x02, 6 },
        { 23, 0x02, 6 },
        { 40, 0x03, 6 },
        { 2, 0x02, 7 },
        { 9, 0x02, 7 },
        { 23, 0x02, 7 },
        { 40, 0x03, 7 },
    },
    /* 229 */
    {
        { 3, 0x02, 4 },
        { 6, 0x02, 4 },
        { 10, 0x02, 4 },
        { 15, 0x02, 4 },
        { 24, 0x02, 4 },
        { 31, 0x02, 4 },
        { 41, 0x02, 4 },
        { 56, 0x03, 4 },
        { 3, 0x02, 5 },
        { 6, 0x02, 5 },
        { 10, 0x02, 5 },
        { 15, 0x02, 5 },
        { 24, 0x02, 5 },
        { 31, 0x02, 5 },
        { 41, 0x02, 5 },
        { 56, 0x03, 5 },
    },
    /* 230 */
    {
        { 3, 0x02, 6 },
        { 6, 0x02, 6 },
        { 10, 0x02, 6 },
        { 15, 0x02, 6 },
        { 24, 0x02, 6 },
        { 31, 0x02, 6 },
        { 41, 0x02, 6 },
        { 56, 0x03, 6 },
        { 3, 0x02, 7 },
        { 6, 0x02, 7 },
        { 10, 0x02, 7 },
        { 15, 0x02, 7 },
        { 24, 0x02, 7 },
        { 31, 0x02, 7 },
        { 41, 0x02, 7 },
        { 56, 0x03, 7 },
    },
    /* 231 */
    {
        { 1, 0x02, 8 },
        { 22, 0x03, 8 },
        { 1, 0x02, 11 },
        { 22, 0x03, 11 },
        { 1, 0x02, 12 },
        { 22, 0x03, 12 },
        { 1, 0x02, 14 },
        { 22, 0x03, 14 },
        { 1, 0x02, 15 },
        { 22, 0x03, 15 },
        { 1, 0x02, 16 },
        { 22, 0x03, 16 },
        { 1, 0x02, 17 },
        { 22, 0x03, 17 },
        { 1, 0x02, 18 },
        { 22, 0x03, 18 },
    },
    /* 232 */
    {
        { 2, 0x02, 8 },
        { 9, 0x02, 8 },
        { 23, 0x02, 8 },
        { 40, 0x03, 8 },
        { 2, 0x02, 11 },
        { 9, 0x02, 11 },
        { 23, 0x02, 11 },
        { 40, 0x03, 11 },
        { 2, 0x02, 12 },
        { 9, 0x02, 12 },
        { 23, 0x02, 12 },
        { 40, 0x03, 12 },
        { 2, 0x02, 14 },
        { 9, 0x02, 14 },
        { 23, 0x02, 14 },
        { 40, 0x03, 14 },
    },
    /* 233 */
    {
        { 3, 0x02, 8 },
        { 6, 0x02, 8 },
        { 10, 0x02, 8 },
        { 15, 0x02, 8 },
        { 24, 0x02, 8 },
        { 31, 0x02, 8 },
        { 41, 0x02, 8 },
        { 56, 0x03, 8 },
        { 3, 0x02, 11 },
        { 6, 0x02, 11 },
        { 10, 0x02, 11 },
        { 15, 0x02, 11 },
        { 24, 0x02, 11 },
        { 31, 0x02, 11 },
        { 41, 0x02, 11 },
        { 56, 0x03, 11 },
    },
    /* 234 */
    {
        { 3, 0x02, 12 },
        { 6, 0x02, 12 },
        { 10, 0x02, 12 },
        { 15, 0x02, 12 },
        { 24, 0x02, 12 },
        { 31, 0x02, 12 },
        { 41, 0x02, 12 },
        { 56, 0x03, 12 },
        { 3, 0x02, 14 },
        { 6, 0x02, 14 },
        { 10, 0x02, 14 },
        { 15, 0x02, 14 },
        { 24, 0x02, 14 },
        { 31, 0x02, 14 },
        { 41, 0x02, 14 },
        { 56, 0x03, 14 },
    },
    /* 235 */
    {
        { 2, 0x02, 15 },
        { 9, 0x02, 15 },
        { 23, 0x02, 15 },
        { 40, 0x03, 15 },
        { 2, 0x02, 16 },
        { 9, 0x02, 16 },
        { 23, 0x02, 16 },
        { 40, 0x03, 16 },
        { 2, 0x02, 17 },
        { 9, 0x02, 17 },
        { 23, 0x02, 17 },
        { 40, 0x03, 17 },
        { 2, 0x02, 18 },
        { 9, 0x02, 18 },
        { 23, 0x02, 18 },
        { 40, 0x03, 18 },
    },
    /* 236 */
    {
        { 3, 0x02, 15 },
        { 6, 0x02, 15 },
        { 10, 0x02, 15 },
        { 15, 0x02, 15 },
        { 24, 0x02, 15 },
        { 31, 0x02, 15 },
        { 41, 0x02, 15 },
        { 56, 0x03, 15 },
        { 3, 0x02, 16 },
        { 6, 0x02, 16 },
        { 10, 0x02, 16 },
        { 15, 0x02, 16 },
        { 24, 0x02, 16 },
        { 31, 0x02, 16 },
        { 41, 0x02, 16 },
        { 56, 0x03, 16 },
    },
    /* 237 */
    {
        { 3, 0x02, 17 },
        { 6, 0x02, 17 },
        { 10, 0x02, 17 },
        { 15, 0x02, 17 },
        { 24, 0x02, 17 },
        { 31, 0x02, 17 },
        { 41, 0x02, 17 },
        { 56, 0x03, 17 },
        { 3, 0x02, 18 },
        { 6, 0x02, 18 },
        { 10, 0x02, 18 },
        { 15, 0x02, 18 },
        { 24, 0x02, 18 },
        { 31, 0x02, 18 },
        { 41, 0x02, 18 },
        { 56, 0x03, 18 },
    },
    /* 238 */
    {
        { 0, 0x03, 19 },
        { 0, 0x03, 20 },
        { 0, 0x03, 21 },
        { 0, 0x03, 23 },
        { 0, 0x03, 24 },
        { 0, 0x03, 25 },
        { 0, 0x03, 26 },
        { 0, 0x03, 27 },
        { 0, 0x03, 28 },
        { 0, 0x03, 29 },
        { 0, 0x03, 30 },
        { 0, 0x03, 31 },
        { 0, 0x03, 127 },
        { 0, 0x03, 220 },
        { 0, 0x03, 249 },
        { 253, 0x00, 0 },
    },
    /* 239 */
    {
        { 1, 0x02, 19 },
        { 22, 0x03, 19 },
        { 1, 0x02, 20 },
        { 22, 0x03, 20 },
        { 1, 0x02, 21 },
        { 22, 0x03, 21 },
        { 1, 0x02, 23 },
        { 22, 0x03, 23 },
        { 1, 0x02, 24 },
        { 22, 0x03, 24 },
        { 1, 0x02, 25 },
        { 22, 0x03, 25 },
        { 1, 0x02, 26 },
        { 22, 0x03, 26 },
        { 1, 0x02, 27 },
        { 22, 0x03, 27 },
    },
    /* 240 */
    {
        { 2, 0x02, 19 },
        { 9, 0x02, 19 },
        { 23, 0x02, 19 },
        { 40, 0x03, 19 },
        { 2, 0x02, 20 },
        { 9, 0x02, 20 },
        { 23, 0x02, 20 },
        { 40, 0x03, 20 },
        { 2, 0x02, 21 },
        { 9, 0x02, 21 },
        { 23, 0x02, 21 },
        { 40, 0x03, 21 },
        { 2, 0x02, 23 },
        { 9, 0x02, 23 },
        { 23, 0x02, 23 },
        { 40, 0x03, 23 },
    },
    /* 241 */
    {
        { 3, 0x02, 19 },
        { 6, 0x02, 19 },
        { 10, 0x02, 19 },
        { 15, 0x02, 19 },
        { 24, 0x02, 19 },
        { 31, 0x02, 19 },
        { 41, 0x02, 19 },
        { 56, 0x03, 19 },
        { 3, 0x02, 20 },
        { 6, 0x02, 20 },
        { 10, 0x02, 20 },
        { 15, 0x02, 20 },
        { 24, 0x02, 20 },
        { 31, 0x02, 20 },
        { 41, 0x02, 20 },
        { 56, 0x03, 20 },
    },
    /* 242 */
    {
        { 3, 0x02, 21 },
        { 6, 0x02, 21 },
        { 10, 0x02, 21 },
        { 15, 0x02, 21 },
        { 24, 0x02, 21 },
        { 31, 0x02, 21 },
        { 41, 0x02, 21 },
        { 56, 0x03, 21 },
        { 3, 0x02, 23 },
        { 6, 0x02, 23 },
        { 10, 0x02, 23 },
        { 15, 0x02, 23 },
        { 24, 0x02, 23 },
        { 31, 0x02, 23 },
        { 41, 0x02, 23 },
        { 56, 0x03, 23 },
    },
    /* 243 */
    {
        { 2, 0x02, 24 },
        { 9, 0x02, 24 },
        { 23, 0x02, 24 },
        { 40, 0x03, 24 },
        { 2, 0x02, 25 },
        { 9, 0x02, 25 },
        { 23, 0x02, 25 },
        { 40, 0x03, 25 },
        { 2, 0x02, 26 },
        { 9, 0x02, 26 },
        { 23, 0x02, 26 },
        { 40, 0x03, 26 },
        { 2, 0x02, 27 },
        { 9, 0x02, 27 },
        { 23, 0x02, 27 },
        { 40, 0x03, 27 },
    },
    /* 244 */
    {
        { 3, 0x02, 24 },
        { 6, 0x02, 24 },
        { 10, 0x02, 24 },
        { 15, 0x02, 24 },
        { 24, 0x02, 24 },
        { 31, 0x02, 24 },
        { 41, 0x02, 24 },
        { 56, 0x03, 24 },
        { 3, 0x02, 25 },
        { 6, 0x02, 25 },
        { 10, 0x02, 25 },
        { 15, 0x02, 25 },
        { 24, 0x02, 25 },
        { 31, 0x02, 25 },
        { 41, 0x02, 25 },
        { 56, 0x03, 25 },
    },
    /* 245 */
    {
        { 3, 0x02, 26 },
        { 6, 0x02, 26 },
        { 10, 0x02, 26 },
        { 15, 0x02, 26 },
        { 24, 0x02, 26 },
        { 31, 0x02, 26 },
        { 41, 0x02, 26 },
        { 56, 0x03, 26 },
        { 3, 0x02, 27 },
        { 6, 0x02, 27 },
        { 10, 0x02, 27 },
        { 15, 0x02, 27 },
        { 24, 0x02, 27 },
        { 31, 0x02, 27 },
        { 41, 0x02, 27 },
        { 56, 0x03, 27 },
    },
    /* 246 */
    {
        { 1, 0x02, 28 },
        { 22, 0x03, 28 },
        { 1, 0x02, 29 },
        { 22, 0x03, 29 },
        { 1, 0x02, 30 },
        { 22, 0x03, 30 },
        { 1, 0x02, 31 },
        { 22, 0x03, 31 },
        { 1, 0x02, 127 },
        { 22, 0x03, 127 },
        { 1, 0x02, 220 },
        { 22, 0x03, 220 },
        { 1, 0x02, 249 },
        { 22, 0x03, 249 },
        { 254, 0x00, 0 },
        { 255, 0x00, 0 },
    },
    /* 247 */
    {
        { 2, 0x02, 28 },
        { 9, 0x02, 28 },
        { 23, 0x02, 28 },
        { 40, 0x03, 28 },
        { 2, 0x02, 29 },
        { 9, 0x02, 29 },
        { 23, 0x02, 29 },
        { 40, 0x03, 29 },
        { 2, 0x02, 30 },
        { 9, 0x02, 30 },
        { 23, 0x02, 30 },
        { 40, 0x03, 30 },
        { 2, 0x02, 31 },
        { 9, 0x02, 31 },
        { 23, 0x02, 31 },
        { 40, 0x03, 31 },
    },
    /* 248 */
    {
        { 3, 0x02, 28 },
        { 6, 0x02, 28 },
        { 10, 0x02, 28 },
        { 15, 0x02, 28 },
        { 24, 0x02, 28 },
        { 31, 0x02, 28 },
        { 41, 0x02, 28 },
        { 56, 0x03, 28 },
        { 3, 0x02, 29 },
        { 6, 0x02, 29 },
        { 10, 0x02, 29 },
        { 15, 0x02, 29 },
        { 24, 0x02, 29 },
        { 31, 0x02, 29 },
        { 41, 0x02, 29 },
        { 56, 0x03, 29 },
    },
    /* 249 */
    {
        { 3, 0x02, 30 },
        { 6, 0x02, 30 },
        { 10, 0x02, 30 },
        { 15, 0x02, 30 },
        { 24, 0x02, 30 },
        { 31, 0x02, 30 },
        { 41, 0x02, 30 },
        { 56, 0x03, 30 },
        { 3, 0x02, 31 },
        { 6, 0x02, 31 },
        { 10, 0x02, 31 },
        { 15, 0x02, 31 },
        { 24, 0x02, 31 },
        { 31, 0x02, 31 },
        { 41, 0x02, 31 },
        { 56, 0x03, 31 },
    },
    /* 250 */
    {
        { 2, 0x02, 127 },
        { 9, 0x02, 127 },
        { 23, 0x02, 127 },
        { 40, 0x03, 127 },
        { 2, 0x02, 220 },
        { 9, 0x02, 220 },
        { 23, 0x02, 220 },
        { 40, 0x03, 220 },
        { 2, 0x02, 249 },
        { 9, 0x02, 249 },
        { 23, 0x02, 249 },
        { 40, 0x03, 249 },
        { 0, 0x03, 10 },
        { 0, 0x03, 13 },
        { 0, 0x03, 22 },
        { 0, 0x04, 0 },
    },
    /* 251 */
    {
        { 3, 0x02, 127 },
        { 6, 0x02, 127 },
        { 10, 0x02, 127 },
        { 15, 0x02, 127 },
        { 24, 0x02, 127 },
        { 31, 0x02, 127 },
        { 41, 0x02, 127 },
        { 56, 0x03, 127 },
        { 3, 0x02, 220 },
        { 6, 0x02, 220 },
        { 10, 0x02, 220 },
        { 15, 0x02, 220 },
        { 24, 0x02, 220 },
        { 31, 0x02, 220 },
        { 41, 0x02, 220 },
        { 56, 0x03, 220 },
    },
    /* 252 */
    {
        { 3, 0x02, 249 },
        { 6, 0x02, 249 },
        { 10, 0x02, 249 },
        { 15, 0x02, 249 },
        { 24, 0x02, 249 },
        { 31, 0x02, 249 },
        { 41, 0x02, 249 },
        { 56, 0x03, 249 },
        { 1, 0x02, 10 },
        { 22, 0x03, 10 },
        { 1, 0x02, 13 },
        { 22, 0x03, 13 },
        { 1, 0x02, 22 },
        { 22, 0x03, 22 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
    },
    /* 253 */
    {
        { 2, 0x02, 10 },
        { 9, 0x02, 10 },
        { 23, 0x02, 10 },
        { 40, 0x03, 10 },
        { 2, 0x02, 13 },
        { 9, 0x02, 13 },
        { 23, 0x02, 13 },
        { 40, 0x03, 13 },
        { 2, 0x02, 22 },
        { 9, 0x02, 22 },
        { 23, 0x02, 22 },
        { 40, 0x03, 22 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
    },
    /* 254 */
    {
        { 3, 0x02, 10 },
        { 6, 0x02, 10 },
        { 10, 0x02, 10 },
        { 15, 0x02, 10 },
        { 24, 0x02, 10 },
        { 31, 0x02, 10 },
        { 41, 0x02, 10 },
        { 56, 0x03, 10 },
        { 3, 0x02, 13 },
        { 6, 0x02, 13 },
        { 10, 0x02, 13 },
        { 15, 0x02, 13 },
        { 24, 0x02, 13 },
        { 31, 0x02, 13 },
        { 41, 0x02, 13 },
        { 56, 0x03, 13 },
    },
    /* 255 */
    {
        { 3, 0x02, 22 },
        { 6, 0x02, 22 },
        { 10, 0x02, 22 },
        { 15, 0x02, 22 },
        { 24, 0x02, 22 },
        { 31, 0x02, 22 },
        { 41, 0x02, 22 },
        { 56, 0x03, 22 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
        { 0, 0x04, 0 },
    },
};
