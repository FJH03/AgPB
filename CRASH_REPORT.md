# 崩溃报告：freezetime 结束时的服务端崩溃

> 日期：2026-09-19
> **根因（已定性）**：`CCSPlayer::Radio()` 对"不是 `CCSBot` 的假客户端"做 `dynamic_cast` 后直接解引用 → 空指针崩溃
> 触发链：freezetime 结束 → `CCSGameRules::CheckFreezePeriodExpired()` → `pPlayer->Radio("radio.go")` → `dynamic_cast<CCSBot*>(this)` 得 NULL → 读 `m_profile`（偏移 `0x1E48`）💥
> 代码状态：**M2.5 原样**（曾经尝试过一版规避实现，已全部回滚，见 §9）
> 相关文档：`ARCHIVE.md` §3.3（入队/出生）、§6（踩过的坑）；`VERIFY.md` 阶段 A

---

## 1. 现象与复现

1. 服务器上有玩家（例如 CT），用 `agpb_add 2` 把 bot 加进**对立阵营**（T）
2. 因为双方都有玩家，游戏自动重新开始
3. **freezetime 结束的瞬间**，服务端崩溃（Access violation）

**两个关键特征**：

- 崩在 `server.dll` 内部，不在 `agpb_mm.dll`
- 只在 bot 成为**某支队伍里唯一/第一个活跃玩家**时发生；bot 与人类**同队**时不崩

---

## 2. 崩溃现场

```
[AgPB] created bot 'AgPB_01' (slot=2, team=2)
AgPB_01 已连接。
(30f8.2c1c): Access violation - code c0000005 (first chance)
server!SMGD_HasGrenade+0x6457f4:
00007fff`512a3b04 488b80481e0000  mov     rax,qword ptr [rax+1E48h] ds:00000000`00001e48=????????????????
```

**指令解读**：`rax = 0`，读 `[rax + 0x1E48]` —— 空 `this` 读成员。`0x1E48 = 7752`。

**调用栈**：

```
00 00000088`0f31d500 00007fff`512668e1  server!SMGD_HasGrenade+0x6457f4   ← 崩（Radio 内部）
01 00000088`0f31db00 00007fff`5127266f  server!SMGD_HasGrenade+0x6085d1
02 00000088`0f31dba0 00007fff`50fde54c  server!SMGD_HasGrenade+0x61435f
03 00000088`0f31dc50 00007fff`50fb832c  server!SMGD_HasGrenade+0x38023c
04 00000088`0f31dd50 00007fff`50f7ee33  server!SMGD_HasGrenade+0x35a01c
05 00000088`0f31dda0 00007fff`6c7d19f5  server!SMGD_HasGrenade+0x320b23   ← RetAddr 在 agpb_mm.dll
*** WARNING: Unable to verify checksum for ...\addons\AgPB\bin\win64\agpb_mm.dll
```

`ln 0x7fff`6c7d19f5`：

```
[E:\Plugins-Platform\AgPB\src\plugin.cpp @ 24] (00007fff`6c7d1900)   agpb_mm!__SourceHook_FHCls_IServerGameDLLGameFrame0::Func+0xf5
   |  (00007fff`6c7d1a80)   agpb_mm!__SourceHook_FHCls_IServerGameClientsClientDisconnect0::Func
```

**关键读法**：`0x6c7d19f5` 落在 thunk `Func`（起始 `6c7d1900`）**内部**（下一个符号在 `Func+0x180`）。
也就是说 **frame 05 是被 thunk 调用的"原 `server!GameFrame`"**，此时我们的 **post** handler
（`SH_ADD_HOOK_MEMFUNC(..., true)`，`sourcehook.h:749` 的参数名是 `post`）**还没轮到执行**。

```
thunk: __SourceHook_FHCls_IServerGameDLLGameFrame0::Func
 └ 原 server!IServerGameDLL::GameFrame                ← frame 05
    └ 引擎 GameFrame 内部                              ← frame 04/03
       └ CCSGameRules::Think                           ← frame 02
          └ CCSGameRules::CheckFreezePeriodExpired()   ← frame 01
             └ CCSPlayer::Radio( "radio.go" )
                └ pBot->GetProfile()  →  NULL + 0x1E48 💥   ← frame 00
```

5 层 server 帧与这条链完全吻合。

> **注意**：`SMGD_HasGrenade` 只是符号表里地址最高的 `SMGD_*` 别名
> （= `CCSBot::HasGrenade()`，`cs_bot_weapon.cpp:555`）。所有 `SMGD_*` 都挤在
> `0x50c2101e ~ 0x50c5e310` 这 250KB 里（全是 `CCSBot` 的方法），崩溃地址在它之后 6.5MB ——
> **完全在符号区之外**，所以栈上那 5 个 `+0xXXXXXX` **不能**用来判断"崩在 bot 代码里"。

---

## 3. 根因

### 3.1 缺陷代码

```cpp
// cs_player.cpp:4186
void CCSPlayer::Radio( const char *pszRadioSound, const char *pszRadioText )
{
        ...
        ConstructRadioFilter( filter );                       // 4153 附近
        ...
        UserMessageBegin ( filter, "SendAudio" );             // 4242：这行在 if 之前，本来就发给含假客户端的 filter
        WRITE_STRING( pFinalSound );
        MessageEnd();

        if ( IsBot() ) {                                      // 4246：FL_FAKECLIENT → 我们的假客户端 = true
                CCSBot *pBot = dynamic_cast< CCSBot* >( this );        // 4247：不是 CCSBot → NULL
                pBot->SpeakAudio( pFinalSound, 1.5f,
                                  pBot->GetProfile()->GetVoicePitch() ); // 4248：💥 空指针
        } else {
                TE_RadioIcon( filter, 0.0, this );
        }
}
```

- `CCSBot : public CBot< CCSPlayer >`（`cs_bot.h:418`），即 IS-A `CCSPlayer`，所以这条 `dynamic_cast` 合法
- `GetProfile()` 是内联成员访问（读 `m_profile`）→ 编译出来就是 `mov rax,[rax+0x1E48]`
- **`0x1E48` 是否正好是 `CCSBot::m_profile` 的偏移，是最值得确认的一点**（见 §7.1）

### 3.2 触发者：freezetime 结束时的自动无线电

```cpp
// cs_gamerules.cpp:3235
void CCSGameRules::CheckFreezePeriodExpired()
{
        ...
        m_bFreezePeriod = false;                              // 3291
        ...
        IGameEvent *event = gameeventmanager->CreateEvent( "round_freeze_end" );
        if ( event ) gameeventmanager->FireEvent( event );     // 3293

        // Update the timers for all clients and play a sound
        bool bCTPlayed = false, bTPlayed = false;
        for ( int i = 1; i <= gpGlobals->maxClients; i++ )
        {
                CCSPlayer *pPlayer = CCSPlayer::Instance( i );
                if ( pPlayer && !FNullEnt( pPlayer->edict() ) )
                {
                        if ( pPlayer->State_Get() == STATE_ACTIVE )            // 只看"在游戏中"的玩家
                        {
                                if ( pPlayer->GetTeamNumber() == TEAM_CT && !bCTPlayed )
                                { pPlayer->Radio( CT_sentence ); bCTPlayed = true; }   // 3312
                                else if ( pPlayer->GetTeamNumber() == TEAM_TERRORIST && !bTPlayed )
                                { pPlayer->Radio( T_sentence );  bTPlayed = true; }    // 3317
                        }
                }
        }
}
```

**要点**：

- **没有 `IsBot()` 过滤**，只有"该队第一个 `STATE_ACTIVE` 的玩家"
- 它的**唯一**调用点是 `cs_gamerules.cpp:3074`（在 `CCSGameRules::Think()` 里）→ 属于引擎 `GameFrame` 内部
- 喊哪句话由回合场景决定（`cs_gamerules.cpp:3258-3287`）：`radio.moveout` / `radio.letsgo` /
  `radio.locknload` / `radio.go` / `radio.elim` / `radio.vip` …，`radio.go` 是最常见的一种

### 3.3 为什么偏偏是"加入敌对阵营"才崩

`bCTPlayed` / `bTPlayed` 保证**每队只挑第一个活跃玩家**：

| 场景 | 谁被选中 | 结果 |
|---|---|---|
| bot 与人类**同队** | 遍历到的人类通常在前（`CCSPlayer::Instance(i)` 按 slot） | 人类 `IsBot()==false` → 走 `else` ✅ 安全，且标志随即置位，bot 被跳过 |
| bot **独占**一支队伍 | bot 自己 | `IsBot()==true` 且不是 `CCSBot` → 💥 |

这也解释了"为什么以前 M1/M2/M2.5 的测试都没崩"—— 那些场景里 bot 要么单独在 CT（人类不在场，
游戏没真正开始），要么和人类同队。

---

## 4. 证据链小结

| # | 事实 | 依据 |
|---|---|---|
| 1 | 崩溃是空指针解引用（`rax=0` 读 `+0x1E48`） | 崩溃指令 |
| 2 | 崩溃发生在**引擎 `GameFrame` 执行期间**，不是我们的 post handler 里 | frame 05 的返回地址落在 `Func` thunk **内部**（无其它符号），说明它调用的是原 `GameFrame` |
| 3 | 5 层 server 帧与 `GameFrame → Think → CheckFreezePeriodExpired → Radio → GetProfile` 完全吻合 | §2 / §3.2 |
| 4 | 触发条件是"每队第一个 `STATE_ACTIVE` 玩家" | `cs_gamerules.cpp:3303-3325` 的 `bCTPlayed/bTPlayed` |
| 5 | `Radio()` 的 `IsBot()` 分支对该返回值**没有 NULL 检查** | `cs_player.cpp:4246-4248` |
| 6 | Valve 自己知道这条假设容易踩 | `player.h:731` 注释：`IsBot()` 对**任何** bot 都返回 true，**cast 前必须检查具体 bot 类型** |

---

## 5. 已排除的其它假设

| 假设 | 排除依据 |
|---|---|
| 假客户端没有 `CCSBot` 对象（一般意义上） | `cs_player.cpp` 里 `GetBot(` / `CCSBotManager` / `m_pBot` **零命中**；CS:S 的 bot 由 `CCSBotManager` 独立管理，`CCSPlayer` 不持有它。**但 `Radio()` 是个例外**（它自己 dynamic_cast） |
| `MoveHelperServer()->SetHost()` 未清理 | `player.cpp:9263` 结尾有 `SetHost( NULL )` |
| 栈上 `CBotCmd` 悬垂 / 未初始化 | 官方示例 `utils/serverplugin_sample/serverplugin_bot.cpp:345` 同样用栈变量；`CBotCmd::Reset()` 会清零全部字段；`CPlayerInfo::RunPlayerMove` 只做字段拷贝 |
| 每 tick 无条件驱动 `RunPlayerMove` 是错的 | 官方示例同样无条件驱动（第 380 行在 `if` 之外） |
| 崩在 bot AI 代码区 | 由 `SMGD_*` 符号分布可知那是符号表末尾效应（见 §2 注） |
| 崩在我们驱动的 `RunPlayerMove` 链上 | 曾按此分析，后经 `ln` 结果修正：frame 05 是被 **thunk** 调用的原 `GameFrame`，而非被 `CAgPB::Think` 调用 |

---

## 6. 修复选项

### 6.1 拦 `CCSPlayer::Radio()`（最直接）

`Radio()`（`cs_player.h:446`）、`HandleMenu_Radio1/2/3` **都不是虚函数**，SourceHook 的
vtable hook 用不上，需要拿到入口地址。**从这次崩溃现场就能反推**：

```
0:000> lm m server                        // 记下 server.dll 基址
0:000> ub 0x7fff`512a3b04 L60             // 往前找函数序言 → Radio 入口
0:000> u  0x7fff`512668e1-0x80 0x7fff`512668e1
                                          // frame 01 的返回地址附近必有 call <Radio> 指令
```

拿到入口 RVA 后（入口 − server 基址），用 manual hook / 自写 detour 拦掉，对
"`IsBot()` 为 true 但不是 `CCSBot`"的实体直接返回即可。

**风险**：RVA 会随 server.dll 版本变化 —— 建议同时做一段特征码校验，或在
`Radio()` 入口处校验前若干字节。

### 6.2 hook `CCSGameRules::Think()`（更稳的替代）

`virtual void Think();`（`cs_gamerules.h:180`）是**虚函数**，可以 vtable hook，不需要地址。
在 pre 阶段把 bot 的 `State`（`m_iPlayerState`，netvar 可读写）暂时挪出 `STATE_ACTIVE`，
post 阶段恢复 → `CheckFreezePeriodExpired()` 的循环就会跳过它。

**代价**：需要正确的 vtable index；一帧内 bot 状态异常（`STATE_ACTIVE` 之外的状态会影响
状态机相关的逻辑）。

### 6.3 在 `round_freeze_end` 事件里"伪装成真人"（已试过，已回滚）

利用 `cs_gamerules.cpp:3293` 的 `FireEvent("round_freeze_end")` **先于** Radio 循环这一点，
在事件监听器里临时摘掉 `FL_FAKECLIENT`（写 `m_fFlags` 的 netvar），约 3 tick 后恢复。
`Radio()` 会走 `else` 分支。

**回滚原因**：按作者决定，先把根因定性清楚再谈规避。实现要点留档：

- `FL_FAKECLIENT = (1<<9)`（`const.h:163`，CS:S 走 `#else` 分支；`const.h:121` 的 `1<<8` 是 PORTAL/HL2 剧情游戏分支）
- 需要给 `netvars.h` 加一个写 int 的辅助（`NetVar_SetInt`）
- 必须加自检：若 `m_fFlags` 里没有 `FL_FAKECLIENT` 位，说明编译分支与 server.dll 不一致，应放弃伪装
- 副作用：窗口内引擎把它当网络客户端（`IsNetClient()` 转真，而 `FL_FAKECLIENT` 的注释正是
  "don't send network messages to them"）

### 6.4 架构层面：别用"无 CCSBot 的假客户端"

`CPluginBotManager` 这条路线在 CS:S 上会持续撞到"`IsBot()` ⇒ 就是 `CCSBot`"这类假设
（`Radio()` 只是第一个撞上的）。M3 之前值得评估：改用 `CCSBotManager` 创建**真** `CCSBot`，
再把它的 AI 输入接管掉。

---

## 7. 验证与待确认

### 7.1 待确认：`0x1E48` vs `CCSBot::m_profile`

`m_profile` 的偏移未在源码中直接可数（取决于 `CBot<CCSPlayer>` / `CBasePlayer` 的完整布局）。
两种确认办法：

1. `agpb_netlist 0` 打印 `CCSPlayer` 全部 268 个字段名 + 偏移，搜 `+7752` —— 若命中，
   说明它是网络字段（但 `m_profile` 是纯服务端成员，**大概率不在表里**，命中反而要重新解释）
2. WinDbg 里反汇编崩溃点附近，看是否有 `call` 到 `SMGD_*` 中的 `SpeakAudio` 相关符号，
   或直接看 `rcx`（`this`）是否为我们的 bot 实体

### 7.2 修复后的验证方式

| 场景 | 期望 |
|---|---|
| bot + 人类**同队** | 本来就不崩（对照） |
| bot **独占**一队，freezetime 结束 | **修复后不再崩**；队伍照常收到 `radio.go` 之类的语音 |
| 换图 / 多回合连续跑 | 每个回合的 freezetime 结束都不崩 |

如果修复后仍崩，优先看是否还有**其它**"`IsBot()` ⇒ `CCSBot`"式的假设被踩到。

---

## 8. 源码位置索引

**崩溃路径**

| 位置 | 内容 |
|---|---|
| `cs_gamerules.cpp:3074` | `CheckFreezePeriodExpired()` 的**唯一**调用点（在 `CCSGameRules::Think()` 内） |
| `cs_gamerules.cpp:3235` | `void CCSGameRules::CheckFreezePeriodExpired()` |
| `cs_gamerules.cpp:3291` | `m_bFreezePeriod = false` |
| `cs_gamerules.cpp:3293` | `FireEvent( "round_freeze_end" )` |
| `cs_gamerules.cpp:3303-3325` | 遍历玩家 → 每队第一个 `STATE_ACTIVE` → `pPlayer->Radio(...)` |
| `cs_gamerules.cpp:3258-3287` | 各场景对应的 radio 语句（`radio.moveout` / `radio.go` / …） |
| `cs_player.cpp:4186` | `CCSPlayer::Radio( const char *pszRadioSound, const char *pszRadioText )` |
| `cs_player.cpp:4246-4248` | 💥 `if (IsBot()) { CBSBot *pBot = dynamic_cast<CCSBot*>(this); pBot->SpeakAudio(...); }` |
| `cs_bot.h:418` | `class CCSBot : public CBot< CCSPlayer >` |
| `player.h:731` | Valve 注释：`IsBot()` 对任何 bot 都返回 true，cast 前须检查具体类型 |

**插件侧**

| 位置 | 内容 |
|---|---|
| `src/plugin.cpp:24` | `SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, ...)`（栈 frame 05 的符号来源） |
| `src/plugin.cpp:513` | `SH_ADD_HOOK_MEMFUNC(..., Hook_GameFrame, true)` —— **true = post** |
| `metamod-source/core/sourcehook.h:749` | `SH_ADD_HOOK_MEMFUNC(..., post)` 宏定义 |
| `player.cpp:9224` | `CPlayerInfo::RunPlayerMove`（内部 `if (m_pParent->IsBot())`，决定了假客户端必须保留 `FL_FAKECLIENT`） |
| `const.h:163` | `FL_FAKECLIENT (1<<9)`（CS:S 分支） |
| `cbase.h:90` | `SMGD_EXPORT_ALIAS` = `#pragma comment(linker, "/export:...")`，`SMGD_*` 符号的来源 |
| `cs_bot_weapon.cpp:557` | `SMGD_HasGrenade`（符号表里地址最高的 `SMGD_*`，栈上全是它的名义偏移） |
| `utils/serverplugin_sample/serverplugin_bot.cpp` | Valve 官方插件 bot 参考实现（`Bot_Think`，343-381 行） |

---

## 9. 当前代码状态

- **仓库已回到 M2.5 原样**：为候选 B 写过的一版规避实现（`netvars.h` 加 `NetVar_SetInt`、
  `bot.*` 加 `SetFakeClientDisguise`/`DisguiseAllAsRealPlayers`、`plugin.cpp` 加
  `FreezeEndListener`）**已由作者全部删除**，`grep` 确认零残留
- 因此 `agpb_enable` 是当前唯一的运行期开关（=0 时不驱动 ucmd）
- 部署提醒：`O:\...\cstrike\addons\AgPB\bin\win64\agpb_mm.dll` 在服务器运行时被占用，
  换 dll 需要先停服
