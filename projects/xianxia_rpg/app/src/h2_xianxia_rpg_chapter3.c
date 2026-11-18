/* Chapter 3, 《雨夜剑阁》. All interaction is turn/step based: the rain is
 * atmosphere, never a timer. Posts share the saved flags word with local
 * chapter bits; the three learned stances and sword glyph are carried on. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define C3_MAPS 3

/* '.' wet stone, 's' steps, 'T' pine/cliff, '~' ravine, '#' masonry,
 * 'v' optional ambush ground, 'Q' shelter, 'n' letter, 'd' cut seal,
 * '1'/'2'/'3' clockwise posts, 'O' central stele, 'X' sword barrier,
 * 'a'/'b'/'c' teaching steles, 'L' resting lantern, 'R' roof, '_' floor. */
static const char* const k_maps[C3_MAPS][H2_XIANXIA_RPG_MAP_ROWS] = {
    {
        "TTTTTTTsTTTTTTT",
        "T.....ss..vv..T",
        "T.d...s.T.vv..T",
        "TTTTT.s.TTTTTTT",
        "T..ssss....vv.T",
        "T..s..TTT..vv.T",
        "T..n....s.....T",
        "T..Q...ss..~~~T",
        "T......s...~~~T",
        "TTTTTTTTTTTTTTT",
    },
    {
        "#######s#######",
        "#.............#",
        "#.......c.....#",
        "#######X#######",
        "#.a.........b.#",
        "#...1..O..3...#",
        "#.............#",
        "#.L....2......#",
        "#.............#",
        "#######s#######",
    },
    {
        "RRRRRRRRRRRRRRR",
        "#_____________#",
        "#_____________#",
        "#_____________#",
        "#_P_________P_#",
        "#_____________#",
        "#_P_________P_#",
        "#_L___________#",
        "#_____________#",
        "#######s#######",
    },
};
static const char* const k_names[C3_MAPS] = {
    "雨中山道",
    "废剑阵庭",
    "剑阁正殿",
};
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "西山雨下了一整夜。江篱把丙三号系在山脚，两人循着雨纹剑穗，踏上剑阁山道。"),
    SAY(JIANGLI,
        "偷船的剑客往这里来了。我要亲口问他，为什么把我娘的船扔进雾里。"),
    SAY(HERO,
        "穗上的三道雨纹，和路边旧剑印一样。先到西侧避雨亭，看看白鹤道长留下的信。"),
    SAY(NARRATOR,
        "沿石阶走可避开剑影。雨草丛里才会遇敌；不用练级，读懂残碑便能过关。"),
};
static const h2_xianxia_rpg_line_t k_pavilion[] = {
    SAY(NARRATOR,
        "亭檐挡住了冷雨，炉上还有热水。气血、灵力恢复。（已记录进度）"),
    SAY(JIANGLI,
        "亭北的信筒里有道长的字。看过再沿石阶往北，雨再大，也总有路。"),
};
static const h2_xianxia_rpg_line_t k_letter[] = {
    SAY(NARRATOR,
        "白鹤留书：青崖，持穗去问守阁人石砚。他虽目盲，却听得出每一柄剑的来历。"),
    SAY(NARRATOR,
        "昨夜锁阵骤断，剑魄失控。莫以蛮力争胜，阵庭三碑，记着听雨剑的起手与破法。"),
    SAY(HERO,
        "道长还画了剑桩：轻触一次，顺时针转一面。三柄剑尖都朝中碑，北面的剑阵才会退。"),
    SAY(JIANGLI, "看方向就能解，不用赶时间。先读两侧的碑，第三块藏在剑阵后。"),
};
static const h2_xianxia_rpg_line_t k_cut[] = {
    SAY(NARRATOR, "山门封符被人削成两半，断口没有焦痕，只有一道极薄的剑痕。"),
    SAY(HERO, "古祠封符也是这样断的。雨纹穗子和断符剑，终于接上了。"),
};
static const h2_xianxia_rpg_line_t k_cut_known[] = {
    SAY(JIANGLI, "和芦苇滩的断缆一样！芦叟见到的斗笠剑客，果然上了这座山。"),
    SAY(HERO,
        "他割的不只是船缆，还有古祠和剑阁的封印。他到底想让什么东西流出去？"),
};
static const h2_xianxia_rpg_line_t k_court[] = {
    SAY(NARRATOR,
        "废剑阵庭中，三根剑桩仍在吐出冷光。北面的剑阵挡住了上殿的石阶。"),
    SAY(JIANGLI,
        "两侧的碑还能读，中间那块刻着转桩的图。西南灯台能歇息，先把退路留好。"),
};
static const h2_xianxia_rpg_line_t k_center[] = {
    SAY(NARRATOR,
        "中碑：三剑归心。西桩向东，下桩向北，东桩向西。剑尖指向此碑，雨阵自息。"),
    SAY(JIANGLI,
        "轻触剑桩，每次顺时针转九十度。剑尖是亮的一端；三根都对了，通路会打开。"),
};
static const h2_xianxia_rpg_line_t k_barrier[] = {
    SAY(NARRATOR, "雨水凝成竖直的剑影，不能穿过。让三根剑桩的剑尖都指向中碑。"),
};
static const h2_xianxia_rpg_line_t k_open[] = {
    SAY(NARRATOR,
        "三道剑光汇进中碑。北面的剑阵熄灭了，后方露出刻着回旋剑纹的残碑。"),
    SAY(JIANGLI,
        "第三块碑出来了！读完再上正殿，就知道遇到回剑该怎么应对。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_stele1[] = {
    SAY(NARRATOR, "残碑·垂雨：垂袖，雨丝成线。三道剑雨连落，硬接会受三次伤。"),
    SAY(JIANGLI,
        "见它垂袖，点“防御”。你凝神回灵，我稳住身形；挡住三道雨，默契就攒满了。"),
    SAY(NARRATOR, "记下垂雨起手。战斗会提示“垂袖成线：防御”。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_stele2[] = {
    SAY(NARRATOR,
        "残碑·穿檐：剑收腰侧，雨聚一线。下一击直穿檐下，须以合击截断剑势。"),
    SAY(JIANGLI,
        "垂雨之后就是穿檐。上一式防住，默契已满；这时点“合击”，它就刺不出来。"),
    SAY(NARRATOR,
        "记下穿檐起手。合击仍耗七点灵力，前后防御可以回灵。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_stele3[] = {
    SAY(NARRATOR,
        "残碑·回风：横剑回身，衣摆逆雨。此式等人先攻，攻击和剑诀都会招来反击。"),
    SAY(JIANGLI,
        "见它横剑，先别出剑。点“防御”回灵，或用“丹药”补血。我只试探，不送上反击。"),
    SAY(HERO, "垂雨防，穿檐合，回风等。读它的起手，不跟着它乱了阵脚。"),
    SAY(NARRATOR, "记下回风起手。三式固定轮转，没有时间限制。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_hall[] = {
    SAY(NARRATOR,
        "正殿屋瓦裂开，冷雨直落在无人的剑架上。一缕剑魄站在殿中，偏殿门后有人敲杖。"),
    SAY(SHIYAN,
        "别近身！它叫听雨，本是护阁的剑魄，昨夜有人割断锁阵，它便认不出人了。"),
    SAY(JIANGLI, "先把西南灯台点亮。我们会在这里歇息，失手也从这里重来。"),
    SAY(NARRATOR, "气血、灵力恢复，灯台成为歇息点。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(NARRATOR, "避雨灯下，气血与灵力恢复。（已记录进度）"),
    SAY(JIANGLI,
        "三式都能慢慢看。防垂雨攒满默契，合击破穿檐，回风时回灵或服药。"),
};
static const h2_xianxia_rpg_line_t k_shiyan[] = {
    SAY(SHIYAN,
        "老夫石砚。剑影封着偏殿，先让听雨归鞘。阵庭的碑还在，它的规矩就在碑上。"),
    SAY(HERO, "我们带着青萍渡的雨纹剑穗而来。待剑魄平息，还请前辈辨认。"),
};
static const h2_xianxia_rpg_line_t k_boss[] = {
    SAY(NARRATOR,
        "剑魄缓缓垂袖。檐外的雨丝竟随它的剑尖，绷成了三条笔直的银线。"),
    SAY(JIANGLI, "别急着出剑。看清起手，再选指令；我会跟上你。"),
};
static const h2_xianxia_rpg_line_t k_quiet[] = {
    SAY(NARRATOR,
        "听雨垂下剑尖，化作一线清光归入剑架。雨势渐小，偏殿门上的剑影也散了。"),
    SAY(JIANGLI, "石前辈出来了。把穗子给他看看。"),
};
static const h2_xianxia_rpg_line_t k_reveal[] = {
    SAY(HERO, "前辈，这条雨纹剑穗，是我们在青萍渡的沉船里找到的。"),
    SAY(SHIYAN,
        "三道雨纹，末尾少一结……闻舟。这是闻舟的穗子，老夫亲手给他系的。"),
    SAY(SHIYAN,
        "他是照水人，三年前离了剑阁。昨夜那一剑，割的是锁阵，不是想取老夫的命。"),
    SAY(NARRATOR,
        "檐角忽然传来一声轻响。一个戴斗笠的青年立在瓦上，雨水沿剑鞘落下。"),
    SAY(WENZHOU, "灯本该照两岸。你们竹溪，点得太亮了。"),
    SAY(JIANGLI, "就是那顶斗笠！闻舟！丙三号是我娘留下的船，你为什么偷它？"),
    SAY(NARRATOR, "闻舟转身没入雨里。江篱追到檐下，山道却已被一道急雨遮住。"),
    SAY(HERO, "点得太亮……我们只是想救村子。前辈，他说的两岸，是怎么回事？"),
};
static const h2_xianxia_rpg_line_t k_wenzhou[] = {
    SAY(NARRATOR, "檐上只剩水痕。闻舟的那句话，却像一根细刺，扎进了雨声里。"),
};
static const h2_xianxia_rpg_line_t k_report[] = {
    SAY(SHIYAN,
        "这锁阵的断口修不回了。锁阵一断，上游的祭灯撑不了几夜。先回竹溪，重燃山脊三灯！"),
    SAY(NARRATOR,
        "石砚把剑纹拓在灯纸上，交给沈青崖。得到灯纹“剑”，残碑记下的三式也会随行。"),
    SAY(JIANGLI, "船的事，我还会问他。眼下先回你家；这一次，别一个人去守灯。"),
    SAY(HERO, "好。等竹溪平安了，我们再查清两岸的灯，究竟怎么了。"),
    SAY(NARRATOR, "山脚一盏溪灯忽然熄了。两人收起剑穗，连夜向竹溪赶去。"),
};
static const h2_xianxia_rpg_line_t k_revive[] = {
    SAY(NARRATOR,
        "你在避雨灯旁醒来，气血灵力恢复。剑桩与读碑记录都还在，听雨会从第一式重来。"),
    SAY(JIANGLI,
        "垂袖成线就防御，收剑聚雨就合击，横剑回身先等。丹药不够，也能用防御和合击慢慢赢。"),
};

static const h2_xianxia_rpg_script_t
    k_scripts[H2_XIANXIA_RPG_SCRIPT_C4_FIRST - H2_XIANXIA_RPG_SCRIPT_C3_FIRST] =
        {
#define AT(id) [H2_XIANXIA_RPG_SCRIPT_C3_##id - H2_XIANXIA_RPG_SCRIPT_C3_FIRST]
            AT(INTRO) = SCRIPT(k_intro, NONE),
            AT(PAVILION) = SCRIPT(k_pavilion, REST),
            AT(LETTER) = SCRIPT(k_letter, C3_BRIEF),
            AT(CUT) = SCRIPT(k_cut, NONE),
            AT(CUT_KNOWN) = SCRIPT(k_cut_known, NONE),
            AT(COURT) = SCRIPT(k_court, C3_BRIEF),
            AT(CENTER) = SCRIPT(k_center, NONE),
            AT(BARRIER) = SCRIPT(k_barrier, NONE),
            AT(OPEN) = SCRIPT(k_open, NONE),
            AT(STELE1) = SCRIPT(k_stele1, C3_STELE1),
            AT(STELE2) = SCRIPT(k_stele2, C3_STELE2),
            AT(STELE3) = SCRIPT(k_stele3, C3_STELE3),
            AT(HALL) = SCRIPT(k_hall, C3_CAMP),
            AT(REST) = SCRIPT(k_rest, REST),
            AT(SHIYAN) = SCRIPT(k_shiyan, NONE),
            AT(BOSS) = SCRIPT(k_boss, C3_BATTLE),
            AT(QUIET) = SCRIPT(k_quiet, NONE),
            AT(REVEAL) = SCRIPT(k_reveal, C3_REVEAL),
            AT(WENZHOU) = SCRIPT(k_wenzhou, NONE),
            AT(REPORT) = SCRIPT(k_report, ENDING),
            AT(REVIVE) = SCRIPT(k_revive, NONE),
#undef AT
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 3u,
    .first_stage = H2_XIANXIA_RPG_STAGE_C3_ASCEND,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C3_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_RAIN_PATH,
    .map_count = C3_MAPS,
    .start_map = H2_XIANXIA_RPG_MAP_RAIN_PATH,
    .rest_map = H2_XIANXIA_RPG_MAP_RAIN_PATH,
    .start_x = 7,
    .start_y = 8,
    .rest_x = 4,
    .rest_y = 7,
    .default_level = 6u,
    .default_pills = 3u,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C3_INTRO,
    .label = "第三章",
    .name = "第三章 · 雨夜剑阁",
    .title = "雨夜剑阁",
    .ending = {"剑魄归鞘，破封者闻舟现身雨檐。",
               "锁阵已断，竹溪三盏祭灯危在旦夕。"},
    .next = "第三章 完 · 下一章《万灯归山》",
    .recap =
        "青萍渡雾散，江篱洗清嫌疑，寻回母亲的丙三号。沉船中留下雨纹剑穗，指向西山剑阁。两人冒雨上山，追问偷船与破封的缘由。",
};
static bool c3_map(h2_xianxia_rpg_map_t map) {
  return map >= H2_XIANXIA_RPG_MAP_RAIN_PATH &&
         map <= H2_XIANXIA_RPG_MAP_SWORD_HALL;
}
char h2_xianxia_rpg_c3_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (!c3_map(map) || x < 0 || y < 0 || x >= H2_XIANXIA_RPG_MAP_COLS ||
      y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_RAIN_PATH][y][x];
}
const char* h2_xianxia_rpg_c3_map_name(h2_xianxia_rpg_map_t map) {
  return c3_map(map) ? k_names[map - H2_XIANXIA_RPG_MAP_RAIN_PATH] : "";
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c3_script(
    h2_xianxia_rpg_script_id_t id) {
  if (id < H2_XIANXIA_RPG_SCRIPT_C3_FIRST ||
      id >= H2_XIANXIA_RPG_SCRIPT_C4_FIRST)
    return NULL;
  return &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C3_FIRST];
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c3_info(void) {
  return &k_info;
}
size_t h2_xianxia_rpg_c3_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  if (map != H2_XIANXIA_RPG_MAP_SWORD_HALL) return 0;
  size_t count = 0;
  out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_SHIYAN, 3, 2,
                                        H2_XIANXIA_RPG_FACING_RIGHT};
  if (game->stage < H2_XIANXIA_RPG_STAGE_C3_RETURN)
    out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_SWORD_SOUL, 7, 3,
                                          H2_XIANXIA_RPG_FACING_DOWN};
  return count;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c3_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  if (actor == H2_XIANXIA_RPG_ACTOR_SWORD_SOUL)
    return H2_XIANXIA_RPG_SCRIPT_C3_BOSS;
  if (actor == H2_XIANXIA_RPG_ACTOR_SHIYAN) {
    if (game->stage < H2_XIANXIA_RPG_STAGE_C3_RETURN)
      return H2_XIANXIA_RPG_SCRIPT_C3_SHIYAN;
    return game->flags & H2_XIANXIA_RPG_FLAG_C3_WENZHOU
               ? H2_XIANXIA_RPG_SCRIPT_C3_REPORT
               : H2_XIANXIA_RPG_SCRIPT_C3_REVEAL;
  }
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}
bool h2_xianxia_rpg_c3_walkable(const h2_xianxia_rpg_game_t* game, char tile) {
  if (tile == 'X') return (game->flags & H2_XIANXIA_RPG_FLAG_C3_OPEN) != 0u;
  return tile == '.' || tile == 's' || tile == '_' || tile == 'v';
}
bool h2_xianxia_rpg_c3_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                        char tile) {
  switch (tile) {
    case 'Q':
    case 'n':
    case 'd':
    case 'O':
    case 'a':
    case 'b':
    case 'c':
    case 'L':
    case '1':
    case '2':
    case '3':
      return true;
    case 'X':
      return !h2_xianxia_rpg_c3_walkable(game, tile);
    default:
      return false;
  }
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c3_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile) {
  switch (tile) {
    case 'Q':
      return H2_XIANXIA_RPG_SCRIPT_C3_PAVILION;
    case 'n':
      return H2_XIANXIA_RPG_SCRIPT_C3_LETTER;
    case 'd':
      return (game->flags & H2_XIANXIA_RPG_FLAG_C2_ROPE)
                 ? H2_XIANXIA_RPG_SCRIPT_C3_CUT_KNOWN
                 : H2_XIANXIA_RPG_SCRIPT_C3_CUT;
    case 'O':
      return H2_XIANXIA_RPG_SCRIPT_C3_CENTER;
    case 'X':
      return H2_XIANXIA_RPG_SCRIPT_C3_BARRIER;
    case 'a':
      return H2_XIANXIA_RPG_SCRIPT_C3_STELE1;
    case 'b':
      return H2_XIANXIA_RPG_SCRIPT_C3_STELE2;
    case 'c':
      return H2_XIANXIA_RPG_SCRIPT_C3_STELE3;
    case 'L':
      return H2_XIANXIA_RPG_SCRIPT_C3_REST;
    default:
      return H2_XIANXIA_RPG_SCRIPT_COUNT;
  }
}
uint32_t h2_xianxia_rpg_c3_post_direction(uint32_t flags, uint32_t post) {
  return post < 3u ? flags >> (2u * post) & 3u : 0u;
}
bool h2_xianxia_rpg_c3_posts_aligned(uint32_t flags) {
  return (flags & H2_XIANXIA_RPG_C3_POST_BITS) == (1u | 0u << 2 | 3u << 4);
}
size_t h2_xianxia_rpg_c3_strings(const char** out, size_t capacity) {
  size_t count = 0;
#define ADD(s)                            \
  do {                                    \
    if (count < capacity) out[count] = s; \
    ++count;                              \
  } while (0)
  for (size_t i = 0; i < C3_MAPS; ++i) ADD(k_names[i]);
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
