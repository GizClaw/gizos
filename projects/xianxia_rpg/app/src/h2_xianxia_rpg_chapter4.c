/* Chapter 4: lanterns carried home, and the cost seen downstream.
 * The village borrows chapter 1's actual layout; only its well becomes the
 * sheltered source lamp. Wind checks happen on completed steps, never time. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

/* Ridge paths cross one exposed column. 'w' is wind, '1'..'3' are lamps.
 * Rows 8 and 9 are scenery, leaving room for the two exploration buttons. */
static const char* const k_maps[3][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"TTTTTTTTTTTTTTT", "T~~~~~T~~~~~~~T", "T.....T.......T", "T.....w....1..T",
     "======w========", "T.....w.......T", "T.....T.......T", "T~~~~~T~~~~~~~T",
     "TTTTTTTTTTTTTTT", "TTTTTTTTTTTTTTT"},
    {"TTTTTTTTTTTTTTT", "T~~~~~~~T~~~~~T", "T.......T.....T", "T.......w..2..T",
     "========w======", "T.......w.....T", "T.......T.....T", "T~~~~~~~T~~~~~T",
     "TTTTTTTTTTTTTTT", "TTTTTTTTTTTTTTT"},
    {"###############", "#_____P_P_____#", "#_____________#", "#______3______#",
     "=====w________#", "#____w________#", "#____w________#", "#____P________#",
     "###############", "###############"},
};
static const char* const k_names[] = {"竹溪村", "山脊灯路·东脊",
                                      "山脊灯路·西脊", "竹溪祭台"};
static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "剑阁雨还未停，两人已赶回竹溪。熟悉的桥下，墨色又漫过了溪灯。"),
    SAY(HERO,
        "祠堂门前都是人……陶伯！我们回来了。锁阵断了，石砚让我们重燃山脊三灯。"),
    SAY(JIANGLI, "先问清灯路。我会陪你过风口，这回也让我守住你的家。"),
};
static const h2_xianxia_rpg_line_t k_brief[] = {
    SAY(ELDER,
        "古祠的封印还在，可山脊三盏祭灯失了照应。今夜再不续火，浊气就要越过桥。"),
    SAY(HERO, "第一回是爹守过的古祠，这一回是全村人的灯。我们去。"),
    SAY(ELDER,
        "井边藏着祭台留下的火种。取火后向东，依次点亮东脊、西脊、祭台三灯。亮灯就是歇息点。"),
    SAY(JIANGLI,
        "我正式和你结伴守灯。路上点下方“江篱：探路”，切成护火；风口就吹不灭火种。"),
    SAY(NARRATOR,
        "江篱入队。护灯战中她有气血；点她的状态框切换撑篙或续灯。丹药可以救起倒下的她。"),
};
static const h2_xianxia_rpg_line_t k_wait[] = {
    SAY(ELDER,
        "取井边火种，向东过山脊。先点东脊灯，再点西脊灯，最后守住祭台。没有时限，慢慢走。"),
};
static const h2_xianxia_rpg_line_t k_fire[] = {
    SAY(NARRATOR,
        "井边石龛开了。祭台留存的一点火，映在当年那块青萍渡铜牌上。火种已取，两人恢复。"),
    SAY(JIANGLI,
        "蓝白横线是风口。每踏一格便吹一次，站着等不会变。点下方按钮让我护火，再走过去。"),
    SAY(NARRATOR,
        "护火可安全过风口；探路时火会灭，返回最近灯台重取。已亮祭灯不会丢。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_need_fire[] = {
    SAY(JIANGLI,
        "灯芯还是冷的。先问陶伯，再到井边取火种；每一段路，先点亮本段祭灯再往东。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "目标：把火种带过风口，点亮三盏祭灯。东脊和西脊的灯在路右侧，第三盏在祭台中央。"),
    SAY(JIANGLI,
        "点下方“江篱：探路”可切成护火。护火时我跟在身后挡风；探路时风会吹灭火种。"),
    SAY(NARRATOR,
        "只按走完的格子判定，不按等待时间。灭火回最近灯台，两人恢复、重取火种；已亮灯保留。"),
    SAY(NARRATOR,
        "最后护灯战：灯不灭，过三波就赢。敌人目标会提前显示，点“护灯”可替灯承伤并回灵。"),
};
static const h2_xianxia_rpg_line_t k_cover[] = {
    SAY(JIANGLI,
        "我收起探路的长篙，护在火种旁。现在是护火，走过风口也不会熄。再点按钮可切回探路。"),
};
static const h2_xianxia_rpg_line_t k_scout[] = {
    SAY(JIANGLI,
        "我去看前路。现在是探路，火种没有遮护，踏入风口会灭；过风口前记得叫我回来。"),
};
static const h2_xianxia_rpg_line_t k_out[] = {
    SAY(NARRATOR,
        "风口吹灭了火种。两人退回最近灯台，重新取火，气血灵力恢复；已经点亮的祭灯仍在。"),
    SAY(JIANGLI,
        "先点下方按钮切成护火，再过风口。走快走慢都一样，不用抢时间。"),
};
static const h2_xianxia_rpg_line_t k_safe[] = {
    SAY(JIANGLI, "风来了，我替火种挡住了。火还亮着，继续点东边的祭灯吧。"),
};
static const h2_xianxia_rpg_line_t k_lamp1[] = {
    SAY(NARRATOR,
        "东脊祭灯亮起。浊气从石阶退开，竹溪第一排屋檐重新有了暖光。（祭灯一，已记录进度）"),
    SAY(NARRATOR,
        "一只信鸽落在灯顶。鲁把头写道：渡口的雾散了，今夜河面却少了一半灯影。"),
    SAY(HERO, "我们这边亮了，他那边却暗了？先把村子守住，这封信也要带给陶伯。"),
};
static const h2_xianxia_rpg_line_t k_lamp2[] = {
    SAY(NARRATOR,
        "西脊祭灯重新燃起。江篱按住胸口，提灯的手轻轻抖了一下。（祭灯二，已记录进度）"),
    SAY(JIANGLI,
        "像有根线从心口扯过去……没事，先去祭台。我能撑住，你也别只顾着灯忘了自己。"),
    SAY(HERO, "我记着。到最后一盏，我们一起守。"),
};
static const h2_xianxia_rpg_line_t k_lamp3[] = {
    SAY(NARRATOR,
        "第三盏祭灯亮起，照水方向的天边却泛起暗红。祭台下，浊潮聚成了墨蛟的残影。"),
    SAY(JIANGLI,
        "它冲灯来的！灯有八十点耐久，守过三波就赢；每波撑三回合，或提前打散它。"),
    SAY(NARRATOR,
        "指令“护灯”替换逃跑：替灯承伤并回灵。普通“防御”只护自己。上方提前显示敌人目标。"),
    SAY(JIANGLI,
        "点我的状态框，切换“撑篙”攻击或“续灯”修复十二点。丹药先点指令，再点要救的人。"),
    SAY(NARRATOR,
        "两人恢复，祭台检查点已记录。再点第三盏灯开战；灭灯或青崖倒下，都从这里重试。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(NARRATOR, "灯下歇息，火种重续，两人气血与灵力恢复。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_battle[] = {
    SAY(HERO, "护住灯，也护住人。它扑灯时我挡，扑你时我帮你疗伤。"),
    SAY(JIANGLI,
        "好。灯和人的气血都在上方，不必急着点。撑过三波，村子就能等到天亮。"),
};
static const h2_xianxia_rpg_line_t k_revive[] = {
    SAY(NARRATOR,
        "两人在祭台灯旁缓过气来。三盏祭灯仍在，火种重续。再点第三盏灯，三波从头开始，耐久全满。"),
    SAY(JIANGLI,
        "看到扑灯或墨浪，就点“护灯”。点我的状态框切成续灯能修复；我倒下时，丹药也能救起。"),
};
static const h2_xianxia_rpg_line_t k_home[] = {
    SAY(NARRATOR,
        "浊潮退入山缝。山脊三灯连成一线，竹溪桥下又有了清水，村民从祠堂里走出来。"),
    SAY(JIANGLI, "灯守住了……可照水那边，为什么更暗了？"),
    SAY(NARRATOR,
        "江篱忽然跪倒，掌心的灯光一闪即灭。沈青崖扶住她，片刻后，她才重新睁开眼。"),
    SAY(HERO, "不再让你硬撑。我们回村，把信和这一路的事都告诉陶伯。"),
};
static const h2_xianxia_rpg_line_t k_report[] = {
    SAY(ELDER,
        "村子活过来了。可这封信……上游越亮，下游越暗。闻舟说灯该照两岸，怕不是空话。"),
    SAY(HERO,
        "铜牌、断符、剑穗，都在把我们往灯阵上引。爹守的究竟是封印，还是一道不能停的灯路？"),
    SAY(ELDER,
        "你爹离开那年，把这半册灯簿封在祭台暗格。我今夜取出来，才知道它缺了后半册。"),
    SAY(NARRATOR,
        "旧灯簿上半册，末页写着：归灯人，照水闻氏。后面十年的栏格，一片空白。"),
    SAY(JIANGLI,
        "闻舟姓闻。我要问他的船，也该去问他们家的灯。你救了村子，我想替下游也问一句。"),
    SAY(HERO, "我们一起去。救了这一岸，不该就让另一岸沉进黑里。"),
};
static const h2_xianxia_rpg_line_t k_rumor[] = {
    SAY(ELDER,
        "你在渡口听过的日子……青萍渡起雾那夜，正是溪灯节。我们放灯的那刻，他们的灯却灭了。"),
};
static const h2_xianxia_rpg_line_t k_depart[] = {
    SAY(HERBALIST,
        "江姑娘的寒意不是山风。若再发作，记着百草寒谷的荀七娘，那是我师妹。别硬撑。"),
    SAY(CHILD,
        "青崖哥哥，我把溪灯留一半，等下游的人也回来放。你们一定要带消息回来。"),
    SAY(NARRATOR,
        "陶伯把灯纹“风”拓在旧灯簿里。江篱系紧长篙，与青崖并肩站在桥头。"),
    SAY(JIANGLI,
        "从渡口同行，到今晚守住你的家。往后不分谁的岸，我都和你一起走。"),
    SAY(NARRATOR,
        "天亮了。两人带着上半册灯簿下山，去照水旧城寻找闻家，以及遗失的下半册。"),
};
static const h2_xianxia_rpg_line_t k_herb[] = {
    SAY(HERBALIST,
        "药都搬到祠堂了。井边灯龛和亮起的祭灯能让两人恢复，别带着伤过山。"),
};
static const h2_xianxia_rpg_line_t k_herb_clear[] = {
    SAY(HERBALIST,
        "溪水清了，我又能回药田。江姑娘脸色不好，等见过陶伯，来喝一碗热汤。"),
};
static const h2_xianxia_rpg_line_t k_child[] = {
    SAY(CHILD,
        "上次你把墨蛟封回去了，这次我也不哭。我的溪灯还在桥边，等你们带火回来。"),
};
static const h2_xianxia_rpg_line_t k_child_clear[] = {
    SAY(CHILD,
        "桥下的灯又亮了！可江篱姐姐说，下游也有小孩子在等灯……我们也给他们留一些。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),       SCRIPT(k_brief, C4_JOIN),
    SCRIPT(k_wait, NONE),        SCRIPT(k_fire, C4_FIRE),
    SCRIPT(k_need_fire, NONE),   SCRIPT(k_help, NONE),
    SCRIPT(k_cover, NONE),       SCRIPT(k_scout, NONE),
    SCRIPT(k_out, NONE),         SCRIPT(k_safe, NONE),
    SCRIPT(k_lamp1, C4_LIGHT1),  SCRIPT(k_lamp2, C4_LIGHT2),
    SCRIPT(k_lamp3, C4_LIGHT3),  SCRIPT(k_rest, C4_FIRE),
    SCRIPT(k_battle, C4_BATTLE), SCRIPT(k_revive, NONE),
    SCRIPT(k_home, C4_HOME),     SCRIPT(k_report, C4_REPORT),
    SCRIPT(k_rumor, C4_REPORT),  SCRIPT(k_depart, ENDING),
    SCRIPT(k_herb, NONE),        SCRIPT(k_herb_clear, NONE),
    SCRIPT(k_child, NONE),       SCRIPT(k_child_clear, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 4u,
    .first_stage = H2_XIANXIA_RPG_STAGE_C4_HOME,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C4_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
    .map_count = 4,
    .start_map = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
    .rest_map = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
    .start_x = 7,
    .start_y = 5,
    .rest_x = 8,
    .rest_y = 5,
    .default_level = 7u,
    .default_pills = 3u,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C4_INTRO,
    .label = "第四章",
    .name = "第四章 · 万灯归山",
    .title = "万灯归山",
    .ending = {"三灯重燃，竹溪平安。", "携旧灯簿，与江篱同赴照水。"},
    .next = "第四章 完 · 下一章《照水旧城》",
    .recap =
        "听雨归鞘，闻舟说灯本该照两岸。锁阵已断，两人赶回竹溪，重燃山脊三灯。",
};
char h2_xianxia_rpg_c4_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= H2_XIANXIA_RPG_MAP_COLS ||
      y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  if (map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE) {
    const char tile = h2_xianxia_rpg_tile(H2_XIANXIA_RPG_MAP_VILLAGE, x, y);
    return tile == 'W' ? 'F' : tile;
  }
  if (map < H2_XIANXIA_RPG_MAP_EAST_RIDGE ||
      map > H2_XIANXIA_RPG_MAP_LANTERN_ALTAR)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_EAST_RIDGE][y][x];
}
const char* h2_xianxia_rpg_c4_map_name(h2_xianxia_rpg_map_t map) {
  return map >= H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE &&
                 map <= H2_XIANXIA_RPG_MAP_LANTERN_ALTAR
             ? k_names[map - H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE]
             : "";
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c4_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C4_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_COUNT
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C4_FIRST]
             : NULL;
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c4_info(void) {
  return &k_info;
}
size_t h2_xianxia_rpg_c4_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  if (map != H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE) return 0;
  const bool clear = game->stage >= H2_XIANXIA_RPG_STAGE_C4_RETURN;
  out[0] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_ELDER, 4, 3,
                                  H2_XIANXIA_RPG_FACING_DOWN};
  out[1] =
      (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_HERBALIST, clear ? 11 : 5, 3,
                             H2_XIANXIA_RPG_FACING_DOWN};
  out[2] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_CHILD, 6, clear ? 8 : 3,
                                  H2_XIANXIA_RPG_FACING_DOWN};
  return 3;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c4_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  const bool clear = game->stage >= H2_XIANXIA_RPG_STAGE_C4_RETURN;
  if (actor == H2_XIANXIA_RPG_ACTOR_ELDER)
    return clear ? H2_XIANXIA_RPG_SCRIPT_C4_REPORT
           : game->flags & H2_XIANXIA_RPG_FLAG_C4_JOINED
               ? H2_XIANXIA_RPG_SCRIPT_C4_WAIT
               : H2_XIANXIA_RPG_SCRIPT_C4_BRIEF;
  if (actor == H2_XIANXIA_RPG_ACTOR_HERBALIST)
    return clear ? H2_XIANXIA_RPG_SCRIPT_C4_HERB_CLEAR
                 : H2_XIANXIA_RPG_SCRIPT_C4_HERB;
  return clear ? H2_XIANXIA_RPG_SCRIPT_C4_CHILD_CLEAR
               : H2_XIANXIA_RPG_SCRIPT_C4_CHILD;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c4_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile) {
  if (!(game->flags & H2_XIANXIA_RPG_FLAG_C4_JOINED))
    return H2_XIANXIA_RPG_SCRIPT_C4_NEED_FIRE;
  if (tile == 'F') return H2_XIANXIA_RPG_SCRIPT_C4_FIRE;
  if (tile >= '1' && tile <= '3') {
    if (tile == '3' && game->stage >= H2_XIANXIA_RPG_STAGE_C4_RETURN)
      return H2_XIANXIA_RPG_SCRIPT_C4_HOME;
    if (tile == '3' && game->stage == H2_XIANXIA_RPG_STAGE_C4_GUARD)
      return H2_XIANXIA_RPG_SCRIPT_C4_BATTLE;
    if (game->flags & (1u << (tile - '1')))
      return H2_XIANXIA_RPG_SCRIPT_C4_REST;
    if (!(game->flags & H2_XIANXIA_RPG_FLAG_C4_FIRE))
      return H2_XIANXIA_RPG_SCRIPT_C4_NEED_FIRE;
    return (h2_xianxia_rpg_script_id_t)(H2_XIANXIA_RPG_SCRIPT_C4_LAMP1 + tile -
                                        '1');
  }
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}
uint32_t h2_xianxia_rpg_c4_lamps(uint32_t flags) {
  return (flags & 1u) + ((flags >> 1u) & 1u) + ((flags >> 2u) & 1u);
}
size_t h2_xianxia_rpg_c4_strings(const char** out, size_t capacity) {
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
