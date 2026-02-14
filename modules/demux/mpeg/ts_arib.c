/*****************************************************************************
 * ts_arib.c : TS demux ARIB specific handling
 *****************************************************************************
 * Copyright (C) 2017 - VideoLAN Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_demux.h>

#include "timestamps.h"
#include "ts_pid.h"
#include "ts.h"

#include "ts_arib.h"

/*
 * ARIB TR-B21
 * Logo PNG is 8 bit indexed dans the palette is missing,
 * provided as a CLUT. We need to reinject the CLUT as a
 * split palette + transparency (as PNG only allows split alpha table)
*/

/* ARIB TR-B14-1 Appendix 1 */
static const unsigned char CLUT_to_chunks[] = {
    /* size + PLTE */
    0x00, 0x00, 0x01, 0x80, 0x50, 0x4c, 0x54, 0x45,
    /* DATA ARIB TR-B14-1 Appendix 1 */
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, /* 0-7 */
    0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,

    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, /* 8-15 */
    0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,

    0x00, 0x00, 0x55, 0x00, 0x55, 0x00, 0x00, 0x55, 0x55, 0x00, 0x55, 0xaa, /* 16-23 */
    0x00, 0x55, 0xff, 0x00, 0xaa, 0x55, 0x00, 0xaa, 0xff, 0x00, 0xff, 0x55,

    0x00, 0xff, 0xaa, 0x55, 0x00, 0x00, 0x55, 0x00, 0x55, 0x55, 0x00, 0xaa, /* 24-31 */
    0x55, 0x00, 0xff, 0x55, 0x55, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0xaa,

    0x55, 0x55, 0xff, 0x55, 0xaa, 0x00, 0x55, 0xaa, 0x00, 0x55, 0xaa, 0x55, /* 32-39 */
    0x55, 0xaa, 0xaa, 0x55, 0xaa, 0xff, 0x55, 0xff, 0x00, 0x55, 0xff, 0x55,

    0x55, 0xff, 0xff, 0xaa, 0x00, 0x55, 0xaa, 0x00, 0xff, 0xaa, 0x55, 0x00, /* 40-47 */
    0xaa, 0x55, 0x55, 0xaa, 0x55, 0xaa, 0xaa, 0x55, 0xff, 0xaa, 0xaa, 0x55,

    0xaa, 0xaa, 0xff, 0xaa, 0xff, 0x00, 0xaa, 0xff, 0x55, 0xaa, 0xff, 0xaa, /* 48-55 */
    0xaa, 0xff, 0xff, 0xff, 0x00, 0x55, 0xff, 0x00, 0xaa, 0xff, 0x55, 0x00,

    0xff, 0x55, 0x55, 0xff, 0x55, 0xaa, 0xff, 0x55, 0xff, 0xff, 0xaa, 0x00, /* 56-63 */
    0xff, 0xaa, 0x55, 0xff, 0xaa, 0xaa, 0xff, 0xaa, 0xff, 0xff, 0xff, 0x55,

    0xff, 0xff, 0xaa, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, /* 64-71 */
    0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff,

    0xff, 0xff, 0xff, 0xaa, 0x00, 0x00, 0x00, 0xaa, 0x00, 0xaa, 0xaa, 0x00, /* 72-79 */
    0x00, 0x00, 0xaa, 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,

    0x00, 0x00, 0x55, 0x00, 0x55, 0x00, 0x00, 0x55, 0x55, 0x00, 0x55, 0xaa, /* 80-87 */
    0x00, 0x55, 0xff, 0x00, 0xaa, 0x55, 0x00, 0xaa, 0xff, 0x00, 0xff, 0x55,

    0x00, 0xff, 0xaa, 0x55, 0x00, 0x00, 0x55, 0x00, 0x55, 0x55, 0x00, 0xaa, /* 88-95 */
    0x55, 0x00, 0xff, 0x55, 0x55, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0xaa,

    0x55, 0x55, 0xff, 0x55, 0xaa, 0x00, 0x55, 0xaa, 0x55, 0x55, 0xaa, 0xaa, /* 96-103 */
    0x55, 0xaa, 0xff, 0x55, 0xff, 0x00, 0x55, 0xff, 0x55, 0x55, 0xff, 0xaa,

    0x55, 0xff, 0xff, 0xaa, 0x00, 0x55, 0xaa, 0x00, 0xff, 0xaa, 0x55, 0x00, /* 104-111 */
    0xaa, 0x55, 0x55, 0xaa, 0x55, 0xaa, 0xaa, 0x55, 0xff, 0xaa, 0xaa, 0x55,

    0xaa, 0xaa, 0xff, 0xaa, 0xff, 0x00, 0xaa, 0xff, 0x55, 0xaa, 0xff, 0xaa, /* 112-119 */
    0xaa, 0xff, 0xff, 0xff, 0x00, 0x55, 0xff, 0x00, 0xaa, 0xff, 0x55, 0x00,

    0xff, 0x55, 0x55, 0xff, 0x55, 0xaa, 0xff, 0x55, 0xff, 0xff, 0xaa, 0x00, /* 120-127 */
    0xff, 0xaa, 0x55, 0xff, 0xaa, 0xaa, 0xff, 0xaa, 0xff, 0xff, 0xff, 0x55,

    /* CRC32 (all data including type, without length and crc) . pngcheck output being the lazy way */
    0x4f, 0xed, 0xfc, 0x8d,

    /* Second Chunk */

    /* size + tRNS */
    0x00, 0x00, 0x00, 0x80, 0x74, 0x52, 0x4e, 0x53,

    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 0-63 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,

    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, /* 64-127 */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80,

    /* CRC32 */
    0xfa, 0xe9, 0x51, 0x40,
};
static const unsigned int CLUT_to_chunks_len = sizeof(CLUT_to_chunks);

bool ts_arib_inject_png_palette( const uint8_t *p_in, size_t i_in, uint8_t **pp_out, size_t *pi_out )
{
    const uint8_t *p_data = p_in;
    const uint8_t *p_idat = NULL;
    size_t i_data = i_in - 8;
    p_data += 8;
    i_data -= 8;

    while( i_data > 11 )
    {
        uint32_t i_len = GetDWBE( p_data );
        if( i_len > 0x7FFFFFFFU || i_len > i_data - 12 )
            break;

        uint32_t i_chunk = VLC_FOURCC(p_data[4], p_data[5], p_data[6], p_data[7]);
        if( i_chunk == VLC_FOURCC('I', 'D', 'A', 'T') )
        {
            p_idat = p_data;
            break;
        }

        p_data += i_len + 12;
        i_data -= i_len + 12;
    }

    if( !p_idat )
        return false;

    {
        uint8_t *p_out = *pp_out = malloc( i_in + CLUT_to_chunks_len );
        if( !p_out )
            return false;
        *pi_out = i_in + CLUT_to_chunks_len;

        const size_t i_head = p_data - p_in;
        memcpy( p_out, p_in, i_head );
        memcpy( &p_out[i_head], CLUT_to_chunks, CLUT_to_chunks_len );
        memcpy( &p_out[i_head + CLUT_to_chunks_len], p_data, i_in - i_head );
    }

    return true;
}


void ts_arib_logo_dr_Delete( ts_arib_logo_dr_t *p_dr )
{
    free( p_dr->p_logo_char );
    free( p_dr );
}

ts_arib_logo_dr_t * ts_arib_logo_dr_Decode( const uint8_t *p_data, size_t i_data )
{
    if( i_data < 2 )
        return NULL;

    ts_arib_logo_dr_t *p_dr = calloc( 1, sizeof(*p_dr) );
    if( unlikely( p_dr == NULL ) )
        return NULL;

    p_dr->i_logo_version = p_data[0];
    switch( p_data[0] )
    {
        case 1:
            if( i_data != 7 )
                break;
            p_dr->i_logo_id = ((p_data[1] & 0x01) << 8) | p_data[2];
            p_dr->i_logo_version = ((p_data[3] & 0x0F) << 8) | p_data[4];
            p_dr->i_download_data_id = (p_data[5] << 8) | p_data[6];
            return p_dr;
        case 2:
            if( i_data != 3 )
                break;
            p_dr->i_logo_id = ((p_data[1] & 0x01) << 8) | p_data[2];
            return p_dr;
        case 3:
            if( i_data <= 2 )
                break;

            p_dr->p_logo_char = malloc( i_data - 1 );
            if( unlikely( p_dr->p_logo_char == NULL ) )
                break;

            p_dr->i_logo_char = i_data - 1;
            memcpy( p_dr->p_logo_char, &p_data[1], p_dr->i_logo_char );
            return p_dr;
        default:
            break;
    }

    ts_arib_logo_dr_Delete( p_dr );
    return NULL;
}

/*
 * SPDX-FileCopyrightText: 2015-2016 rndomhack
 * SPDX-License-Identifier: MIT
 * https://github.com/Chinachu/node-aribts
 */

#include "arib_tables.h"
#include <vlc_charset.h>
#include <vlc_memstream.h>

typedef struct
{
    uint8_t  graphic[4];      /* Character set designated to G0..G3 */
    uint8_t  graphic_mode[4]; /* 0: Graphic, 1: DRCS, 2: Other */
    uint8_t  graphic_byte[4]; /* Bytes per character (1 or 2) */
    uint8_t  graphic_l;       /* Set invoked into GL */
    uint8_t  graphic_r;       /* Set invoked into GR */
} arib_parser_t;

/* Character Set IDs (subset of charCode in node-aribts) */
#define CS_KANJI                0x42
#define CS_ASCII                0x4A
#define CS_HIRAGANA             0x30
#define CS_KATAKANA             0x31
#define CS_MOSAIC_A             0x32
#define CS_MOSAIC_B             0x33
#define CS_MOSAIC_C             0x34
#define CS_MOSAIC_D             0x35
#define CS_PROP_ASCII           0x36
#define CS_PROP_HIRAGANA        0x37
#define CS_PROP_KATAKANA        0x38
#define CS_JIS_KANJI_1          0x39
#define CS_JIS_KANJI_2          0x3A
#define CS_SYMBOL               0x3B
#define CS_JIS_X0201_KATAKANA   0x49

/* Mode */
#define MODE_GRAPHIC 0
#define MODE_DRCS    1
#define MODE_OTHER   2

static int cmp_u8_sjis( const void *key, const void *elem )
{
    const uint8_t k = *(const uint8_t *)key;
    const arib_map_sjis_t *e = (const arib_map_sjis_t *)elem;
    return (int)k - (int)e->key;
}

static int cmp_u16_str( const void *key, const void *elem )
{
    const uint16_t k = *(const uint16_t *)key;
    const arib_map_str_t *e = (const arib_map_str_t *)elem;
    return (int)k - (int)e->key;
}

static void arib_buf_append( uint8_t **pp_buf, size_t *pi_buf, size_t *pi_alloc,
                             uint8_t b1, uint8_t b2 )
{
    if( *pi_buf + 2 > *pi_alloc )
    {
        size_t i_new = *pi_alloc + 128;
        uint8_t *p_new = realloc( *pp_buf, i_new );
        if( !p_new ) return;
        *pp_buf = p_new;
        *pi_alloc = i_new;
    }
    (*pp_buf)[(*pi_buf)++] = b1;
    if( b2 ) (*pp_buf)[(*pi_buf)++] = b2;
}

static void arib_flush( struct vlc_memstream *p_stream, uint8_t **pp_buf, size_t *pi_buf )
{
    if( *pi_buf > 0 )
    {
        char *psz = FromCharset( "Shift_JIS", *pp_buf, *pi_buf );
        if( psz )
        {
            vlc_memstream_puts( p_stream, psz );
            free( psz );
        }
        *pi_buf = 0;
    }
}

static void get_sjis_kanji( uint8_t b1, uint8_t b2, uint8_t res[2] )
{
    uint8_t row = (b1 < 0x5F) ? 0x70 : 0xB0;
    uint8_t cell = (b1 & 1) ? ((b2 > 0x5F) ? 0x20 : 0x1F) : 0x7E;

    res[0] = (((b1 + 1) >> 1) + row) & 0xFF;
    res[1] = (b2 + cell) & 0xFF;
}

static void ts_arib_process_gl( arib_parser_t *p_ctx, uint8_t c,
                                const uint8_t *p_in, size_t i_in, size_t *pi,
                                struct vlc_memstream *p_stream,
                                uint8_t **pp_sjis, size_t *pi_sjis, size_t *pi_sjis_alloc,
                                bool b_graphic_normal )
{
    uint8_t set_idx = p_ctx->graphic_l;
    if( p_ctx->graphic_mode[set_idx] != MODE_GRAPHIC )
    {
        /* Invalid mode for GL, skip char(s) */
        if( p_ctx->graphic_byte[set_idx] == 2 && *pi < i_in ) (*pi)++;
    }
    else
    {
        uint8_t cs = p_ctx->graphic[set_idx];

        if( p_ctx->graphic_byte[set_idx] == 2 )
        {
             /* 2-byte GL */
             uint8_t c2 = (*pi < i_in) ? p_in[(*pi)++] : 0;
             if( !c2 ) return;

             /* Kanji / Gaiji Handling */
             uint16_t code = (c << 8) | c2;

             arib_map_str_t *p_map = bsearch( &code, arib_gaiji_map, arib_gaiji_map_count,
                                              sizeof(arib_map_str_t), cmp_u16_str );
             if( p_map )
             {
                 arib_flush( p_stream, pp_sjis, pi_sjis );
                 vlc_memstream_puts( p_stream, p_map->val );
             }
             else
             {
                 uint8_t sjis[2];
                 get_sjis_kanji( c, c2, sjis );
                 arib_buf_append( pp_sjis, pi_sjis, pi_sjis_alloc, sjis[0], sjis[1] );
             }
        }
        else
        {
            /* 1-byte GL */
            const arib_map_sjis_t *p_el = NULL;
            uint8_t val[2] = {0};

            if( cs == CS_ASCII || cs == CS_PROP_ASCII || cs == CS_JIS_X0201_KATAKANA )
            {
                if( b_graphic_normal )
                     p_el = bsearch( &c, arib_ascii_map, arib_ascii_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );
                else
                     val[0] = c;
            }
            else if( cs == CS_HIRAGANA || cs == CS_PROP_HIRAGANA )
            {
                p_el = bsearch( &c, arib_hiragana_map, arib_hiragana_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );
            }
            else if( cs == CS_KATAKANA || cs == CS_PROP_KATAKANA )
            {
                p_el = bsearch( &c, arib_katakana_map, arib_katakana_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );
            }

            if( p_el )
            {
                val[0] = p_el->val[0];
                val[1] = p_el->val[1];
            }
            else if( !val[0] && b_graphic_normal && cs == CS_ASCII )
            {
                val[0] = c;
            }

            arib_buf_append( pp_sjis, pi_sjis, pi_sjis_alloc, val[0], val[1] );
        }
    }
}

char * ts_arib_Decode( const uint8_t *p_in, size_t i_in )
{
    if( !p_in || !i_in ) return NULL;

    struct vlc_memstream stream;
    if( vlc_memstream_open( &stream ) )
        return NULL;

    arib_parser_t ctx;
    /* Default init similar to node-aribts */
    ctx.graphic[0] = CS_KANJI;
    ctx.graphic[1] = CS_ASCII;
    ctx.graphic[2] = CS_HIRAGANA;
    ctx.graphic[3] = CS_KATAKANA;
    ctx.graphic_mode[0] = MODE_GRAPHIC;
    ctx.graphic_mode[1] = MODE_GRAPHIC;
    ctx.graphic_mode[2] = MODE_GRAPHIC;
    ctx.graphic_mode[3] = MODE_GRAPHIC;
    ctx.graphic_byte[0] = 2;
    ctx.graphic_byte[1] = 1;
    ctx.graphic_byte[2] = 1;
    ctx.graphic_byte[3] = 1;
    ctx.graphic_l = 0;
    ctx.graphic_r = 2;
    bool b_graphic_normal = true; /* For handling 1-byte charsets vs MSZ/NSZ? node-aribts uses graphicNormal */

    uint8_t *p_sjis = NULL;
    size_t i_sjis = 0;
    size_t i_sjis_alloc = 0;

    size_t i = 0;
    while( i < i_in )
    {
        uint8_t c = p_in[i++];

        if( c <= 0x20 ) /* C0 */
        {
            switch( c )
            {
                case 0x0D: /* APR (CR) */
                    arib_buf_append( &p_sjis, &i_sjis, &i_sjis_alloc, 0x0D, 0x0A );
                    break;
                case 0x0E: /* LS1 */
                    ctx.graphic_l = 1;
                    break;
                case 0x0F: /* LS0 */
                    ctx.graphic_l = 0;
                    break;
                case 0x16: /* PAPF */
                    if( i < i_in ) i++;
                    break;
                case 0x19: /* SS2 */
                {
                    uint8_t saved_l = ctx.graphic_l;
                    ctx.graphic_l = 2;
                    if( i < i_in )
                    {
                        uint8_t next_c = p_in[i];
                        if ( next_c > 0x20 && next_c <= 0x7E )
                        {
                            i++;
                            ts_arib_process_gl( &ctx, next_c, p_in, i_in, &i, &stream,
                                                &p_sjis, &i_sjis, &i_sjis_alloc, b_graphic_normal );
                        }
                    }
                    ctx.graphic_l = saved_l;
                }
                    break;
                case 0x1C: /* APS */
                    if( i + 1 < i_in ) i += 2;
                    else i = i_in;
                    break;
                case 0x1D: /* SS3 */
                {
                    uint8_t saved_l = ctx.graphic_l;
                    ctx.graphic_l = 3;
                    if( i < i_in )
                    {
                        uint8_t next_c = p_in[i];
                        if ( next_c > 0x20 && next_c <= 0x7E )
                        {
                            i++;
                            ts_arib_process_gl( &ctx, next_c, p_in, i_in, &i, &stream,
                                                &p_sjis, &i_sjis, &i_sjis_alloc, b_graphic_normal );
                        }
                    }
                    ctx.graphic_l = saved_l;
                }
                    break;
                case 0x1B: /* ESC */
                    if( i < i_in )
                    {
                        uint8_t c2 = p_in[i++];
                        if( c2 == 0x24 ) /* Invoke MB */
                        {
                            if ( i < i_in )
                            {
                                uint8_t c3 = p_in[i++];
                                uint8_t set = 0;
                                if( c3 >= 0x28 && c3 <= 0x2B ) /* G0..G3 $ ( */
                                {
                                    set = c3 - 0x28;
                                    if( i < i_in ) c3 = p_in[i++];
                                }
                                else
                                {
                                    set = 0; /* G0 $ */
                                }

                                /* Check for DRCS/Other 4th byte */
                                if( c3 == 0x20 || c3 == 0x28 )
                                {
                                    /* Read one more byte */
                                    if( i < i_in ) c3 = p_in[i++];
                                }

                                /* Final byte c3 defines the charset */
                                if( c2 == 0x24 ) /* Multi-byte */
                                {
                                    ctx.graphic[set] = c3;
                                    ctx.graphic_mode[set] = (c3 == 0x20) ? MODE_DRCS : MODE_GRAPHIC; /* Simplification */
                                    ctx.graphic_byte[set] = 2;
                                }
                            }
                        }
                        else if( c2 >= 0x28 && c2 <= 0x2B ) /* Invoke SB G0..G3 */
                        {
                             uint8_t set = c2 - 0x28;
                             if( i < i_in )
                             {
                                 uint8_t c3 = p_in[i++];
                                 /* Check for DRCS/Other 4th byte */
                                 if( c3 == 0x20 || c3 == 0x28 )
                                 {
                                     /* Read one more byte */
                                     if( i < i_in ) c3 = p_in[i++];
                                 }

                                 ctx.graphic[set] = c3;
                                 ctx.graphic_mode[set] = (c3 == 0x20) ? MODE_DRCS : MODE_GRAPHIC;
                                 ctx.graphic_byte[set] = 1;

                                 if( c3 == 0x20 ) /* SPC */
                                     arib_buf_append( &p_sjis, &i_sjis, &i_sjis_alloc, 0x81, 0x40 );
                             }
                        }
                        else if( c2 == 0x6E ) ctx.graphic_l = 2; /* LS2 */
                        else if( c2 == 0x6F ) ctx.graphic_l = 3; /* LS3 */
                        else if( c2 == 0x7E ) ctx.graphic_r = 1; /* LS1R */
                        else if( c2 == 0x7D ) ctx.graphic_r = 2; /* LS2R */
                        else if( c2 == 0x7C ) ctx.graphic_r = 3; /* LS3R */
                    }
                    break;
                case 0x20: /* SP */
                    arib_buf_append( &p_sjis, &i_sjis, &i_sjis_alloc,
                                     b_graphic_normal ? 0x81 : 0x20,
                                     b_graphic_normal ? 0x40 : 0x00 );
                    break;
            }
        }
        else if( c <= 0x7E ) /* GL */
        {
            ts_arib_process_gl( &ctx, c, p_in, i_in, &i, &stream,
                                &p_sjis, &i_sjis, &i_sjis_alloc, b_graphic_normal );
        }
        else if( c <= 0xA0 ) /* C1 */
        {
             switch( c )
             {
                 case 0x89: b_graphic_normal = false; break; /* MSZ */
                 case 0x8A: b_graphic_normal = true; break; /* NSZ */
                 case 0x8B: /* SZX */
                     if( i < i_in ) i++;
                     break;
                 case 0x90: /* COL */
                     if( i < i_in )
                     {
                         uint8_t p1 = p_in[i++];
                         if( p1 == 0x20 && i < i_in ) i++;
                     }
                     break;
                 case 0x95: /* MACRO */
                     while( i < i_in && p_in[i] != 0x4F ) i++;
                     if( i < i_in ) i++;
                     break;
                 case 0x9D: /* TIME */
                 {
                     if( i < i_in )
                     {
                         uint8_t p1 = p_in[i++];
                         if( p1 == 0x20 )
                         {
                             if( i < i_in ) i++;
                         }
                         else if( p1 == 0x28 )
                         {
                              if( i < i_in ) i++;
                         }
                     }
                     break;
                 }
             }
        }
        else if( c != 0xFF ) /* GR */
        {
            /* GR Logic, mask 0x7F and use graphic_r */
             uint8_t set_idx = ctx.graphic_r;
            if( ctx.graphic_mode[set_idx] != MODE_GRAPHIC )
            {
                if( ctx.graphic_byte[set_idx] == 2 && i < i_in ) i++;
            }
            else
            {
                uint8_t c7 = c & 0x7F;
                uint8_t cs = ctx.graphic[set_idx];

                if( ctx.graphic_byte[set_idx] == 2 )
                {
                     uint8_t c2 = (i < i_in) ? (p_in[i++] & 0x7F) : 0;
                     if( !c2 ) break;
                     uint16_t code = (c7 << 8) | c2;

                      arib_map_str_t *p_map = bsearch( &code, arib_gaiji_map, arib_gaiji_map_count,
                                                      sizeof(arib_map_str_t), cmp_u16_str );
                     if( p_map )
                     {
                         arib_flush( &stream, &p_sjis, &i_sjis );
                         vlc_memstream_puts( &stream, p_map->val );
                     }
                     else
                     {
                         uint8_t sjis[2];
                         get_sjis_kanji( c7, c2, sjis );
                         arib_buf_append( &p_sjis, &i_sjis, &i_sjis_alloc, sjis[0], sjis[1] );
                     }
                }
                else
                {
                    /* 1-byte GR */
                    const arib_map_sjis_t *p_el = NULL;
                    uint8_t val[2] = {0};

                    if( cs == CS_ASCII || cs == CS_PROP_ASCII )
                    {
                         if( b_graphic_normal )
                             p_el = bsearch( &c7, arib_ascii_map, arib_ascii_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );
                         else
                             val[0] = c7;
                    }
                    else if( cs == CS_HIRAGANA || cs == CS_PROP_HIRAGANA )
                         p_el = bsearch( &c7, arib_hiragana_map, arib_hiragana_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );
                    else if( cs == CS_KATAKANA || cs == CS_PROP_KATAKANA || cs == CS_JIS_X0201_KATAKANA )
                         p_el = bsearch( &c7, arib_katakana_map, arib_katakana_map_count, sizeof(arib_map_sjis_t), cmp_u8_sjis );

                    if( p_el )
                    {
                        val[0] = p_el->val[0];
                        val[1] = p_el->val[1];
                    }
                    else if( !val[0] )
                    {
                        val[0] = c7; /* Default pass-through? */
                    }

                    arib_buf_append( &p_sjis, &i_sjis, &i_sjis_alloc, val[0], val[1] );
                }
            }
        }
    }

    arib_flush( &stream, &p_sjis, &i_sjis );
    free( p_sjis );

    if( vlc_memstream_close( &stream ) )
        return NULL;

    return stream.ptr;
}
