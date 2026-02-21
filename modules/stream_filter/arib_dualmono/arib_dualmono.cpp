/*****************************************************************************
 * arib_dualmono.cpp : ARIB DualMono audio separator stream filter
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_block.h>

#include <tsreadex/servicefilter.hpp>

#include <new>

struct stream_sys_t
{
    CServiceFilter *p_filter;
    block_t        *p_list;
    uint8_t         ts_buf[188];
    int             i_ts_buf;
};

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin ()
    set_subcategory(SUBCAT_INPUT_STREAM_FILTER)
    set_capability("stream_filter", 0)
    add_shortcut("arib_dualmono")
    set_description(N_("ARIB DualMono audio separator stream filter"))
    set_callbacks(Open, Close)
vlc_module_end ()

static size_t RemainRead(stream_t *p_stream, uint8_t *p_data, size_t i_toread)
{
    stream_sys_t *p_sys = (stream_sys_t*)p_stream->p_sys;
    size_t i_total = 0;

    while (p_sys->p_list && i_toread)
    {
        size_t i_copy = __MIN(i_toread, p_sys->p_list->i_buffer);
        memcpy(p_data, p_sys->p_list->p_buffer, i_copy);

        i_toread -= i_copy;
        i_total += i_copy;
        p_data += i_copy;

        p_sys->p_list->i_buffer -= i_copy;
        p_sys->p_list->p_buffer += i_copy;

        if (p_sys->p_list->i_buffer == 0)
        {
            block_t *p_prevhead = p_sys->p_list;
            p_sys->p_list = p_sys->p_list->p_next;
            block_Release(p_prevhead);
        }
    }
    return i_total;
}

static bool RemainAdd(stream_t *p_stream, const uint8_t *p_data, size_t i_size)
{
    stream_sys_t *p_sys = (stream_sys_t*)p_stream->p_sys;
    if (i_size == 0) return true;
    block_t *p_block = block_Alloc(i_size);
    if (!p_block) return false;
    memcpy(p_block->p_buffer, p_data, i_size);
    p_block->i_buffer = i_size;
    block_ChainAppend(&p_sys->p_list, p_block);
    return true;
}

static void RemainFlush(stream_sys_t *p_sys)
{
    block_ChainRelease(p_sys->p_list);
    p_sys->p_list = NULL;
}

static ssize_t Read(stream_t *p_stream, void *p_buf, size_t i_toread)
{
    stream_sys_t *p_sys = (stream_sys_t*)p_stream->p_sys;
    uint8_t *p_dst = (uint8_t*)p_buf;
    size_t i_total_read = 0;

    if (!i_toread) return -1;

    size_t i_fromremain = RemainRead(p_stream, p_dst, i_toread);
    i_total_read += i_fromremain;
    p_dst += i_fromremain;
    i_toread -= i_fromremain;

    uint8_t read_buf[32768];

    while (i_toread)
    {
        int i_srcread = vlc_stream_Read(p_stream->s, read_buf, sizeof(read_buf));
        if (i_srcread <= 0) break;

        int offset = 0;
        while (offset < i_srcread)
        {
            int piece = __MIN((int)(188 - p_sys->i_ts_buf), i_srcread - offset);
            memcpy(p_sys->ts_buf + p_sys->i_ts_buf, read_buf + offset, piece);
            p_sys->i_ts_buf += piece;
            offset += piece;

            if (p_sys->i_ts_buf == 188)
            {
                if (p_sys->ts_buf[0] != 0x47) {
                    msg_Warn(p_stream, "Loss of TS sync");
                    int i_sync = 1;
                    while (i_sync < 188 && p_sys->ts_buf[i_sync] != 0x47) i_sync++;
                    int remain = 188 - i_sync;
                    if (remain > 0) memmove(p_sys->ts_buf, p_sys->ts_buf + i_sync, remain);
                    p_sys->i_ts_buf = remain;
                    continue;
                }

                p_sys->p_filter->AddPacket(p_sys->ts_buf);
                p_sys->i_ts_buf = 0;
            }
        }

        const auto& packets = p_sys->p_filter->GetPackets();
        if (!packets.empty())
        {
            size_t gen_size = packets.size();
            size_t gen_copy = __MIN(gen_size, i_toread);
            memcpy(p_dst, packets.data(), gen_copy);

            i_total_read += gen_copy;
            p_dst += gen_copy;
            i_toread -= gen_copy;

            if (gen_size > gen_copy) {
                RemainAdd(p_stream, packets.data() + gen_copy, gen_size - gen_copy);
            }
            p_sys->p_filter->ClearPackets();
        }
    }

    return i_total_read > 0 ? i_total_read : 0;
}

static int Seek(stream_t *p_stream, uint64_t i_pos)
{
    int i_ret = vlc_stream_Seek(p_stream->s, i_pos);
    if (i_ret == VLC_SUCCESS)
    {
        stream_sys_t *p_sys = (stream_sys_t*)p_stream->p_sys;
        RemainFlush(p_sys);
        p_sys->i_ts_buf = 0;
        p_sys->p_filter->ClearPackets();
    }
    return i_ret;
}

static int Control(stream_t *p_stream, int i_query, va_list args)
{
    return vlc_stream_vaControl(p_stream->s, i_query, args);
}

static int Open(vlc_object_t *p_object)
{
    stream_t *p_stream = (stream_t *)p_object;

    // Optional: Only apply to streams that look like TS
    // (If probing data is small, could just proceed and let it fail gracefully)
    // For now we assume if forced/matched, we apply the filter.

    stream_sys_t *p_sys = (stream_sys_t*)calloc(1, sizeof(*p_sys));
    if (p_sys == NULL)
        return VLC_ENOMEM;

    p_sys->p_filter = new (std::nothrow) CServiceFilter();
    if (!p_sys->p_filter)
    {
        free(p_sys);
        return VLC_ENOMEM;
    }

    // Process the first program found in PAT instead of skipping (passthrough mode)
    p_sys->p_filter->SetProgramNumberOrIndex(-1);

    // Set audio1 mode to 12 (+4 mono to stereo, +8 activates TransmuxDualMono in tsreadex logic)
    p_sys->p_filter->SetAudio1Mode(12);
    // Set audio2 mode to 4 (+4 mono to stereo)
    p_sys->p_filter->SetAudio2Mode(4);

    msg_Dbg(p_stream, "ARIB DualMono filter initialized (tsreadex processing enabled)");

    p_stream->p_sys = p_sys;
    p_stream->pf_read = Read;
    p_stream->pf_seek = Seek;
    p_stream->pf_control = Control;

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_object)
{
    stream_t *p_stream = (stream_t *)p_object;
    stream_sys_t *p_sys = (stream_sys_t*)p_stream->p_sys;

    RemainFlush(p_sys);
    delete p_sys->p_filter;
    free(p_sys);
}
