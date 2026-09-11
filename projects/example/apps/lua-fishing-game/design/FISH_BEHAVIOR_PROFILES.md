# 30 种海鱼的后台行为参数

状态：第一版设计标定，全部是游戏参数，不是实测生物学常数。已接入实时鱼行为决策；动作完成时才重新抽样，小鱼体型门槛与冲刺冷却均参与筛选。

每种鱼独立配置：长冲刺体重门槛（kg，0 表示禁用）；爆发力、耐力、转向、甩头、下潜、跃水、依赖掩体、恢复速度、警惕性、挂钩保持性（0–100）；冲刺持续时间与冷却（秒）。挂钩保持性只是基础解剖倾向，实际脱钩还取决于钩型、挂钩位置、松线和冲击。

概率在每次行为决策时计算，先应用体型、体力与冷却硬门槛，再按鱼种特征与地形调权，最后归一化。连续重复动作降权。小鱼没有持续冲刺状态，仍可短促加速；无体力时优先喘息。潜深与距离通过世界鱼群与饵位置决定遭遇，不靠按钮触发随机换种。

| 鱼种 | 冲刺门槛 kg | 爆发 | 耐力 | 转向 | 甩头 | 下潜 | 跃水 | 掩体 | 恢复 | 警惕 | 挂钩保持 | 冲刺秒 | 冷却秒 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| MARBLED ROCKFISH | 0 | 22 | 18 | 35 | 80 | 65 | 0 | 95 | 40 | 30 | 65 | 0 | 5 |
| BLACK SEA BREAM | 1.2 | 48 | 55 | 62 | 55 | 60 | 0 | 55 | 45 | 75 | 70 | 1.8 | 6 |
| YELLOWFIN SEABREAM | 1 | 45 | 48 | 68 | 50 | 55 | 0 | 45 | 48 | 72 | 65 | 1.5 | 6 |
| LARGESCALE BLACKFISH | 1 | 55 | 65 | 68 | 50 | 65 | 0 | 65 | 48 | 85 | 65 | 1.8 | 7 |
| DUSKY RABBITFISH | 0 | 35 | 40 | 72 | 45 | 40 | 0 | 65 | 50 | 65 | 55 | 0 | 5 |
| GRASS PUFFER | 0 | 12 | 15 | 25 | 25 | 20 | 0 | 30 | 35 | 25 | 75 | 0 | 5 |
| SPOTTED SEA BASS | 2 | 72 | 50 | 85 | 85 | 30 | 55 | 55 | 58 | 70 | 55 | 2.5 | 6 |
| LEOPARD CORAL TROUT | 1.5 | 76 | 48 | 45 | 65 | 95 | 0 | 98 | 35 | 60 | 78 | 1.4 | 8 |
| GREAT BARRACUDA | 2 | 92 | 55 | 65 | 60 | 15 | 20 | 10 | 50 | 60 | 62 | 3.5 | 7 |
| SPANISH MACKEREL | 3 | 95 | 75 | 60 | 45 | 35 | 10 | 5 | 55 | 45 | 55 | 4.2 | 7 |
| GIANT TREVALLY | 5 | 98 | 92 | 75 | 50 | 55 | 10 | 35 | 55 | 55 | 85 | 5.5 | 9 |
| MAHI-MAHI | 4 | 85 | 70 | 90 | 75 | 15 | 95 | 0 | 62 | 40 | 60 | 3.6 | 6 |
| BARRAMUNDI | 3 | 78 | 65 | 75 | 82 | 45 | 45 | 65 | 52 | 65 | 65 | 2.8 | 7 |
| SILVER SILLAGO | 0 | 18 | 18 | 60 | 45 | 15 | 0 | 10 | 55 | 60 | 30 | 0 | 5 |
| FLATHEAD GREY MULLET | 1 | 65 | 70 | 80 | 40 | 20 | 45 | 5 | 60 | 85 | 35 | 2.0 | 6 |
| SPOTTED SCAT | 0 | 25 | 35 | 70 | 45 | 40 | 0 | 60 | 50 | 60 | 45 | 0 | 5 |
| RED SEA BREAM | 2 | 65 | 65 | 60 | 70 | 70 | 0 | 40 | 45 | 75 | 75 | 2.3 | 7 |
| MANGROVE JACK | 1.5 | 82 | 58 | 80 | 65 | 75 | 15 | 90 | 45 | 70 | 80 | 2.2 | 7 |
| RED EMPEROR | 2 | 70 | 70 | 55 | 60 | 75 | 0 | 60 | 40 | 60 | 75 | 2.4 | 7 |
| ORANGE-SPOTTED GROUPER | 2 | 72 | 50 | 38 | 62 | 92 | 0 | 95 | 35 | 55 | 75 | 1.5 | 8 |
| BROWN-MARBLED GROUPER | 3 | 78 | 58 | 35 | 70 | 98 | 0 | 98 | 32 | 60 | 85 | 1.6 | 9 |
| GIANT GROUPER | 10 | 95 | 80 | 22 | 85 | 100 | 0 | 100 | 25 | 45 | 95 | 2.3 | 12 |
| HONG KONG GROUPER | 1 | 65 | 42 | 45 | 62 | 88 | 0 | 95 | 38 | 65 | 70 | 1.2 | 7 |
| KELP GROUPER | 3 | 80 | 60 | 42 | 70 | 96 | 0 | 98 | 32 | 75 | 85 | 1.8 | 9 |
| MACKEREL SCAD | 0 | 40 | 35 | 80 | 35 | 25 | 0 | 0 | 65 | 40 | 35 | 0 | 5 |
| BLUEFIN TREVALLY | 2.5 | 88 | 80 | 90 | 55 | 45 | 10 | 25 | 58 | 75 | 75 | 4.0 | 7 |
| GREATER AMBERJACK | 5 | 92 | 90 | 60 | 45 | 90 | 0 | 30 | 50 | 55 | 80 | 4.8 | 9 |
| LARGEHEAD HAIRTAIL | 0.8 | 60 | 35 | 75 | 78 | 45 | 0 | 5 | 58 | 50 | 40 | 1.5 | 6 |
| YELLOWFIN TUNA | 10 | 96 | 100 | 58 | 35 | 82 | 5 | 0 | 55 | 50 | 90 | 6.5 | 10 |
| SKIPJACK TUNA | 3 | 85 | 85 | 72 | 45 | 60 | 5 | 0 | 65 | 45 | 70 | 3.8 | 7 |

这些属性是后台数据，不出现在 HUD，不形成绿区/平衡条。体型门槛是当前游戏规则，不代表该物种在现实中达到这个重量才可能冲刺。

## Wind and time feeding activity

These are tunable game coefficients, not measured biological probabilities. Each of the 30 species has a day, twilight or night activity profile (`Weather.fish_periods`, catalog order). Wind levels 0/1/2/3 multiply activity respectively by: day 0.8/1/1.15/0.65; twilight 0.75/1/1.3/0.75; night 0.85/1/1.1/0.6. Time smoothly interpolates night/day activity with dawn and dusk peaks at 06:00 and 18:00. Hairtail and rockfish use night profiles; grazers use day profiles.

For each existing scheduled encounter opportunity, habitat/diet eligibility is weighted by each species' time × wind activity. A single acceptance roll uses `1-exp(-0.8 * weighted_activity / baseline_abundance)`, then the species is sampled from those weighted candidates. This affects actual fish encounters and subsequent bite opportunities without frame-rate-dependent RNG, guaranteeing a bite, modifying fish mass, or interrupting an already committed bite. Wave widths/heights use 0.35/1/1.5/2.2 scales for the same four wind levels.
