/*****************************************************************************
 * mpeg4audio.c: MPEG-4 audio packetizer unit testing
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#undef NDEBUG
#include <assert.h>

#include <vlc/vlc.h>
#include <vlc_codec.h>
#include <vlc_meta.h>
#include <vlc_modules.h>

#include "../../../lib/libvlc_internal.h"
#include "../../libvlc/test.h"

struct packetizer_owner
{
    decoder_t packetizer;
    es_format_t fmt_in;
};

static decoder_t *CreatePacketizer(libvlc_instance_t *vlc)
{
    struct packetizer_owner *owner =
        vlc_object_create(vlc->p_libvlc_int, sizeof(*owner));
    if (owner == NULL)
        return NULL;

    decoder_t *packetizer = &owner->packetizer;
    es_format_Init(&owner->fmt_in, AUDIO_ES, VLC_CODEC_MP4A);
    es_format_Init(&packetizer->fmt_out, AUDIO_ES, 0);
    owner->fmt_in.i_original_fourcc = VLC_FOURCC('A', 'D', 'T', 'S');
    owner->fmt_in.b_packetized = false;
    packetizer->fmt_in = &owner->fmt_in;

    decoder_LoadModule(packetizer, true, false);
    if (packetizer->p_module == NULL)
    {
        es_format_Clean(&owner->fmt_in);
        es_format_Clean(&packetizer->fmt_out);
        vlc_object_delete(packetizer);
        return NULL;
    }
    return packetizer;
}

static void DeletePacketizer(decoder_t *packetizer)
{
    struct packetizer_owner *owner =
        container_of(packetizer, struct packetizer_owner, packetizer);

    module_unneed(packetizer, packetizer->p_module);
    es_format_Clean(&owner->fmt_in);
    es_format_Clean(&packetizer->fmt_out);
    if (packetizer->p_description != NULL)
        vlc_meta_Delete(packetizer->p_description);
    vlc_object_delete(packetizer);
}

static block_t *PacketizeNext(decoder_t *packetizer, block_t **input)
{
    block_t *output;
    do
        output = packetizer->pf_packetize(packetizer, input);
    while (output == NULL && *input != NULL);
    return output;
}

static void TestChannelConfigurationChanges(libvlc_instance_t *vlc)
{
    /* Four one-byte AAC payloads: two 5.1 frames followed by two stereo
     * frames. The fourth header lets the packetizer validate the third. */
    static const uint8_t adts[] = {
        0xff, 0xf1, 0x4d, 0x80, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4d, 0x80, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0x1f, 0xfc, 0x00,
    };
    static const uint8_t asc_5_1[] = { 0x11, 0xb0 };
    static const uint8_t asc_stereo[] = { 0x11, 0x90 };

    decoder_t *packetizer = CreatePacketizer(vlc);
    assert(packetizer != NULL);

    block_t *input = block_Alloc(sizeof(adts));
    assert(input != NULL);
    memcpy(input->p_buffer, adts, sizeof(adts));
    input->i_pts = VLC_TICK_0;

    block_t *output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    assert(packetizer->fmt_out.audio.i_channels == 6);
    assert(packetizer->fmt_out.i_extra == sizeof(asc_5_1));
    assert(memcmp(packetizer->fmt_out.p_extra, asc_5_1,
                  sizeof(asc_5_1)) == 0);

    es_format_t fmt_5_1;
    es_format_Init(&fmt_5_1, UNKNOWN_ES, 0);
    assert(es_format_Copy(&fmt_5_1, &packetizer->fmt_out) == VLC_SUCCESS);
    block_Release(output);

    output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    block_Release(output);

    output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    assert(packetizer->fmt_out.audio.i_channels == 2);
    assert(packetizer->fmt_out.i_extra == sizeof(asc_stereo));
    assert(memcmp(packetizer->fmt_out.p_extra, asc_stereo,
                  sizeof(asc_stereo)) == 0);
    assert(!es_format_IsSimilar(&fmt_5_1, &packetizer->fmt_out));
    block_Release(output);
    block_Release(input);

    es_format_Clean(&fmt_5_1);
    DeletePacketizer(packetizer);
}

static void TestDualMonoChanges(libvlc_instance_t *vlc)
{
    /* Stereo, dual mono, then stereo again. The final header lets the
     * packetizer validate and return the preceding stereo frame. */
    static const uint8_t adts[] = {
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4c, 0x00, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0x1f, 0xfc, 0x00,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0x1f, 0xfc, 0x00,
    };

    decoder_t *packetizer = CreatePacketizer(vlc);
    assert(packetizer != NULL);

    block_t *input = block_Alloc(sizeof(adts));
    assert(input != NULL);
    memcpy(input->p_buffer, adts, sizeof(adts));
    input->i_pts = VLC_TICK_0;

    block_t *output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    assert(!(packetizer->fmt_out.audio.i_chan_mode &
             AOUT_CHANMODE_DUALMONO));

    es_format_t fmt_stereo;
    es_format_Init(&fmt_stereo, UNKNOWN_ES, 0);
    assert(es_format_Copy(&fmt_stereo, &packetizer->fmt_out) == VLC_SUCCESS);
    block_Release(output);

    output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    assert(packetizer->fmt_out.audio.i_channels == 2);
    assert(packetizer->fmt_out.audio.i_chan_mode & AOUT_CHANMODE_DUALMONO);
    assert(!es_format_IsSimilar(&fmt_stereo, &packetizer->fmt_out));
    block_Release(output);

    output = PacketizeNext(packetizer, &input);
    assert(output != NULL);
    assert(!(packetizer->fmt_out.audio.i_chan_mode &
             AOUT_CHANMODE_DUALMONO));
    assert(es_format_IsSimilar(&fmt_stereo, &packetizer->fmt_out));
    block_Release(output);
    block_Release(input);

    es_format_Clean(&fmt_stereo);
    DeletePacketizer(packetizer);
}

int main(void)
{
    test_init();
    libvlc_instance_t *vlc = libvlc_new(0, NULL);
    assert(vlc != NULL);

    TestChannelConfigurationChanges(vlc);
    TestDualMonoChanges(vlc);

    libvlc_release(vlc);
    return 0;
}
