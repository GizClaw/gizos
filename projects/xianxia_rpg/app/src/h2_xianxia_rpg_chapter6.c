/* Chapter 6: warm basalt, cold straight ice corridors and a shared spring.
 * '=' stops a slide; 'i' continues it. 'h' adds heat on completed steps.
 * 'S' cools, 'F' warms, 'm' is the route's medicine, 'R' the healing spring. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_maps[3][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"###############", "#hhh...R...hhh#", "#hhh.......hhh#", "#.............#",
     "===============", "#.............#", "#..SS.....FF..#", "#.............#",
     "#.............#", "###############"},
    {"###############", "#~~~#####~~~~~#", "#~~~#####...m.#", "#..S##S####S..#",
     "===hh==h======#", "#.=~~~~~~~..=.#", "#.===========.#", "#~~~~~~~~~~~~~#",
     "#~~~~~~~~~~~~~#", "###############"},
    {"###########F###", "#####=iiiii=###", "#####i#####im=#", "#####i#####i#i#",
     "==iii=#####i#i#", "###########i#i#", "###########=i=#", "###########F###",
     "###############", "###############"},
};
static const char* const k_names[] = {"百草寒谷·药庐", "温泉·火苔石滩",
                                      "冻溪·冰魄回廊"};
static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "照水药铺的火已远了。江篱靠在青崖肩上，掌心灼热，呼吸却结成白霜。山谷一半冒着暖雾，一半凝着青冰。"),
    SAY(HERO,
        "柳婆婆说，她的师妹荀七娘住在这里。江篱，再撑一程，我们已经到药庐了。"),
    SAY(JIANGLI,
        "灯簿上……为什么会有我的名字？青崖，先别替我许愿。我还想自己撑船回去。"),
    SAY(NARRATOR,
        "点药庐中央的荀七娘问诊。西去温泉，东去冻溪；两条路都能求药，不必两边都走。"),
};
static const h2_xianxia_rpg_line_t k_brief[] = {
    SAY(XUN,
        "柳师姐托你们来的？把灯簿放下，让我先看人。她不是得了寒病，是旧阵把冷暖两股息都牵到了心口。"),
    SAY(XUN,
        "药引有两味可选：西边温泉的火苔，东边冻溪的冰魄。有其中一味，我就能借泉水调成冷、暖两剂，救治用药不会耗尽。"),
    SAY(XUN,
        "温泉蒸汽每走一格添两点热，到了八就由药绳接回泉台。点蓝色泉台降回四。蒸汽道短，下方灰白石路长却安全。"),
    SAY(XUN,
        "冻溪只能沿上下左右滑，灰白止滑石才停。每滑一程降一点温；点金色篝火回到四。到零也会被药绳接回，不会丢药。"),
    SAY(JIANGLI,
        "外面越热，我心里越冷。别硬撑着替我挡……我们都稳住，再去碰药引。"),
    SAY(XUN,
        "温度二到六才能采药。下方可选温泉或冻溪；在路上随时点“回药庐”，能换路、歇息。没有时限，等着不会变冷。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "两路任选其一即可救治。温度四最稳，二到六可采药。红色蒸汽格每步加二，蓝冰每次滑行减一；零或八会安全退回最近泉台或火堆。"),
    SAY(NARRATOR,
        "温泉：点蓝泉降温存档。蒸汽近路需中途降温，下方灰白石路可绕开蒸汽。火苔在右上。冻溪：点同排或同列的一格滑行，碰石停下，不能中途转弯。"),
    SAY(NARRATOR,
        "冻溪从入口向右、向上、向右，到上方火堆回暖；再向下、向右、向上，取右上冰魄。下方火堆也能回暖。每次停滑都存档。"),
    SAY(XUN,
        "取得一味便可回庐，点上方双色泉眼救人。也可补走另一条路，双药引会结成温玉，仪式的稳区从三至五扩大到二至六。"),
    SAY(XUN,
        "仪式先看下一息：热息选凉药，冷息选暖药；泉灵来扰就驱灵。护心可停一息却不增加稳息，五次连续稳息才算完成。失衡会退回泉边，药引保留。"),
};
static const h2_xianxia_rpg_line_t k_hot[] = {
    SAY(NARRATOR,
        "温泉像埋在雪里的晚霞。红色蒸汽从石缝冒出，蓝色泉台之间有一条近路；下方灰白石路绕过全部热息。"),
    SAY(JIANGLI,
        "这里好暖……可我胸口更冷了。你每穿一格蒸汽都在发热，别连闯。先点泉台，药引在右上。"),
    SAY(NARRATOR,
        "路线与入口已记录。可先走近路到中间泉台降温，也可走下方石路。点“回药庐”随时安全撤回。"),
};
static const h2_xianxia_rpg_line_t k_cold[] = {
    SAY(NARRATOR,
        "冰下的草影仍在摇。冻溪像一面折起的镜，灰白止滑石嵌在蓝冰尽头，金色火堆隔水相望。"),
    SAY(JIANGLI,
        "这里越冷，心口的灯越烫。你脚下不能随意拐弯了……点同排或同列的一格，滑到止滑石再换方向。"),
    SAY(NARRATOR,
        "先右、上、右，到顶端火堆回暖，再下、右、上。每程降一点温，停滑后记录。下方火堆可补暖；右上冰魄旁也有止滑石。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(NARRATOR,
        "药火与泉水把温度调回四，气血灵力恢复。这里成为新的安全检查点，药引与所走路线都已记录。"),
};
static const h2_xianxia_rpg_line_t k_fire_moss[] = {
    SAY(NARRATOR,
        "火苔不长在最烫的石缝，反而伏在冷泉边。青崖稳住呼吸，摘下那点像余烬的红。火苔已收好。"),
    SAY(JIANGLI, "原来不是越亮越好。灯也好，人也好，总要留一点能喘息的地方。"),
    SAY(NARRATOR,
        "药引与落点已记录。点“回药庐”，再点上方双色泉眼便可救治；也能补走冻溪取得温玉。"),
};
static const h2_xianxia_rpg_line_t k_ice_soul[] = {
    SAY(NARRATOR,
        "冰魄里封着一片还绿的草叶。青崖掌心回暖，冰壳才肯松开；他没有用剑去砸。冰魄已收好。"),
    SAY(JIANGLI,
        "它没有死，只是等一个不伤它的人。你看，我也还醒着……别把我当成一盏快灭的灯。"),
    SAY(NARRATOR,
        "药引与落点已记录。点“回药庐”，再点上方双色泉眼便可救治；也能补走温泉取得温玉。"),
};
static const h2_xianxia_rpg_line_t k_need_balance[] = {
    SAY(NARRATOR,
        "手心的冷暖不稳，药引缩回石缝。温度须在二到六：温泉点蓝泉降温，冻溪点篝火回暖。不会耗掉药引。"),
};
static const h2_xianxia_rpg_line_t k_retreat[] = {
    SAY(NARRATOR,
        "寒热逼近极限，荀七娘的药绳轻轻一收。两人回到最近安全处，温度与气血恢复，药引不丢，已记录新的尝试。"),
    SAY(XUN, "救人不是比谁先倒下。看清冷暖，再走。也可以回药庐换一条路。"),
};
static const h2_xianxia_rpg_line_t k_ready[] = {
    SAY(XUN,
        "药引已够。你取来的药借双色泉水分成凉药、暖药，尽管用，不动你的回春丹。江篱，我听着你的呼吸。"),
    SAY(JIANGLI, "我也听着。青崖，等我说冷，再给我暖；别只顾着把灯推亮。"),
    SAY(XUN,
        "每一步先看来势：热息加二，用凉药减二；冷息减二，用暖药加二。泉灵来扰时点驱灵，便不受冷暖推动。"),
    SAY(XUN,
        "连续五次留在稳区便成功。护心能挡住这一息，但不增加次数；碰到零或八，就先停下来重试。每一步都记录，等待不计时。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {
    SAY(XUN,
        "先停。她的脉没有断，只是我们的步子乱了。药引还在，泉眼也还在，重来便是。"),
    SAY(NARRATOR,
        "本次仪式已安全退出，温度恢复为四，稳息重新计算。点双色泉眼再试；先看冷、热或泉灵提示，再选下方指令。检查点已记录。"),
};
static const h2_xianxia_rpg_line_t k_recovered[] = {
    SAY(NARRATOR,
        "第五息落下，泉水终于不再翻滚。江篱自己握住了提灯，指尖的霜一点点化开。"),
    SAY(JIANGLI, "这次……我听见的是水声，不是有人在心里扯绳子。我还在，青崖。"),
    SAY(XUN,
        "命保住了。可这不是病根：她心口是“以剑系灯”，把将散的命系进了灯阵。她是活的节点，阵一失衡，便由她受着。"),
    SAY(XUN,
        "这种系法，我只在竹溪沈家见过。十年前该有人用它救过她；谁动的手，仅凭脉象，我不能替你下断语。"),
    SAY(JIANGLI,
        "我记得一点了。雾夜，一个持剑的人说：别怕，替我看着这盏灯……后来我就回到渡船上。"),
    SAY(HERO,
        "十年前……也是爹没有回来的那一年。若真是沈家的手，我更不能让你一个人受着。"),
    SAY(JIANGLI,
        "先别替谁认罪，也别替我决定一生。我想知道原来的灯阵是什么样，再自己选往后怎么活。"),
    SAY(NARRATOR,
        "江篱康复，灯纹“泉”记入灯簿，往后合击可积四盏默契。康复检查点已记录。再问荀七娘，商量去处。"),
};
static const h2_xianxia_rpg_line_t k_end[] = {
    SAY(XUN,
        "救下她，不等于解开系灯。要解结，先得知道绳本来怎么走。镜湖底下照着两岸立阵时的旧影，去那里找原来的路。"),
    SAY(NARRATOR,
        "药炉旁压着闻舟留下的信：去镜湖看看，当年是谁先失约。他替两人探过路，字仍写得像划伤纸的一剑。"),
    SAY(JIANGLI,
        "旧城里你先背起闻婆，才去问簿上的字。我记得。到了镜湖，也别急着替任何一岸答话。"),
    SAY(HERO, "我们一起看。为你的活路，也为问清沈家留下的这道结。"),
    SAY(NARRATOR,
        "荀七娘把药绳系在灯柄上。江篱走出药庐，第一次自己踏向谷外。灯火安稳了，真正的问心才要开始。"),
};
static const h2_xianxia_rpg_line_t k_both[] = {
    SAY(XUN,
        "火苔与冰魄相遇，结成一枚温玉。带着它，救治稳区从三至五拓宽为二至六；这是多走一路的余地，不是必须付的代价。"),
    SAY(NARRATOR,
        "温玉：救治稳区扩大。两条寻药经历已记录，未来辨认寒热阵室时仍可回想。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),        SCRIPT(k_brief, C6_BRIEF),
    SCRIPT(k_help, NONE),         SCRIPT(k_hot, NONE),
    SCRIPT(k_cold, NONE),         SCRIPT(k_rest, NONE),
    SCRIPT(k_fire_moss, NONE),    SCRIPT(k_ice_soul, NONE),
    SCRIPT(k_need_balance, NONE), SCRIPT(k_retreat, NONE),
    SCRIPT(k_ready, C6_RITUAL),   SCRIPT(k_retry, NONE),
    SCRIPT(k_recovered, NONE),    SCRIPT(k_end, ENDING),
    SCRIPT(k_both, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 6,
    .first_stage = H2_XIANXIA_RPG_STAGE_C6_ARRIVE,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C6_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_HERB_HUT,
    .map_count = 3,
    .start_map = H2_XIANXIA_RPG_MAP_HERB_HUT,
    .rest_map = H2_XIANXIA_RPG_MAP_HERB_HUT,
    .start_x = 7,
    .start_y = 5,
    .rest_x = 7,
    .rest_y = 5,
    .default_level = 9,
    .default_pills = 3,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C6_INTRO,
    .label = "第六章",
    .name = "第六章 · 百草寒谷",
    .title = "百草寒谷",
    .ending = {"冷暖归心，江篱终于脱险。", "以剑系灯，沈家旧法待问。"},
    .next = "第六章 完 · 下一章《镜湖问心》",
    .recap =
        "照水旧城救下闻婆，两册灯簿揭出改阵证据。江篱的名字在“系灯”一行闪现，她随即倒下。大夫指向百草寒谷的荀七娘。",
};
char h2_xianxia_rpg_c6_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (map < H2_XIANXIA_RPG_MAP_HERB_HUT ||
      map > H2_XIANXIA_RPG_MAP_FROZEN_CREEK || x < 0 ||
      x >= H2_XIANXIA_RPG_MAP_COLS || y < 0 || y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_HERB_HUT][y][x];
}
const char* h2_xianxia_rpg_c6_map_name(h2_xianxia_rpg_map_t map) {
  return map >= H2_XIANXIA_RPG_MAP_HERB_HUT &&
                 map <= H2_XIANXIA_RPG_MAP_FROZEN_CREEK
             ? k_names[map - H2_XIANXIA_RPG_MAP_HERB_HUT]
             : "";
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c6_info(void) {
  return &k_info;
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c6_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C6_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_C7_FIRST
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C6_FIRST]
             : NULL;
}
size_t h2_xianxia_rpg_c6_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  (void)game;
  if (map != H2_XIANXIA_RPG_MAP_HERB_HUT) return 0;
  out[0] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_XUN, 7, 3,
                                  H2_XIANXIA_RPG_FACING_DOWN};
  return 1;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c6_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  (void)actor;
  if (game->stage == H2_XIANXIA_RPG_STAGE_C6_ARRIVE)
    return H2_XIANXIA_RPG_SCRIPT_C6_BRIEF;
  if (game->stage >= H2_XIANXIA_RPG_STAGE_C6_RECOVERED)
    return H2_XIANXIA_RPG_SCRIPT_C6_END;
  return H2_XIANXIA_RPG_SCRIPT_C6_HELP;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c6_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile) {
  if (tile == 'S' || tile == 'F') return H2_XIANXIA_RPG_SCRIPT_C6_REST;
  if (tile == 'R')
    return game->stage == H2_XIANXIA_RPG_STAGE_C6_READY
               ? H2_XIANXIA_RPG_SCRIPT_C6_READY
               : H2_XIANXIA_RPG_SCRIPT_C6_HELP;
  if (tile == 'm')
    return game->map == H2_XIANXIA_RPG_MAP_HOT_SPRING
               ? H2_XIANXIA_RPG_SCRIPT_C6_FIRE_MOSS
               : H2_XIANXIA_RPG_SCRIPT_C6_ICE_SOUL;
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}
size_t h2_xianxia_rpg_c6_strings(const char** out, size_t capacity) {
  size_t count = 0;
#define ADD(s)                            \
  do {                                    \
    if (count < capacity) out[count] = s; \
    ++count;                              \
  } while (0)
  for (size_t i = 0; i < ARRAY_SIZE(k_names); ++i) ADD(k_names[i]);
  for (size_t i = 0; i < ARRAY_SIZE(k_scripts); ++i)
    for (size_t j = 0; j < k_scripts[i].count; ++j)
      ADD(k_scripts[i].lines[j].text);
  ADD(k_info.label);
  ADD(k_info.name);
  ADD(k_info.title);
  ADD(k_info.ending[0]);
  ADD(k_info.ending[1]);
  ADD(k_info.next);
  ADD(k_info.recap);
#undef ADD
  return count;
}
