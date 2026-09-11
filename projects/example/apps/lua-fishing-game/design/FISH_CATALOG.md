# 海钓鱼形象与重量参考（30 种）

以下是用于第一版游戏设计的常见钓获体重**估计区间**，单位 kg。它们不是某个统一海域的渔获统计分位区间，也不是最大纪录体重。岸钓、船钓、季节、鱼龄与地区会显著影响分布，后续选定钓场后应再校准。

龙趸的 5–40 kg 是大型稀有目标鱼的设计档位，不能称为普遍常见钓获。GT、红甘、鬼头刀与金枪鱼按对应船钓/外海目标场景列出；这些重量范围不意味着鱼种常见。

| # | 中文名 | 游戏英文名 | 钓获体重参考 kg（估计） |
|---|---|---|---|
| 1 | 石九公（褐菖鲉） | MARBLED ROCKFISH | 0.05–0.5 |
| 2 | 黑鲷 | BLACK SEA BREAM | 0.3–2 |
| 3 | 黄鳍鲷 | YELLOWFIN SEABREAM | 0.2–1.5 |
| 4 | 黑毛（斑鱾） | LARGESCALE BLACKFISH | 0.3–1.5 |
| 5 | 臭肚（褐篮子鱼） | DUSKY RABBITFISH | 0.1–0.6 |
| 6 | 草河豚 | GRASS PUFFER | 0.03–0.25 |
| 7 | 海鲈（花鲈） | SPOTTED SEA BASS | 0.5–4 |
| 8 | 东星斑 | LEOPARD CORAL TROUT | 0.5–3 |
| 9 | 海狼（大鳞金梭鱼） | GREAT BARRACUDA | 0.5–5 |
| 10 | 康氏马鲛 | SPANISH MACKEREL | 1–8 |
| 11 | GT（浪人鲹） | GIANT TREVALLY | 2–15 |
| 12 | 鬼头刀（鲯鳅） | MAHI-MAHI | 2–12 |
| 13 | 尖吻鲈 | BARRAMUNDI | 1–6 |
| 14 | 沙梭（多鳞鱚） | SILVER SILLAGO | 0.05–0.25 |
| 15 | 乌头（鲻鱼） | FLATHEAD GREY MULLET | 0.3–1.5 |
| 16 | 金钱鱼 | SPOTTED SCAT | 0.1–0.5 |
| 17 | 真鲷 | RED SEA BREAM | 0.5–3 |
| 18 | 红友（紫红笛鲷） | MANGROVE JACK | 0.3–3 |
| 19 | 千年笛鲷 | RED EMPEROR | 0.5–4 |
| 20 | 青斑（点带石斑） | ORANGE-SPOTTED GROUPER | 0.5–4 |
| 21 | 老虎斑（棕点石斑） | BROWN-MARBLED GROUPER | 1–6 |
| 22 | 龙趸（鞍带石斑） | GIANT GROUPER | 5–40 |
| 23 | 红斑（赤点石斑） | HONG KONG GROUPER | 0.2–1.5 |
| 24 | 云纹石斑 | KELP GROUPER | 1–6 |
| 25 | 巴浪鱼（蓝圆鲹） | MACKEREL SCAD | 0.08–0.35 |
| 26 | 蓝鳍鲹 | BLUEFIN TREVALLY | 1–5 |
| 27 | 红甘（高体鰤） | GREATER AMBERJACK | 2–15 |
| 28 | 带鱼 | LARGEHEAD HAIRTAIL | 0.2–1.2 |
| 29 | 黄鳍金枪鱼 | YELLOWFIN TUNA | 5–40 |
| 30 | 鲣鱼 | SKIPJACK TUNA | 1–5 |

来源及使用边界：
- [黑鲷物种资料](https://www.fishbase.se/summary/6531)用于核对物种尺度；该页最大体重不是常见钓获区间。
- [FAO 太平洋金枪鱼渔业资料](https://www.fao.org/4/t1817e/t1817e05.htm)说明黄鳍金枪鱼的大小分布会随渔业而明显变化。
- [FAO 石斑鱼鉴别资料](https://www.fao.org/4/x2400e/x2400e37.pdf)用于龙趸形态与不同生长阶段参考。
- [台湾鱼类名录](https://fishdb.sinica.edu.tw/chi/fisheconomic.php?R1=family_c&dere=desc&key=&page=1&pz=500)用于物种名核对。

这些来源不直接证明表中全部上下限；表中数值是明确标注的初步设计估计。代码的 `reference_min_kg/reference_max_kg` 用于生成偏向小个体的游戏重量；实际重量参与惯性、水阻与推力尺度，不直接作为悬挂重力施加给鱼竿。

所有鱼形均为 Lua 多边形/线条绘制。三张 `fish-atlas` 页面覆盖 30 种，各图缩放用于辨识而非跨物种等比例。鱼袋为第四页（RODS → REELS → LURES → BAG），支持纵向滚动。`fish-bag-preview` 使用示例数据，界面不显示预览说明；缩略图只显示鱼形，长重仅在 info 区显示；正式鱼袋显示本次运行中捕获的独立鱼获；重启后清空，尚未接入持久化。


## 游戏价格

价格单位为金币，属于虚构游戏经济数值，不对应现实海鲜市场价格。单条总价 = 重量（kg）× 鱼种单价（金币/kg），四舍五入到整数。鱼袋 info 区第三列以金币图标显示总价。

| 鱼种英文名 | 单价（金币/kg） | 示例总价（金币） |
|---|---:|---:|
| MARBLED ROCKFISH | 60 | 19 |
| BLACK SEA BREAM | 48 | 53 |
| YELLOWFIN SEABREAM | 52 | 34 |
| LARGESCALE BLACKFISH | 44 | 37 |
| DUSKY RABBITFISH | 32 | 11 |
| GRASS PUFFER | 12 | 2 |
| SPOTTED SEA BASS | 38 | 91 |
| LEOPARD CORAL TROUT | 180 | 288 |
| GREAT BARRACUDA | 24 | 86 |
| SPANISH MACKEREL | 30 | 93 |
| GIANT TREVALLY | 42 | 260 |
| MAHI-MAHI | 28 | 151 |
| BARRAMUNDI | 46 | 152 |
| SILVER SILLAGO | 56 | 7 |
| FLATHEAD GREY MULLET | 20 | 16 |
| SPOTTED SCAT | 26 | 7 |
| RED SEA BREAM | 76 | 114 |
| MANGROVE JACK | 68 | 88 |
| RED EMPEROR | 88 | 79 |
| ORANGE-SPOTTED GROUPER | 96 | 173 |
| BROWN-MARBLED GROUPER | 120 | 336 |
| GIANT GROUPER | 110 | 4180 |
| HONG KONG GROUPER | 140 | 91 |
| KELP GROUPER | 100 | 330 |
| MACKEREL SCAD | 10 | 2 |
| BLUEFIN TREVALLY | 50 | 130 |
| GREATER AMBERJACK | 36 | 198 |
| LARGEHEAD HAIRTAIL | 40 | 32 |
| YELLOWFIN TUNA | 80 | 1760 |
| SKIPJACK TUNA | 22 | 70 |
