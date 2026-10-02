# 崩溃报告：freezetime 结束时的服务端崩溃

> 日期：2026-09-19 ｜ 状态：根因已定性，**未修复**（2026-10-02 复核，规避实现仍零残留）。
> 测试约束：**bot 必须与人类同队**（见文末）。

## 现象与复现

1. 服务器上有玩家（例如 CT），`agpb_add 2` 把 bot 加进**对立阵营**（T）；
2. 双方都有玩家 → 游戏自动重新开始；
3. **freezetime 结束的瞬间**服务端崩溃（Access violation）。

两个特征：崩在 `server.dll` 内部（不在 `agpb_mm.dll`）；只在 bot 成为
**某队唯一 / 第一个活跃玩家**时发生，bot 与人类同队不崩。

## 根因

freezetime 结束 → `CCSGameRules::CheckFreezePeriodExpired()` → 每队第一个 `STATE_ACTIVE`
玩家喊一次无线电（`radio.go` 之类）→ `CCSPlayer::Radio()` 里：

```cpp
if (IsBot()) { CCSBot *pBot = dynamic_cast<CCSBot*>(this); pBot->SpeakAudio(...); }
```

假客户端 `IsBot()` 为 true，但它**不是 `CCSBot`** → cast 得 NULL → 读 `m_profile`
（偏移 `0x1E48`）→ 空指针崩溃。栈上出现的 `SMGD_*` 只是符号表里离崩溃点最近的导出，
不是真实调用链。

## 已排除的假设

- 不是我们驱动的 `RunPlayerMove` 链：frame 05 是被 thunk 调用的原 `GameFrame`。
- 不是武器 / 手雷逻辑：那些 `SMGD_*` 符号只是地址最近，不是真实函数。

## 已试过、已回滚

在 `round_freeze_end` 事件里临时摘掉 `FL_FAKECLIENT` 伪装真人：能绕过崩溃，但窗口期内
引擎会把 bot 当网络客户端，副作用不可控；作者决定先把根因定性清楚再谈规避，实现已全部删除。

## 修复选项

| 方案 | 说明 | 代价 |
|---|---|---|
| Hook `CCSPlayer::Radio()` | 非虚函数，需要入口地址 + 特征码校验，对 `IsBot() && !CCSBot` 直接返回 | RVA 随 server.dll 版本变化 |
| Hook `CCSGameRules::Think()` | 虚函数可 vtable hook；pre 把 bot 移出 `STATE_ACTIVE`，post 恢复 | 一帧内状态异常；vtable index 要确认 |
| 软件断点 / 内存补丁 | 走 SourceMod 那类内存补丁思路 | 需要引入外部依赖或自写补丁 |
| 架构层面改造 | 改用 `CCSBotManager` 建**真** `CCSBot` 再接管输入，绕开所有「`IsBot()` ⇒ 就是 `CCSBot`」的假设 | 改动最大，影响面广 |

## 测试约束（修复前必须遵守）

- bot 与人类**同队**，且队里人类 slot 更小、处于 `STATE_ACTIVE`；
- 人类断线 / 切观察者 / 换队之前，先 `agpb_kick all`；
- 换图 / 换场景前先 `agpb_kick all`。

## 修复后的验证

| 场景 | 期望 |
|---|---|
| bot + 人类同队 | 本来就不崩（对照） |
| bot 独占一队，freezetime 结束 | 不再崩，队伍照常收到无线电语音 |
| 连续多回合 / 换图 | 每回合 freezetime 结束都不崩 |

如果修复后仍崩，优先排查是否还有其它「`IsBot()` ⇒ `CCSBot`」式的引擎假设被踩到。
