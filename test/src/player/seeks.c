// SPDX-License-Identifier: LGPL-2.1-or-later
/*****************************************************************************
 * seeks.c: seeks player test
 *****************************************************************************
 * Copyright (C) 2018-2025 VLC authors and VideoLAN
 *****************************************************************************/

#include "common.h"

static void
test_seeks(struct ctx *ctx)
{
    test_log("seeks\n");
    vlc_player_t *player = ctx->player;

    struct media_params params = DEFAULT_MEDIA_PARAMS(VLC_TICK_FROM_SEC(10));
    player_set_current_mock_media(ctx, "media1", &params, false);

    /* only the last one will be taken into account before start */
    vlc_player_SetTimeFast(player, 0);
    vlc_player_SetTimeFast(player, VLC_TICK_FROM_SEC(100));
    vlc_player_SetTimeFast(player, 10);

    vlc_tick_t seek_time = VLC_TICK_FROM_SEC(5);
    vlc_player_SetTimeFast(player, seek_time);
    player_start(ctx);

    {
        vec_on_position_changed *vec = &ctx->report.on_position_changed;
        while (vec->size == 0)
            vlc_player_CondWait(player, &ctx->wait);

        assert(VEC_LAST(vec).time >= seek_time);
        assert_position(ctx, &VEC_LAST(vec));

        vlc_tick_t last_time = VEC_LAST(vec).time;

        vlc_tick_t jump_time = -VLC_TICK_FROM_SEC(2);
        vlc_player_JumpTime(player, jump_time);

        while (VEC_LAST(vec).time >= last_time)
            vlc_player_CondWait(player, &ctx->wait);

        assert(VEC_LAST(vec).time >= last_time + jump_time);
        assert_position(ctx, &VEC_LAST(vec));

        vlc_player_Pause(player);
        wait_state(ctx, VLC_PLAYER_STATE_PAUSED);

        const size_t report_count = vec->size;
        const vlc_tick_t paused_seek_time = VLC_TICK_FROM_SEC(8);
        vlc_player_SetTime(player, paused_seek_time);

        while (vec->size == report_count ||
               VEC_LAST(vec).time < paused_seek_time)
            vlc_player_CondWait(player, &ctx->wait);

        assert_position(ctx, &VEC_LAST(vec));
        assert_state(ctx, VLC_PLAYER_STATE_PAUSED);

        /* A paused seek may preroll a frame, but must not resume playback. */
        vlc_player_Unlock(player);
        vlc_tick_sleep(VLC_TICK_FROM_MS(100));
        vlc_player_Lock(player);
        assert_state(ctx, VLC_PLAYER_STATE_PAUSED);

        /* Stale seek preroll requests must not prevent an explicit next-frame
         * burst after the seek has completed. */
        vec_on_next_frame_status *status_vec =
            &ctx->report.on_next_frame_status;
        const size_t status_count = status_vec->size;
        vlc_player_NextVideoFrame(player);
        vlc_player_NextVideoFrame(player);
        while (status_vec->size < status_count + 2)
            vlc_player_CondWait(player, &ctx->wait);
        assert(status_vec->data[status_count] == 0);
        assert(status_vec->data[status_count + 1] == 0);
        assert_state(ctx, VLC_PLAYER_STATE_PAUSED);
    }

    vlc_player_SetPosition(player, 2.0f);

    test_prestop(ctx);

    wait_state(ctx, VLC_PLAYER_STATE_STOPPED);
    assert_normal_state(ctx);

    test_end(ctx);
}

int
main(void)
{
    struct ctx ctx;
    ctx_init(&ctx, 0);
    test_seeks(&ctx);
    ctx_destroy(&ctx);
    return 0;
}
