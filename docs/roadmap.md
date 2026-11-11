# Roadmap

Current features and tested devices are documented in the [guides](README.md)
and [hardware reference](hardware.md). This roadmap lists feature proposals
and areas for broader support.

## Feature proposals

These additions are proposed work.

| Proposal | Remaining addition |
| --- | --- |
| Podcasts and audiobooks | Series/episode/chapter metadata, dedicated browsing, played state, automatic per-item resume, exclusion from Shuffle Songs, and spoken-audio speed control. |
| Lyrics | Host-prepared embedded/sidecar lyrics and a bounded Now Playing view. |
| Star ratings and desktop history access | 0–5 ratings, device rating controls, and desktop access to listening history, preserving stable track IDs. |
| Album shuffle | Random album order with disc/track order retained within each album, including repeat and resume. |
| Slideshow settings | Saved interval, repeat/shuffle, and optional music-playlist choices. |
| Sound Check compatibility | Import Apple `iTunNORM` values or consistently label normalization as ReplayGain. Consider host loudness analysis for untagged music. |
| Metadata and artwork editing | Single/batch tag overrides and cover replacement, with explicit file-tag write policy, asset invalidation, and track-identity preservation. |
| Smart playlists and larger saved lists | Host-evaluated playlist rules and a bounded design beyond 16 saved lists / 1,024 entries each. |
| Saved sync policies | Persistent folder/playlist selections that determine future syncs. |
| Desktop playback preview | Audio/video playback within Media Manager. |
| Video categories | Movie, show/season/episode, and music-video metadata, editing, and browsing. |

Implementation entry points: [music compiler](../tools/content/music.py),
[music schema](../services/music/database.h), [queue](../services/music/queue.h),
[history](../services/music/history_record.h),
[desktop](../tools/studio/media_desktop.py), and
[video catalog](../tools/content/video_catalog.py).

## Broader support

| Area | Remaining work |
| --- | --- |
| Utilities | Alarms with measured wake behavior, world clocks, stopwatch/laps, host-prepared contacts and calendar events, and a persisted combination screen lock. |
| Languages | Broader glyph coverage, translated menus, and matching input/search rules. |
| Accessories and TV output | Physically qualify M25 TV output; add TV output on other backends and support for additional dock/car/remote protocols, radio accessories, and recording. |
| Hardware variants | Extend support to additional boards, storage adapters, panels, and NOR chips. |
| Apple interoperability | Expand supported Apple media formats and folder/path handling. |
| Optional expansion | Assess local recommendations, proprietary stock features, protected purchases/Audible, and commercial iPod games separately. Consider macOS/Linux packaging and CD ripping. |
