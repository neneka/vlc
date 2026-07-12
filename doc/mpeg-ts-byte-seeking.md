# MPEG-TS byte position and seeking

## 目的

non-fastseek MPEG-TSでは、PCRを探索して時刻へ着地するのではなく、ファイル内のbyte位置を使ってシークする。
一方、demuxのread headはデコーダや出力より先まで読み進むため、`vlc_stream_Tell()`を現在の再生位置として使わない。

この実装では、外部へ返すpositionと実際のシーク先をbyte比率に統一し、timestampは「表示中のESと元のbyte位置を対応付けるため」にのみ使用する。

fastseek可能なMPEG-TSについては、既存のPCR/time seek経路を変更しない。

## positionの生成

1. TS packetを読み込んだ時点で、packet先頭のstream offsetとstream全体サイズを`block_t`へ記録する。
2. PESの変換・分割後も、そのPESが開始したbyte offsetをES blockへ引き継ぐ。
3. `es_out`はESのPTS/DTSと`byte offset / stream size`の対応をリングへ保存する。
4. output clockのtimestampに最も近いES pointを検索し、現在表示中のbyte positionとしてplayerへ通知する。
5. player timerはこのpositionを保持し、時間によるposition補間を行わない。

これにより、表示positionはdemuxの先読み量やPCR時刻ではなく、現在提示されているESに対応するbyte位置になる。

## position指定シーク

non-fastseek MPEG-TSの`DEMUX_SET_POSITION`は、次のbyte offsetへ直接シークする。

```text
target_byte = stream_size * target_position
```

シーク後にPCRを探索して着地点を補正しない。

## 時刻指定シーク

VLCKitのjump APIは最終的に時刻指定シークへ到達する。そのまま`DEMUX_SET_TIME`へ渡すと、non-fastseek TSの従来fallbackは`vlc_stream_Tell()`を現在位置として使用するため、先読み量がシーク結果へ混入する。

このためplayerは、non-fastseek MPEG-TSの時刻指定シークをposition指定シークへ変換する。

```text
current_time     = 現在表示中のmedia timestamp
current_position = 現在表示中のbyte position
time_delta       = target_time - current_time

target_position = current_position + time_delta * position_rate
```

変換後は`INPUT_CONTROL_SET_POSITION`を送り、timerのseek stateにはtarget timeとtarget positionの両方を設定する。これによりbuffering中も目標positionを維持できる。

## 表示byte履歴

player timerは、表示済みの`(timestamp, byte position)`を最大2048点保持する。シークやclock discontinuity、best ES sourceの切り替え時には履歴を破棄する。

計算では直近30秒以内のpointだけを有効とする。

### 後方シーク

target timeが30秒履歴内にある場合、targetを挟む2点間でbyte positionを補間する。

```text
target_position = lower.position
                + (upper.position - lower.position) * time_fraction
```

片側のpointしかない場合は、targetとの時刻差が1秒以内の場合のみ使用する。

15秒戻しのような短い後方シークでは、bitrate推定よりこの直接lookupを優先する。

### 局所delta

履歴から直接lookupできない前方シークなどでは、直近10秒のpointに対する線形回帰からposition delta/secondを求める。

局所deltaは次の条件を満たした場合に使用する。

- 5秒以上のtimestamp spanがある
- 8点以上の有効なpointがある
- 算出結果が正の値である
- targetが現在位置から前後30秒以内である

5秒時点では全体平均を使用し、履歴が10秒へ近づくにつれて局所deltaの比率を増やす。

```text
weight = clamp((history_span - 5sec) / 5sec, 0, 1)
seek_rate = global_rate + (local_rate - global_rate) * weight
```

異常なtimestampやbyte mappingの影響を抑えるため、local rateはglobal rateの0.25倍から4倍へ制限する。

## fallback

局所履歴を利用できない場合は、ProbeEndで確定した全体長から次のglobal rateを使用する。

```text
global_rate = 1 / duration
```

表示履歴がある場合は、現在表示中のbyte positionを基準にglobal rateを適用する。表示履歴がまだない場合は、absolute target timeをdurationで割ってtarget positionを求める。

次の場合はglobal rateを優先する。

- 再生開始直後
- シークまたはdiscontinuity直後
- 履歴が5秒未満または8点未満
- 30秒を超える時刻移動
- 局所deltaが計算不能

## 時間窓の選択

局所deltaの標準窓は10秒とする。

- 5秒未満ではGOPや一時的なmux変動の影響を受けやすい。
- 10秒程度あれば短周期の変動を平滑化しつつ、VBR区間の変化へ追従できる。
- 30秒をそのまま平均へ使うと、広告や番組区間などのbitrate変化への追従が遅くなる。

履歴自体は30秒保持し、短い後方シークの直接lookupへ利用する。局所rateの算出には、そのうち直近10秒だけを使う。

## 制約

- timestampはbyte mappingの検索キーとして使用するが、PCR探索による着地補正は行わない。
- 前方のVBR変化は過去の履歴だけでは予測できないため、局所deltaは30秒以内に限定する。
- TS packetの多重化順序やB-frameによりbyte positionが短時間だけ非単調になる可能性がある。線形回帰と時間窓によって影響を平滑化する。
- `STREAM_CAN_FASTSEEK`がtrueのソースは既存経路を使用し、このbyte変換の対象外とする。
