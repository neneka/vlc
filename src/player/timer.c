/*****************************************************************************
 * player.c: Player interface
 *****************************************************************************
 * Copyright © 2018 VLC authors and VideoLAN
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

#include <limits.h>

#include "player.h"

#define BYTE_POSITION_HISTORY_LENGTH VLC_TICK_FROM_SEC(30)
#define BYTE_POSITION_RATE_WINDOW VLC_TICK_FROM_SEC(10)
#define BYTE_POSITION_RATE_MIN_SPAN VLC_TICK_FROM_SEC(5)
#define BYTE_POSITION_LOCAL_SEEK_MAX VLC_TICK_FROM_SEC(30)
#define BYTE_POSITION_DIRECT_MAX_ERROR VLC_TICK_FROM_SEC(1)
#define BYTE_POSITION_RATE_MIN_POINTS 8

static void
vlc_player_ResetBytePositionHistory(vlc_player_t *player)
{
    player->timer.byte_position_history.next = 0;
    player->timer.byte_position_history.count = 0;
}

static void
vlc_player_AddBytePositionPoint(vlc_player_t *player, vlc_tick_t ts,
                                double position)
{
    size_t index = player->timer.byte_position_history.next;
    player->timer.byte_position_history.points[index] =
        (struct vlc_player_byte_position_point) {
            .ts = ts,
            .position = position,
        };
    player->timer.byte_position_history.next =
        (index + 1) % VLC_PLAYER_BYTE_POSITION_HISTORY_SIZE;
    if (player->timer.byte_position_history.count
        < VLC_PLAYER_BYTE_POSITION_HISTORY_SIZE)
        player->timer.byte_position_history.count++;
}

static double
vlc_player_ClampPosition(double position)
{
    if (position < 0.0)
        return 0.0;
    if (position > 1.0)
        return 1.0;
    return position;
}

void
vlc_player_ResetTimer(vlc_player_t *player)
{
    vlc_mutex_lock(&player->timer.lock);

    player->timer.input_live = false;
    player->timer.input_length = VLC_TICK_INVALID;
    player->timer.input_normal_time = VLC_TICK_0;
    player->timer.last_ts = VLC_TICK_INVALID;
    player->timer.start_offset = 0;
    player->timer.input_position = 0;
    player->timer.smpte_source.smpte.last_framenum = ULONG_MAX;
    player->timer.seek_ts = VLC_TICK_INVALID;
    player->timer.seek_position = -1;
    player->timer.trust_demux_pos = -1;
    player->timer.position_anchor_initialized = false;
    vlc_player_ResetBytePositionHistory(player);
    player->timer.update_state = UPDATE_STATE_RESUMED;
    player->timer.pause_date = VLC_TICK_INVALID;
    player->timer.stopping = false;

    vlc_mutex_unlock(&player->timer.lock);
}

static void
vlc_player_SendTimerSeek(vlc_player_t *player,
                         struct vlc_player_timer_source *source,
                         const struct vlc_player_timer_point *point,
                         bool is_smpte)
{
    (void) player;
    vlc_player_timer_id *timer;

    vlc_list_foreach(timer, &source->listeners, node)
    {
        if (is_smpte)
        {
            if (timer->smpte_cbs->on_seek != NULL)
                timer->smpte_cbs->on_seek(point, timer->data);
        }
        else
        {
            if (timer->cbs->on_seek != NULL)
                timer->cbs->on_seek(point, timer->data);
        }
    }
}

static void
vlc_player_SendTimerPause(vlc_player_t *player,
                          struct vlc_player_timer_source *source,
                          vlc_tick_t system_date, bool is_smpte)
{
    (void) player;

    vlc_player_timer_id *timer;
    vlc_list_foreach(timer, &source->listeners, node)
    {
        if (is_smpte)
        {
            if (timer->smpte_cbs->on_paused != NULL)
                timer->smpte_cbs->on_paused(system_date, timer->data);
        }
        else
        {
            timer->last_update_date = VLC_TICK_INVALID;
            if (timer->cbs->on_paused != NULL)
                timer->cbs->on_paused(system_date, timer->data);
        }
    }
}

static void
vlc_player_SendTimerSourceUpdates(vlc_player_t *player,
                                  struct vlc_player_timer_source *source,
                                  bool force_update,
                                  const struct vlc_player_timer_point *point)
{
    (void) player;
    vlc_player_timer_id *timer;

    vlc_list_foreach(timer, &source->listeners, node)
    {
        /* Respect refresh delay of the timer */
        if (force_update || timer->period == VLC_TICK_INVALID
         || timer->last_update_date == VLC_TICK_INVALID
         || point->system_date == VLC_TICK_MAX /* always update when paused */
         || point->system_date - timer->last_update_date >= timer->period)
        {
            timer->cbs->on_update(point, timer->data);
            timer->last_update_date = point->system_date == VLC_TICK_MAX ?
                                      VLC_TICK_INVALID : point->system_date;
        }
    }
}

static void
vlc_player_SendSmpteTimerSourceUpdates(vlc_player_t *player,
                                       struct vlc_player_timer_source *source,
                                       const struct vlc_player_timer_point *point)
{
    (void) player;
    vlc_player_timer_id *timer;

    struct vlc_player_timer_smpte_timecode tc;
    unsigned long framenum;
    unsigned frame_rate;
    unsigned frame_rate_base;

    if (source->smpte.df > 0)
    {
        /* Use the exact SMPTE framerate that can be different from the input
         * source (at demuxer/decoder level) */
        assert(source->smpte.df_fps == 30 || source->smpte.df_fps == 60);
        frame_rate = source->smpte.df_fps * 1000;
        frame_rate_base = 1001;

        /* Convert the ts to a frame number */
        framenum = round(point->ts * frame_rate
                         / (double) frame_rate_base / VLC_TICK_FROM_SEC(1));

        /* Drop 2 or 4 frames every minutes except every 10 minutes in order to
         * make one hour of timecode match one hour on the clock. */
        ldiv_t res;
        res = ldiv(framenum, source->smpte.frames_per_10mins);

        framenum += (9 * source->smpte.df * res.quot)
                  + (source->smpte.df * ((res.rem - source->smpte.df)
                     / (source->smpte.frames_per_10mins / 10)));

        tc.drop_frame = true;

        /* Use 30 or 60 framerates for the next frames/seconds/minutes/hours
         * calculaton */
        frame_rate = source->smpte.df_fps;
        frame_rate_base = 1;
    }
    else
    {
        frame_rate = source->smpte.frame_rate;
        frame_rate_base = source->smpte.frame_rate_base;

        /* Convert the ts to a frame number */
        framenum = round(point->ts * frame_rate
                         / (double) frame_rate_base / VLC_TICK_FROM_SEC(1));

        tc.drop_frame = false;
    }
    if (framenum == source->smpte.last_framenum)
        return;

    source->smpte.last_framenum = framenum;

    tc.frames = framenum % (frame_rate / frame_rate_base);
    tc.seconds = (framenum * frame_rate_base / frame_rate) % 60;
    tc.minutes = (framenum * frame_rate_base / frame_rate / 60) % 60;
    tc.hours = framenum * frame_rate_base / frame_rate / 3600;

    tc.frame_resolution = source->smpte.frame_resolution;

    vlc_list_foreach(timer, &source->listeners, node)
        timer->smpte_cbs->on_update(&tc, timer->data);
}

static void
vlc_player_UpdateSmpteTimerFPS(vlc_player_t *player,
                               struct vlc_player_timer_source *source,
                               unsigned frame_rate, unsigned frame_rate_base)
{
    (void) player;
    source->smpte.frame_rate = frame_rate;
    source->smpte.frame_rate_base = frame_rate_base;

    /* Calculate everything that will be needed to create smpte timecodes */
    source->smpte.frame_resolution = 0;

    unsigned max_frames = frame_rate / frame_rate_base;

    if (max_frames == 29 && (100 * frame_rate / frame_rate_base) == 2997)
    {
        /* SMPTE Timecode: 29.97 fps DF */
        source->smpte.df = 2;
        source->smpte.df_fps = 30;
        source->smpte.frames_per_10mins = 17982; /* 29.97 * 60 * 10 */
    }
    else if (max_frames == 59 && (100 * frame_rate / frame_rate_base) == 5994)
    {
        /* SMPTE Timecode: 59.94 fps DF */
        source->smpte.df = 4;
        source->smpte.df_fps = 60;
        source->smpte.frames_per_10mins = 35964; /* 59.94 * 60 * 10 */
    }
    else
        source->smpte.df = 0;

    while (max_frames != 0)
    {
        max_frames /= 10;
        source->smpte.frame_resolution++;
    }
}

void
vlc_player_UpdateTimerEvent(vlc_player_t *player, vlc_es_id_t *es_source,
                            enum vlc_player_timer_event event,
                            vlc_tick_t system_date)
{
    vlc_mutex_lock(&player->timer.lock);

    /* Discontinuity is signalled by all output clocks and the input.
     * discard the event if it was already signalled or not on the good
     * es_source. */

    switch (event)
    {
        case VLC_PLAYER_TIMER_EVENT_DISCONTINUITY:
            assert(system_date == VLC_TICK_INVALID);
            for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
            {
                struct vlc_player_timer_source *source = &player->timer.sources[i];
                if (source->es != es_source)
                    continue;

                /* There can be several discontinuities on the same source
                 * for one seek request, hence the need of the
                 * 'timer.seeking' variable to notify only once the end of
                 * the seek request. */
                if (source->seeking)
                {
                    source->seeking = false;
                    vlc_player_SendTimerSeek(player, source, NULL,
                                             i == VLC_PLAYER_TIMER_TYPE_SMPTE);
                }
                source->point.system_date = VLC_TICK_INVALID;
                if (i == VLC_PLAYER_TIMER_TYPE_BEST)
                    vlc_player_ResetBytePositionHistory(player);
            }
            break;

        case VLC_PLAYER_TIMER_EVENT_PAUSED:
            assert(system_date != VLC_TICK_INVALID);
            player->timer.update_state = UPDATE_STATE_PAUSED;
            player->timer.pause_date = system_date;

            for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
            {
                struct vlc_player_timer_source *source = &player->timer.sources[i];
                if (source->es != es_source)
                    continue;
                vlc_player_SendTimerPause(player, source, system_date,
                                          i == VLC_PLAYER_TIMER_TYPE_SMPTE);
            }

            break;

        case VLC_PLAYER_TIMER_EVENT_PLAYING:
            assert(!player->timer.stopping);
            player->timer.update_state = UPDATE_STATE_RESUMING;
            break;

        case VLC_PLAYER_TIMER_EVENT_STOPPING:
            player->timer.stopping = true;
            for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
            {
                struct vlc_player_timer_source *source = &player->timer.sources[i];
                vlc_player_SendTimerPause(player, source, system_date,
                                          i == VLC_PLAYER_TIMER_TYPE_SMPTE);
            }
            break;

        default:
            vlc_assert_unreachable();
    }

    vlc_mutex_unlock(&player->timer.lock);
}

void
vlc_player_UpdateTimerSeekState(vlc_player_t *player, vlc_tick_t time,
                                double position)
{
    vlc_mutex_lock(&player->timer.lock);
    struct vlc_player_timer_source *source = &player->timer.best_source;

    if (time == VLC_TICK_INVALID)
    {
        assert(position >= 0);
        if (source->point.length != VLC_TICK_INVALID)
            player->timer.seek_ts = position * source->point.length;
        else
            player->timer.seek_ts = VLC_TICK_INVALID;
    }
    else
        player->timer.seek_ts = time;

    if (position < 0)
    {
        assert(time != VLC_TICK_INVALID);
        if (source->point.length != VLC_TICK_INVALID && !source->point.live)
            player->timer.seek_position = time / (double) source->point.length;
    }
    else
        player->timer.seek_position = position;

    const struct vlc_player_timer_point point =
    {
        .position = player->timer.seek_position,
        .rate = source->point.rate,
        .ts = player->timer.seek_ts,
        .length = source->point.length,
        .live = source->point.live,
        .system_date = VLC_TICK_MAX,
    };

    source->seeking = true;
    vlc_player_SendTimerSeek(player, source, &point, false);

    source = &player->timer.smpte_source;
    source->seeking = true;
    vlc_player_SendTimerSeek(player, source, &point, true);

    vlc_mutex_unlock(&player->timer.lock);
}

static void
vlc_player_UpdateTimerSource(vlc_player_t *player,
                             struct vlc_player_timer_source *source,
                             double rate, vlc_tick_t ts, vlc_tick_t system_date,
                             double byte_position)
{
    assert(ts >= VLC_TICK_0);
    assert(player->timer.input_normal_time >= VLC_TICK_0);

    const vlc_tick_t previous_system_date = source->point.system_date;
    const double previous_position = source->point.position;

    source->point.rate = rate;
    source->point.ts = ts - player->timer.input_normal_time - player->timer.start_offset + VLC_TICK_0;
    source->point.length = player->timer.input_length;
    source->point.live = player->timer.input_live;

    /* Put an invalid date for the first point in order to disable
     * interpolation (behave as paused), indeed, we should wait for one more
     * point before starting interpolation (ideally, it should be more) */
    if (source->point.system_date == VLC_TICK_INVALID)
        source->point.system_date = VLC_TICK_MAX;
    else
        source->point.system_date = system_date;

    struct vlc_player_input *input = player->input;
    bool trust_demux_pos = false;

    if (player->timer.trust_demux_pos != -1)
    {
        trust_demux_pos = (player->timer.trust_demux_pos == 1);
    }
    else
    {
        int trust_status = 0;
        if (input && input->thread)
        {
            input_thread_private_t *priv = input_priv(input->thread);
            if (priv->master && priv->master->p_demux)
            {
                demux_t *p_demux = priv->master->p_demux;
                if (var_GetBool(p_demux, "is-mpegts"))
                {
                  bool stream_can_fastseek = false;
                  if (p_demux->s)
                    vlc_stream_Control(p_demux->s, STREAM_CAN_FASTSEEK, &stream_can_fastseek);

                  msg_Dbg(player, "TIMER: MPEG-TS detected via is-mpegts. stream_can_fastseek: %d", stream_can_fastseek);
                  if (!stream_can_fastseek)
                    trust_status = 1;
                }
            }
        }
        player->timer.trust_demux_pos = trust_status;
        trust_demux_pos = (trust_status == 1);
    }

    if (!trust_demux_pos && source->point.length != VLC_TICK_INVALID && !source->point.live)
        source->point.position = (ts - player->timer.input_normal_time - player->timer.start_offset)
                               / (double) source->point.length;
    else if (trust_demux_pos && source == &player->timer.best_source &&
             source->point.length > 0 && !source->point.live)
    {
        source->point.position = previous_position;

        if (byte_position >= 0.0)
        {
            source->point.position = vlc_player_ClampPosition(byte_position);
            player->timer.position_anchor_initialized = true;
            vlc_player_AddBytePositionPoint(player, source->point.ts,
                                            source->point.position);
        }
        else if (previous_system_date == VLC_TICK_INVALID)
        {
            if (player->timer.seek_position >= 0.0)
            {
                /* The first output-clock point after a seek is the moment the
                 * requested byte position starts being presented. */
                source->point.position = player->timer.seek_position;
            }
            else if (!player->timer.position_anchor_initialized)
            {
                /* The first output clock may arrive before input_normal_time
                 * is known, so its timestamp cannot establish a normalized
                 * position.  Playback without a pending seek starts at zero
                 * and advances from output-clock updates from this point on. */
                source->point.position = 0.0;
            }

            player->timer.position_anchor_initialized = true;
        }

        if (source->point.position < 0.0)
            source->point.position = 0.0;
        else if (source->point.position > 1.0)
            source->point.position = 1.0;

        /* Byte position is sampled on output-clock updates. Time-based
         * interpolation would turn it back into a timestamp position. */
        source->point.length = VLC_TICK_INVALID;
    }
    else
        source->point.position = player->timer.input_position;
}

static void
vlc_player_UpdateTimerBestSource(vlc_player_t *player, vlc_es_id_t *es_source,
                                 bool es_source_is_master,
                                 const struct vlc_player_timer_point *point,
                                 vlc_tick_t system_date,
                                 bool force_update)
{
    /* Best source priority:
     * 1/ es_source != NULL + master (from the master ES track)
     * 2/ es_source != NULL (from the first ES track updated)
     * 3/ es_source == NULL (from the input)
     */
    struct vlc_player_timer_source *source = &player->timer.best_source;
    vlc_es_id_t *previous_es = source->es;
    if (!source->es || es_source_is_master)
        source->es = es_source;
    if (source->es != previous_es)
        vlc_player_ResetBytePositionHistory(player);

    /* Notify the best source */
    if (source->es == es_source || force_update)
    {
        if (source->point.rate != point->rate)
        {
            player->timer.last_ts = VLC_TICK_INVALID;
            force_update = true;
        }

        /* When paused (VLC_TICK_MAX), the same ts can be send more than one
         * time from the video source, only send it if different in that case.
         */
        if (point->ts != player->timer.last_ts
          || (source->point.system_date != system_date && system_date != VLC_TICK_MAX))
        {
            vlc_player_UpdateTimerSource(player, source, point->rate, point->ts,
                                         system_date, point->position);
            player->timer.last_ts = point->ts;

            /* It is possible to receive valid points while seeking. These
             * points could be updated when the input thread didn't yet process
             * the seek request. */
            if (!source->seeking)
            {
                /* Reset seek time/position now that we receive a valid point
                 * and seek was processed */
                player->timer.seek_ts = VLC_TICK_INVALID;
                player->timer.seek_position = -1;
            }

            if (!vlc_list_is_empty(&source->listeners))
                vlc_player_SendTimerSourceUpdates(player, source, force_update,
                                                  &source->point);

            if (player->timer.update_state == UPDATE_STATE_RESUMING)
            {
                player->timer.update_state = UPDATE_STATE_RESUMED;
                player->timer.pause_date = VLC_TICK_INVALID;
            }
        }
    }
}

static void
vlc_player_UpdateTimerSmpteSource(vlc_player_t *player, vlc_es_id_t *es_source,
                                  const struct vlc_player_timer_point *point,
                                  vlc_tick_t system_date,
                                  unsigned frame_rate, unsigned frame_rate_base)
{
    struct vlc_player_timer_source *source = &player->timer.smpte_source;
    /* SMPTE source: only the video source */
    if (!source->es && es_source && vlc_es_id_GetCat(es_source) == VIDEO_ES)
        source->es = es_source;

    /* Notify the SMPTE source, also notify when the video output was rendered
     * while the clock was paused */
    if (source->es == es_source && source->es)
    {
        if (frame_rate != 0 && (frame_rate != source->smpte.frame_rate
         || frame_rate_base != source->smpte.frame_rate_base))
        {
            assert(frame_rate_base != 0);
            vlc_player_UpdateSmpteTimerFPS(player, source, frame_rate,
                                           frame_rate_base);
        }

        if (source->smpte.frame_rate != 0)
        {
            vlc_player_UpdateTimerSource(player, source, point->rate, point->ts,
                                         system_date, point->position);

            if (!vlc_list_is_empty(&source->listeners))
                vlc_player_SendSmpteTimerSourceUpdates(player, source,
                                                       &source->point);
        }
    }
}

void
vlc_player_UpdateTimer(vlc_player_t *player, vlc_es_id_t *es_source,
                       bool es_source_is_master,
                       const struct vlc_player_timer_point *point,
                       vlc_tick_t normal_time,
                       unsigned frame_rate, unsigned frame_rate_base,
                       vlc_tick_t start_offset)
{
    assert(point);
    /* A null source can't be the master */
    assert(es_source == NULL ? !es_source_is_master : true);

    vlc_mutex_lock(&player->timer.lock);

    bool force_update = false;
    if (!es_source) /* input source */
    {
        /* Only valid for input sources */
        if (normal_time != VLC_TICK_INVALID
         && player->timer.input_normal_time != normal_time)
        {
            player->timer.input_normal_time = normal_time;
            player->timer.last_ts = VLC_TICK_INVALID;
            force_update = true;
        }
        if (player->timer.input_length != point->length
         && point->length >= VLC_TICK_0)
        {
            player->timer.input_length = point->length;
            player->timer.last_ts = VLC_TICK_INVALID;
            force_update = true;
        }
        if (player->timer.input_live != point->live)
        {
            player->timer.input_live = point->live;
            force_update = true;
        }
        if (start_offset > 0)
            player->timer.start_offset = start_offset;
        /* Will likely be overridden by non input source */
        player->timer.input_position = point->position;

        if (point->ts == VLC_TICK_INVALID
         || point->system_date == VLC_TICK_INVALID)
        {
            /* ts can only be invalid from the input source */
            vlc_mutex_unlock(&player->timer.lock);
            return;
        }
    }

    assert(point->ts != VLC_TICK_INVALID);

    vlc_tick_t system_date = point->system_date;
    if (player->timer.update_state == UPDATE_STATE_PAUSED)
    {
        if (es_source != NULL && point->system_date == VLC_TICK_MAX)
        {
            /* Update was forced, probably a next/prev frame */
            force_update = true;
        }
        system_date = VLC_TICK_MAX;
    }

    if (!player->timer.stopping)
        vlc_player_UpdateTimerBestSource(player, es_source,
                                         es_source_is_master, point, system_date,
                                         force_update);

    vlc_player_UpdateTimerSmpteSource(player, es_source, point, system_date,
                                      frame_rate, frame_rate_base);

    vlc_mutex_unlock(&player->timer.lock);
}

void
vlc_player_RemoveTimerSource(vlc_player_t *player, vlc_es_id_t *es_source)
{
    vlc_mutex_lock(&player->timer.lock);
    struct vlc_player_timer_source *bestsource = &player->timer.best_source;
    struct vlc_player_timer_source *smptesource = &player->timer.smpte_source;

    /* Unlikely case where the source ES is deleted while seeking */
    if (bestsource->es == es_source && bestsource->seeking)
    {
        bestsource->seeking = false;
        vlc_player_SendTimerSeek(player, bestsource, NULL, false);
    }
    if (smptesource->es == es_source && smptesource->seeking)
    {
        smptesource->seeking = false;
        vlc_player_SendTimerSeek(player, smptesource, NULL, true);
    }

    for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
    {
        struct vlc_player_timer_source *source = &player->timer.sources[i];
        if (source->es == es_source)
        {
            /* Discontinuity should have been already signaled */
            assert(source->point.system_date == VLC_TICK_INVALID);
            source->es = NULL;
            if (i == VLC_PLAYER_TIMER_TYPE_BEST)
                vlc_player_ResetBytePositionHistory(player);
        }
    }
    vlc_mutex_unlock(&player->timer.lock);
}

static bool
vlc_player_FindBytePosition(vlc_player_t *player, vlc_tick_t current_ts,
                            vlc_tick_t target_ts, double *position)
{
    const struct vlc_player_byte_position_point *lower = NULL;
    const struct vlc_player_byte_position_point *upper = NULL;

    for (size_t i = 0; i < player->timer.byte_position_history.count; ++i)
    {
        const struct vlc_player_byte_position_point *point =
            &player->timer.byte_position_history.points[i];
        if (point->ts > current_ts || current_ts - point->ts
                                      > BYTE_POSITION_HISTORY_LENGTH)
            continue;

        if (point->ts <= target_ts && (!lower || point->ts > lower->ts))
            lower = point;
        if (point->ts >= target_ts && (!upper || point->ts < upper->ts))
            upper = point;
    }

    if (lower && upper)
    {
        if (lower->ts == upper->ts)
            *position = lower->position;
        else
        {
            const double fraction = (target_ts - lower->ts)
                                  / (double) (upper->ts - lower->ts);
            *position = lower->position
                      + (upper->position - lower->position) * fraction;
        }
        return true;
    }

    const struct vlc_player_byte_position_point *nearest = lower ? lower : upper;
    if (nearest)
    {
        const vlc_tick_t error = nearest->ts > target_ts
                               ? nearest->ts - target_ts
                               : target_ts - nearest->ts;
        if (error <= BYTE_POSITION_DIRECT_MAX_ERROR)
        {
            *position = nearest->position;
            return true;
        }
    }
    return false;
}

static bool
vlc_player_GetLocalBytePositionRate(vlc_player_t *player,
                                    vlc_tick_t current_ts, double *rate,
                                    vlc_tick_t *span)
{
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    vlc_tick_t first_ts = VLC_TICK_MAX;
    vlc_tick_t last_ts = VLC_TICK_INVALID;
    size_t count = 0;

    for (size_t i = 0; i < player->timer.byte_position_history.count; ++i)
    {
        const struct vlc_player_byte_position_point *point =
            &player->timer.byte_position_history.points[i];
        if (point->ts > current_ts || current_ts - point->ts
                                      > BYTE_POSITION_RATE_WINDOW)
            continue;

        const double x = (point->ts - current_ts)
                       / (double) VLC_TICK_FROM_SEC(1);
        const double y = point->position;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        if (point->ts < first_ts)
            first_ts = point->ts;
        if (point->ts > last_ts)
            last_ts = point->ts;
        count++;
    }

    if (count < BYTE_POSITION_RATE_MIN_POINTS || first_ts == VLC_TICK_MAX
     || last_ts - first_ts < BYTE_POSITION_RATE_MIN_SPAN)
        return false;

    const double denominator = count * sum_xx - sum_x * sum_x;
    if (denominator <= 0.0)
        return false;

    *rate = (count * sum_xy - sum_x * sum_y) / denominator;
    *span = last_ts - first_ts;
    return *rate > 0.0;
}

bool
vlc_player_GetByteSeekPosition(vlc_player_t *player, vlc_tick_t target_time,
                               double *position)
{
    vlc_mutex_lock(&player->timer.lock);

    const struct vlc_player_timer_source *source = &player->timer.best_source;
    if (player->timer.trust_demux_pos != 1 || source->es == NULL)
    {
        vlc_mutex_unlock(&player->timer.lock);
        return false;
    }

    if (player->timer.byte_position_history.count == 0
     || source->point.position < 0.0
     || source->point.ts == VLC_TICK_INVALID)
    {
        if (player->timer.input_length <= 0)
        {
            vlc_mutex_unlock(&player->timer.lock);
            return false;
        }
        *position = target_time / (double) player->timer.input_length;
        *position = vlc_player_ClampPosition(*position);
        vlc_mutex_unlock(&player->timer.lock);
        return true;
    }

    const vlc_tick_t current_time = source->point.ts;
    const vlc_tick_t time_delta = target_time - current_time;

    /* A recent backward target already has a presented byte mapping, so it
     * is more accurate than extrapolating either a local or global rate. */
    if (target_time <= current_time
     && current_time - target_time <= BYTE_POSITION_HISTORY_LENGTH
     && vlc_player_FindBytePosition(player, current_time, target_time,
                                    position))
    {
        *position = vlc_player_ClampPosition(*position);
        vlc_mutex_unlock(&player->timer.lock);
        return true;
    }

    if (player->timer.input_length <= 0)
    {
        vlc_mutex_unlock(&player->timer.lock);
        return false;
    }

    const double global_rate = VLC_TICK_FROM_SEC(1)
                             / (double) player->timer.input_length;
    double seek_rate = global_rate;
    const vlc_tick_t abs_delta = time_delta < 0 ? -time_delta : time_delta;

    if (abs_delta <= BYTE_POSITION_LOCAL_SEEK_MAX)
    {
        double local_rate;
        vlc_tick_t span;
        if (vlc_player_GetLocalBytePositionRate(player, current_time,
                                                &local_rate, &span))
        {
            if (local_rate < global_rate * 0.25)
                local_rate = global_rate * 0.25;
            else if (local_rate > global_rate * 4.0)
                local_rate = global_rate * 4.0;

            /* Fade the local estimate in between five and ten seconds of
             * samples to avoid noisy startup and post-discontinuity rates. */
            double weight = (span - BYTE_POSITION_RATE_MIN_SPAN)
                          / (double) (BYTE_POSITION_RATE_WINDOW
                                    - BYTE_POSITION_RATE_MIN_SPAN);
            if (weight > 1.0)
                weight = 1.0;
            seek_rate = global_rate + (local_rate - global_rate) * weight;
        }
    }

    *position = source->point.position
              + time_delta / (double) VLC_TICK_FROM_SEC(1) * seek_rate;
    *position = vlc_player_ClampPosition(*position);
    vlc_mutex_unlock(&player->timer.lock);
    return true;
}

int
vlc_player_GetTimerPoint(vlc_player_t *player, bool *seeking,
                         vlc_tick_t system_now,
                         vlc_tick_t *out_ts, double *out_pos)
{
    int ret = VLC_EGENERIC;
    vlc_mutex_lock(&player->timer.lock);
    bool timer_seeking = player->timer.seek_ts != VLC_TICK_INVALID
                      || player->timer.seek_position >= 0.0f;
    if (*seeking && timer_seeking)
    {
        if (out_ts != NULL)
        {
            if (player->timer.seek_ts == VLC_TICK_INVALID)
                goto end;
            *out_ts = player->timer.seek_ts;
        }
        if (out_pos != NULL)
        {
            if (player->timer.seek_position < 0)
                goto end;
            *out_pos = player->timer.seek_position;
        }
        ret = VLC_SUCCESS;
        goto end;
    }

    if (player->timer.best_source.point.system_date == VLC_TICK_INVALID)
        goto end;

    if (system_now != VLC_TICK_INVALID) /* interpolate */
    {
        /* If paused, force interpolation to the paused date */
        if (player->timer.pause_date != VLC_TICK_INVALID)
            system_now = player->timer.pause_date;
        ret = vlc_player_timer_point_Interpolate(&player->timer.best_source.point,
                                                 system_now, out_ts, out_pos);
    }
    else
    {
        const struct vlc_player_timer_point *point =
            &player->timer.best_source.point;
        if (out_ts)
            *out_ts = point->ts;
        if (out_pos)
            *out_pos = point->position;
        ret = VLC_SUCCESS;
    }

    if (ret == VLC_SUCCESS)
        *seeking = timer_seeking;
end:
    vlc_mutex_unlock(&player->timer.lock);
    return ret;
}

vlc_player_timer_id *
vlc_player_AddTimer(vlc_player_t *player, vlc_tick_t min_period,
                    const struct vlc_player_timer_cbs *cbs, void *data)
{
    assert(min_period >= VLC_TICK_0 || min_period == VLC_TICK_INVALID);
    assert(cbs && cbs->on_update);

    struct vlc_player_timer_id *timer = malloc(sizeof(*timer));
    if (!timer)
        return NULL;
    timer->period = min_period;
    timer->last_update_date = VLC_TICK_INVALID;
    timer->cbs = cbs;
    timer->data = data;

    vlc_mutex_lock(&player->timer.lock);
    vlc_list_append(&timer->node, &player->timer.best_source.listeners);
    vlc_mutex_unlock(&player->timer.lock);

    return timer;
}

vlc_player_timer_id *
vlc_player_AddSmpteTimer(vlc_player_t *player,
                         const struct vlc_player_timer_smpte_cbs *cbs,
                         void *data)
{
    assert(cbs && cbs->on_update);

    struct vlc_player_timer_id *timer = malloc(sizeof(*timer));
    if (!timer)
        return NULL;
    timer->period = VLC_TICK_INVALID;
    timer->last_update_date = VLC_TICK_INVALID;
    timer->smpte_cbs = cbs;
    timer->data = data;

    vlc_mutex_lock(&player->timer.lock);
    vlc_list_append(&timer->node, &player->timer.smpte_source.listeners);
    vlc_mutex_unlock(&player->timer.lock);

    return timer;
}

void
vlc_player_RemoveTimer(vlc_player_t *player, vlc_player_timer_id *timer)
{
    assert(timer);

    vlc_mutex_lock(&player->timer.lock);
    vlc_list_remove(&timer->node);
    vlc_mutex_unlock(&player->timer.lock);

    free(timer);
}

int
vlc_player_timer_point_Interpolate(const struct vlc_player_timer_point *point,
                                   vlc_tick_t system_now,
                                   vlc_tick_t *out_ts, double *out_pos)
{
    assert(point);
    assert(system_now > 0);
    assert(out_ts || out_pos);

    /* A system_date == VLC_TICK_MAX means the clock was paused when it updated
     * this point, so there is nothing to interpolate */
    const vlc_tick_t drift = point->system_date == VLC_TICK_MAX ? 0
                           : (system_now - point->system_date) * point->rate;
    vlc_tick_t ts = point->ts;
    double pos = point->position;

    if (ts != VLC_TICK_INVALID)
    {
        ts += drift;
        if (unlikely(ts < VLC_TICK_0))
            return VLC_EGENERIC;
    }
    if (point->length != VLC_TICK_INVALID && !point->live)
    {
        pos += drift / (double) point->length;
        if (unlikely(pos < 0.f))
            return VLC_EGENERIC;
        if (pos > 1.f)
            pos = 1.f;
        if (ts > point->length)
            ts = point->length;
    }

    if (out_ts)
        *out_ts = ts;
    if (out_pos)
        *out_pos = pos;

    return VLC_SUCCESS;
}

vlc_tick_t
vlc_player_timer_point_GetNextIntervalDate(const struct vlc_player_timer_point *point,
                                           vlc_tick_t system_now,
                                           vlc_tick_t interpolated_ts,
                                           vlc_tick_t next_interval)
{
    assert(point);
    assert(system_now > 0);
    assert(next_interval != VLC_TICK_INVALID);

    const unsigned ts_interval = interpolated_ts / next_interval;
    const vlc_tick_t ts_next_interval =
        ts_interval * next_interval + next_interval;

    return (ts_next_interval - interpolated_ts) / point->rate + system_now;
}

void
vlc_player_InitTimer(vlc_player_t *player)
{
    vlc_mutex_init(&player->timer.lock);

    for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
    {
        vlc_list_init(&player->timer.sources[i].listeners);
        player->timer.sources[i].point.system_date = VLC_TICK_INVALID;
        player->timer.sources[i].es = NULL;
        player->timer.sources[i].seeking = false;
    }
    vlc_player_ResetTimer(player);
}

void
vlc_player_DestroyTimer(vlc_player_t *player)
{
    for (size_t i = 0; i < VLC_PLAYER_TIMER_TYPE_COUNT; ++i)
        assert(vlc_list_is_empty(&player->timer.sources[i].listeners));
}
