/**
 * AgPB - 目标游戏编译期开关。
 *
 * 三个目标互斥，未指定时默认当前项目：
 *   AGPB_GAME_CSCZS  CS:CZS（当前项目）
 *   AGPB_GAME_CSS    Counter-Strike: Source
 *   AGPB_GAME_CSGO   Counter-Strike: Global Offensive
 *
 * CS:CZS 是基于 CS:S 改的，构建仍用 css SDK（`-s css`），命令不用改；
 * 不传时默认 AGPB_GAME_CSCZS。只有真去编 css / csgo 变体时才需要显式覆盖。
 *
 * 规则（AGENTS.md）：游戏间差异**只认用户指出的**，在实现点用这些宏显式分支，
 * 注释标 [game-diff]；允许编译期硬编码，不要为了区分游戏去读内存 / 做运行期探测。
 * 构建时用 /DAGPB_GAME_CSS=1（或 /DAGPB_GAME_CSGO=1）覆盖默认值。
 */

#ifndef _INCLUDE_AGPB_GAME_TARGET_H_
#define _INCLUDE_AGPB_GAME_TARGET_H_

#if !defined( AGPB_GAME_CSCZS ) && !defined( AGPB_GAME_CSS ) && !defined( AGPB_GAME_CSGO )
	#define AGPB_GAME_CSCZS 1
#endif

#if ( defined( AGPB_GAME_CSCZS ) + defined( AGPB_GAME_CSS ) + defined( AGPB_GAME_CSGO ) ) != 1
	#error "AgPB: define exactly one of AGPB_GAME_CSCZS / AGPB_GAME_CSS / AGPB_GAME_CSGO"
#endif

#endif // _INCLUDE_AGPB_GAME_TARGET_H_
