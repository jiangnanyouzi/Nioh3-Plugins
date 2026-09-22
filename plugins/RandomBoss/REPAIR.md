# RandomBoss 更新后修复手册

**这份文档是什么**：游戏更新后，RandomBoss 某个功能不工作时，照着它定位并修好。
**不是什么**：逆向过程记录。推导过程和证据链在 `analysis/purple_variant_re_2026-09-22.md`。

维护者如果只想改代码，看 §3 的补丁清单 + §6 的现场验证就够了。
如果签名失效需要重新找站点，看 §5。

---

## 1. 基线

| 项 | 值 |
| --- | --- |
| 游戏版本 | Nioh 3 **v2.0.2.0** |
| 模块大小 | `87,011,328` 字节 |
| 基址 | 每次启动不同；本手册写作期间一直是 `0x7FF70CF10000` |
| **RVA 计算** | **`RVA = VA − 模块基址`**，没有例外 |
| 构建 | `cmake --build build --target RandomBoss --config Release` |
| 产物 | `build/bin/Release/RandomBoss.dll` |
| 安装 | 复制到 `<游戏>\plugins\RandomBoss.dll`（**游戏运行时会占用，必须先关游戏**） |
| 日志 | `<游戏>\logs\RandomBoss.log`（**每次启动清空**） |
| 崩溃日志 | `<游戏>\logs\NIOH3PluginLoader.log`（看 `ExceptionCode` / `ExceptionAddress`） |
| 配置 | `<游戏>\plugins\RandomBoss.ini`（模板：`RandomBoss.template.ini`） |

> ⚠️ **CMake 的 post-build 拷贝在游戏运行时必然失败**（`MSB3073`）。
> 那时 DLL 已经编译好了，只是没装 —— 关掉游戏再手动复制一次即可。

---

## 2. 文件地图

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 安装循环、三个钩子的 body、换怪写记录、`DeriveRecordOffsets()`、启动日志 |
| `src/patterns.h` | **所有特征码与偏移**，顶部有实测命中数表 |
| `src/purple.cpp` | 紫皮相关：`ApplyTargetFlags` / `MapPurpleMark` / 四个补丁函数 |
| `src/maps.cpp` | 放置记录的扫描与改写、`MapRecordFlagsLookLikePlacement`、池选择 |
| `src/config.cpp` | ini 读取、热重载 |
| `src/state.h` / `src/core.h` / `src/config.h` | 全局量声明、常量、限值 |
| `HookStub.asm` | 三个钩子的汇编 trampoline |
| `common/src/HookUtils.cpp` | `ScanIDAPattern` / `CountIDAPatternMatches`（**所有插件共用，改动要谨慎**） |

---

## 3. 补丁清单（唯一的"地图"）

全部为**内存补丁**：不写磁盘、不动存档，**游戏退出即自动恢复**。
写入前都校验目标字节，校验不过就打日志并拒绝。

### 3.1 代码补丁

| # | 功能 | ini 开关 | RVA | 原字节 | 改成 | 定位常量 | 校验 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 记录不再是"一次性空壳"⇒ **能重生** | `MapPurple` | —（**不是代码补丁**） | `g_targetFlags = 0x011E3701` | `0x001E3701` | `ApplyTargetFlags()` —— **见 §3.4** | L1 形状校验 |
| 2 | spawn plain 门 A（`+0xE9`） | `MapForceEmpower` | `0x54FB6B` | `74 3A` | `90 90` | `kRevivePlainBranchPattern + 7` | 首字节 `74` |
| 3 | spawn plain 门 A（已击杀查表） | `MapForceEmpower` | `0x54FB7D` | `75 21` | `90 90` | `kRevivePlainBranchPattern + 25` | 首字节 `75` |
| 4 | spawn plain 门 B（已击杀查表） | `MapForceEmpower` | `0x54FC24` | `75 21` | `90 90` | `kRevivePlainBranch2Pattern + 36` | 首字节 `75` |
| 5 | **击杀期 MARK PLAIN** | `MapForceEmpower` | `0x2A225A` | `01` | `00` | `kKillPlainMarkPattern + 11` | 前 6 字节 `C6 87 EA 00 00 00` |
| 6 | 被禁用的放置点能出场 | `MapIgnoreBlocked` | `0x133F6E` | `0F 8D xx xx xx xx` | `90 ×6` | `kBlockedPlacementPattern + 9` | `0F 8D` |
| ~~—~~ | ~~渲染门~~ **已删除** | ~~`MapPurpleRender`~~ | ~~`0x9A2188`~~ | ~~`74 E8`~~ | — | **2026-09-22 实测证伪**，键也已从代码和 ini 删除 | — |

**5 号是"紫皮 + 打死能重生"的决定性补丁。** 它把 `mov byte [rdi+0xEA],1` 的立即数改成 0，
所以既能让击杀不再把怪标成普通，也能**主动修回**已经被标成普通的实体。
（选"写 0"而不是"NOP 整条 store"，就是为了这个"能修回"的性质。）

### 3.2 钩子

| 钩子 | RVA | 安装条件 | 作用 |
| --- | --- | --- | --- |
| map placement key | `0x669895`（resume = +7） | `MapHook=1` **且** `MapBoss=1` | 源级换怪；**并在此处推导记录布局** |
| purple flag writer | `0x27DB78`（pattern `0x27DB6A` + 14） | `MapPurple=1` **且 `MapForceEmpower=0`** | 每帧写 `entity+0xE9` |
| factory entry | `0x5FC570` | `FactoryDiag=1` **或**（`MapPurple=1` 且 `MapForceEmpower=0`） | 全覆盖实体的 `+0xE9` 标记 |

> 后两个钩子在 `MapForceEmpower=1`（默认用法）时**故意不装**：
> `MapForceEmpower` 把读 `+0xE9` 的那道门 NOP 了，标记改变不了任何东西，
> 而 purple flag writer 是**每帧、每个活动实体**都会跑一次。

### 3.3 数据布局（**签名保护不到这些**）

**放置记录**（由 `44 8B 70 04` 的 disp8 推导，见 §5.4）：

| 偏移 | 含义 |
| --- | --- |
| `+0x00` | instanceId（u32，非 0） |
| `+0x04` | key（u32）← 换怪写这里 |
| `+0x08` | flags（u32）：低字节 `0x01`，第二字节 ∈ `{0x36,0x37,0x3B}`；**bit24 = 一次性**；bit16 固定清 |

**实体**：

| 偏移 | 含义 |
| --- | --- |
| `+0x00` | 绑定的放置对象指针 |
| `+0x20` | instanceId |
| `+0x28` | key |
| `+0xE8` | dword，低字节是 `+0xE8`，bit8 是 `+0xE9` |
| `+0xE9` | "强化候补"标记（引擎的门读它；**不是外观开关**） |
| **`+0xEA`** | **外观开关：`0` = 紫皮，`1` = 强制普通** |

**放置对象**（`entity+0x00` 指向）：

| 偏移 | 含义 |
| --- | --- |
| `+0x0C0` | 组件块（**`0` = 空壳，即不会重生的那种**） |
| `+0x0F8` | 记录指针 |
| `+0x8D4` | 状态枚举，实测 `0 / 2 / 3` |
| `+0xAE14` | 启用字节 |

> ⚠️ **对象在换图时会重建**，地址和 instanceId 都会变。**写之前必须重新枚举。**

### 3.4 `ApplyTargetFlags()` —— "能重生"就是这个函数干的（**改动前必读**）

`src/purple.cpp`。**7 个代码补丁都不负责"能不能重生"**，负责的是它。
出问题时它比任何补丁都值得先看。

```cpp
void ApplyTargetFlags(std::uintptr_t record) {
  if (!g_mapPurple) return;                              // 关着就不动
  flagsOffset = g_recordFlagsOffset;                     // 推导值，默认 8
  flagWord    = record + flagsOffset;
  current     = *flagWord;
  if (!MapRecordFlagsLookLikePlacement(current)) return; // ← L1 形状校验
  desired = g_targetFlags;                               // 默认 0x001E3701
  updated = (current & 0x0000FFFF) | (desired & 0xFFFF0000);
  if (updated == current) return;                        // ← 见下面"2026-09-22 修"
  *flagWord = updated;
  // 每 32 次打一行 `purple record flags %p: X -> Y`
}
```

**它做什么**：把 flags 的**高 16 位**换成 `g_targetFlags` 的高 16 位（即 **bit24 清 0、bit16 清 0**），
**低 16 位原样保留** —— 低 16 位是放置记录的"族标签"（实测有 `3701 / 3601 / 3B01` 三种），
扫描的形状校验依赖它，不能动。

**为什么重要**：记录 `+0x08` 的 **bit24 = 1 ⇒ 引擎把放置点建成"一次性空壳"**
（`对象+0x0C0 == 0`），打死之后**再也不出现**。清掉 bit24 才能重生。
这条是实测的，38 个对象零例外（见 `analysis/` §34）。

**三个调用点**（都要动到，少一个就有记录漏掉）：

| 调用点 | 位置 | 覆盖的是 |
| --- | --- | --- |
| 扫描·"已是目标 key"分支 | `maps.cpp`（`IsMapTargetKey(f[1])` 为真） | **引擎自己放的**目标怪放置点 |
| 扫描·换怪分支 | `maps.cpp`（写完 key 之后） | 插件换过的放置点 |
| 钩子·`finish()` | `main.cpp`（`MapBossHookBody` 的出口 lambda） | 扫描之后才动态生成的放置点 |

> 这三条是历史教训：第二条分支**曾经直接 `continue`**，于是"引擎自己放的、
> 且带普通变体位"的记录永远拿不到修正 —— 实测 314 条记录扫过两整遍还剩 2 条没改。

**2026-09-22 修掉的一个判据错误**：原来的提前返回写的是 `current == desired`，
但真正写下去的 `updated` 保留了记录自己的低 16 位。于是**族标签不是 `3701` 的记录**
（`1E3B01` 这种）永远满足 `current != desired`，每次扫描都用同样的值重写一遍。
日志里表现为 `1E3B01 -> 1E3B01` 三行。**无害但是假信号** —— 看起来"改了 3 条"，
实际一条都没变。现在判据是 `updated == current`。

**排查时看什么**：

| 日志 | 含义 |
| --- | --- |
| `purple record flags %p: X -> Y`（上限 32 行） | 真的改了。`X` 和 `Y` **必须不同** |
| `native target placement fixed ... (engine-authored ordinary variant)`（上限 16 行） | 引擎自己放的目标怪被修正 |
| `REFUSED to write record ...`（上限 8 行） | L1 拦下；布局可能变了 |
| `X -> X`（同一个值） | **不该再出现**；出现说明判据又被改回去了 |
| **一行都没有** | `MapPurple=0`，或所有记录本来就已是目标值 |

---

## 4. 出问题时：按日志分流

### 4.1 正常启动应该正好是这 7 行

```
record layout derived from the binary: key +0x4, flags +0x8 (matches the v2.0.2.0 measurement)
map placement hook installed at 00007FF7xxxxxxxx (source-level swap, resume 00007FF7xxxxxxxx)
MapForceEmpower: NOPed BOTH plain branches at ... (RVA 54FB6B ...) and ... (RVA 54FB7D ...)
MapForceEmpower: NOPed the THIRD plain gate at ... (RVA 54FC24 ...)
MapForceEmpower: kill-time mark-plain store neutralised at ... (RVA 2A225A ...)
MapIgnoreBlocked: blocked-placement jump NOPed at ... (RVA 133F6E ...)
all hooks online (attempt 1)
```

（`purple flag hook` / `factory entry hook` / `ichi-nan render gate` **不应该出现** ——
前两个在当前配置下故意不装，第三个的功能已删除。）

### 4.2 分流表

| 日志现象 | 病因 | 处理 |
| --- | --- | --- |
| `... pattern not found (will retry)` | 签名漂移：那段代码被改动了 | §5 重新定位该站点，更新特征码 |
| `... signature is NOT UNIQUE (2 or more matches)` | 通配把唯一性弄丢了（或游戏新增了同形状函数） | §5.3 收紧签名；**唯一性优先于抗漂移** |
| `... reads XX, expected 74/75 (je/jne) - NOT patched` | 签名找到了，但那不是条件跳转 | 号对错了站点，重新定位 |
| `... shape reads ... expected C6 87 EA 00 00 00` | 击杀期 store 的编码变了（不再用 disp32） | 重新读该处字节，改 `kKillPlainMarkPattern` |
| `record layout MOVED - derived key +0xX, flags +0xX` | 记录布局真的变了 | 这是**有意的告警**；核对 §3.3，扫描会因此停手（L3） |
| `REFUSED to write record ... not a placement flag word` | 记录形状校验拦下了写入（L1） | 多半是布局变了；若确认没变，检查 `MapRecordFlagsLookLikePlacement` 的白名单 |
| `blocked-placement pattern not found` | 同上，签名漂移 | §5 |
| 完全没有 `all hooks online` | ini 没读到 / 插件没加载 | 先查 `NIOH3PluginLoader.log` |
| `map rank ...` / `fep raw #...` / `ichi-nan render` | **不该出现**，说明装的是旧 DLL | 确认 `plugins\RandomBoss.dll` 的 MD5 |
| `purple record flags %p: X -> X`（同一个值） | 提前返回的判据退化成了 `current == desired` | 见 **§3.4** 末段；这是"看起来改了、其实没改"的假信号 |

### 4.3 按"游戏里的症状"分流

| 症状 | 先查什么 | 判据 |
| --- | --- | --- |
| 紫皮丢了（变普通） | 实体 `+0xEA` | `1` = 被标成普通。若补丁 5 已生效却又是 1 → 还有别的写入点，用 §5.2 抓 |
| 紫皮在但打死后不重生 | 记录 `+0x08` bit24；对象 `+0x0C0` | bit24=1 或 `+0x0C0==0` ⇒ 又是一次性空壳（§3.1 的 1 号） |
| 怪根本不出现 | 对象 `+0x8D4` / `+0xAE14` | `+0x8D4==3` 且 `+0xAE14==0` ⇒ 被禁用趟钉死（§3.1 的 6 号） |
| 换怪不生效 | 日志有没有 `map inst #N` / `map source swap` | 没有 ⇒ 池/源名单是空的，看 §4.2 的 WARNING |

> **热重载的坑**：改 ini 会热重载**数值**，但**不会重新打补丁**。
> 所有补丁都要**重启游戏**才生效。这个坑在本项目里真实发生过一次
> （改了 `MapPurpleRender` 却以为已经生效）。

---

## 5. 重新定位一个站点

### 5.1 通用流程

1. 用 Cheat Engine 附加 `Nioh3.exe`，记下模块基址。
2. AOB 扫描（`protection` 用 `+X`）。
3. 拿到 VA 后**立刻换算 RVA** 并在反汇编器里核对那个地址的字节 ——
   不要相信"VA 的低 32 位"。
4. 改 `patterns.h`：**所有 `rel32` / `disp32` / 短跳位移都写成 `?`**。
5. **数命中数**，确认唯一（见 §5.3）。命中 > 1 时 `ScanIDAPattern` 会取地址最低的那一处，
   **静默挂错函数**。
6. 重新编译、关游戏、装 DLL、开机看日志。

### 5.2 用硬件断点找"谁写了这个字段"

这是本项目最有效的手法，找到过两个关键站点。

```
set_data_breakpoint(地址 = 字段地址, access_type = "w", size = 1)
→ 在游戏里触发行为（打死怪 / 传送 / 拜神社）
→ get_breakpoint_hits(id)
```

命中里读 `registers.RDI` / `RSI` / `RBX` 判断对象身份，然后看 `RIP`：

> ⚠️ **CE 报的 `instruction` 是"访问指令的下一条"**（RIP = 访问指令 + 长度）。
> 所以真正的写入指令在 `RIP` **往前**一条。本项目靠这条规则抓到过 `0x2A2254`。

### 5.3 唯一性必须实测

```
aob_scan(pattern, protection="+X", limit=200)
```

**注意**：插件已经打过的补丁/钩子会改掉站点字节，导致原签名扫不到。
重新扫的时候要把**被改动的那些字节通配掉**，测的才是"周边上下文有几处匹配"。

**反例（真实）**：`kMapPlacementKeyPattern` 里的 `74 1A` **不能**通配 ——
通配成 `74 ?` 后从 1 处变 **3 处**。

⇒ 规则：**唯一性优先于抗漂移。**
签名失效 = 功能不激活 + 有日志（安全）；匹配到错地方 = 改错代码（不安全）。

### 5.4 记录布局怎么重新推导

`kMapPlacementKeyPattern` 就是 `44 8B 70 04` = `mov r14d,[rax+04]`：

```
偏移 0: 44        REX.R
偏移 1: 8B        mov r32, r/m32
偏移 2: 70        ModRM: mod=01(disp8), reg=110(r14d), rm=000(rax)
偏移 3: 04        disp8 = 0x04  ← key 的相对偏移就在这个字节
```

`DeriveRecordOffsets()` 读 `code[3]`，flags 取 `key + 4`。
**必须在装钩子之前读** —— 钩子会把前 7 个字节覆盖掉。

---

## 6. 现场验证（改完内存后怎么确认）

### 6.1 紫皮 + 重生（本插件的核心功能）

1. 枚举实体，找到目标怪（日志里的 `map inst #N ... key=A263C` 给出了 `rec=` 和 `ent=`）。
2. 读实体 `+0xE8..+0xEA`：
   ```
   01 01 00   ← E8=1 E9=1 EA=0 = 紫皮状态（正确）
   01 01 01   ← EA=1 = 被标成普通（错误）
   ```
3. 读记录 `+0x08`：应为 `001E3701`（bit24 清）。
4. 读对象 `+0x0C0`：应非空（不是空壳）；`+0x8D4` 应为 `0`。
5. **在游戏里打死它，然后拜神社让它重生** —— 应该还是紫皮。

> 单变量原则：一次只改一个字节，改完**读回确认**，再让玩家看结果。
> 本项目两次错误归因（§35 / §36）都是因为一次动了两个变量。

### 6.2 枚举实体 / 放置对象

```
AOBScan('C8 5B 3D 11 F7 7F 00 00')   // 对象 +0x08 的类指针
⇒ 对象基址 = 命中地址 − 8
```

---

## 7. 已知陷阱（都真实踩过）

| # | 陷阱 | 正确做法 |
| --- | --- | --- |
| 1 | **拿 VA 的低 32 位当 RVA** | 永远 `VA − 模块基址`，并用反汇编器核对字节 |
| 2 | CE 数据断点的 `instruction` 是**下一条** | 真正的访问指令在它前面一条 |
| 3 | `writeInteger(addr, val, 'dword')` **静默失败** | 用 `write_memory` 显式给字节 |
| 4 | AOB 扫描会**阻塞 CE 桥** | 一次只扫一个；先停掉后台守护线程 |
| 5 | 打过补丁的站点**原签名扫不到** | 重扫时把补丁字节通配掉 |
| 6 | 同一形状的函数序言**极可能不唯一** | 数命中数；工厂钩子的序言原本命中 **7** 处 |
| 7 | 通配位移**可能破坏唯一性** | 通配后**重新数** |
| 8 | 改 ini **只热重载数值，不会重打补丁** | 补丁一律**重启游戏** |
| 9 | 对象在换图时**重建** | 写之前重新枚举 |
| 10 | 结构体偏移**不受签名保护** | `ApplyTargetFlags` 有 L1 形状校验；改变布局要靠 `DeriveRecordOffsets` |
| 11 | 一次改两个变量 → **错误归因** | 一次一个变量；先读回确认再下结论 |

---

## 8. 更新后 15 分钟检查清单

1. [ ] 关游戏，备份旧的 `plugins\RandomBoss.dll`
2. [ ] 清空/记录 `logs\RandomBoss.log` 的 mtime
3. [ ] 启动游戏，看日志：
   - [ ] 7 行正常日志是否齐全
   - [ ] 有没有 `pattern not found` / `NOT UNIQUE` / `NOT patched` / `shape reads`
4. [ ] 进图找到目标怪（`key=A263C`）：
   - [ ] 外观：紫皮？
   - [ ] 打死 → 拜神社重生：还是紫皮？
5. [ ] 若有失败项，按 §4.2 分流表定位，按 §5 重新找站点
6. [ ] 修完重新编译 → 关游戏 → 装 DLL → 重跑第 3、4 步
7. [ ] 更新 `patterns.h` 顶部的**实测命中数表**

> 只要"签名找不到"就**拒绝打补丁**这个设计是对的：更新后最坏结果是
> **某个功能不激活 + 日志里有一行说明**，而不是把游戏改坏。
> 出问题时优先怀疑签名，不要怀疑补丁逻辑。
