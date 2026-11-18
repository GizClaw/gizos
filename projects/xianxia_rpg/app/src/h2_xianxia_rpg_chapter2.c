/* Chapter 2, 《渡口迷雾》: the ferry dock, the reed flats and the wreck on the
 * sandbar. Like h2_xianxia_rpg_story.c, the Chinese text here is a glyph
 * source of the body font. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define C2_MAPS 3

/* Tiles: '.' ground, '~' water, 'T' tree, 'R' roof, 'H' wall, 'D' door,
 * 'p' planks, 'n' notice board, 't' tea stall, 'm' drying nets, 'j' berth,
 * 'i' reed thicket, 'v' tall reeds (ambush ground), 'F' fog lifted by lamp
 * post '1', 'G' fog lifted by lamp post '2', '4' the wreck's bow lamp post,
 * 'U' the boat Bing San, 'c' a cut mooring line, 'k' fish basket, '|' hull,
 * 'x' crates, 'w' bilge water, 'Y' the sunken ferry lamp. */
static const char* const k_maps[C2_MAPS][H2_XIANXIA_RPG_MAP_ROWS] = {
    {
        "TTTTTTTTTTTTTTT",
        "TRRR.n..RRR.ttT",
        "THDH....HDH.t.T",
        "..............T",
        "T.............T",
        "T..........mm.T",
        "Tppppppppppmm.T",
        "~~~~~~~~~p~~~~~",
        "~~~~~~~~~p~~~~~",
        "~~~~~~~~~j~~~~~",
    },
    {
        "iiiiiiiiiiiiiii",
        "i~...Gvvc.F...i",
        "i~...Gvv.vF...i",
        "i~...G.vv.Fvv.i",
        "i~...G..v.F....",
        "i~U..Gv.2.Fv..i",
        "i~~..Gvv..F.v.i",
        "i~~..G..v.F.1.i",
        "i~~~.G.k.vFv..i",
        "iiiiiiiiiiiiiii",
    },
    {
        "iiiiiiiiiiiiiii",
        "i~~~..||||||||i",
        "i~~..4|pppxpYpi",
        "i~....|pxppwppi",
        "i~.v..|ppwwpxpi",
        "iU....ppppxpppi",
        "i~..v.|pwppxppi",
        "i~~...|xppwpppi",
        "i~~~~.||||||||i",
        "iiiiiiiiiiiiiii",
    },
};

static const char* const k_map_names[C2_MAPS] = {
    "青萍渡",
    "芦苇滩",
    "沙洲沉船",
};

#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "便船在白茫茫的江雾里摸索了大半日，才靠上青萍渡。码头冷冷清清，泊位全空着。"),
    SAY(HERO,
        "铜牌上刻着“青萍渡·丙三”……先找渡口管事问问这场雾，再打听丙三号的下落。"),
    SAY(NARRATOR, "金色菱标指着要找的人。桂嫂的茶棚可以歇息并记录进度。"),
};

static const h2_xianxia_rpg_line_t k_master_brief[] = {
    SAY(FERRYMASTER,
        "雾是三天前夜里起的。船一进去就打转，转到天亮又回原处，渡口已经停摆三天了。"),
    SAY(HERO, "我从竹溪来，想打听一条船，船牌是“青萍渡·丙三”。"),
    SAY(FERRYMASTER,
        "丙三？那是江家的船，五天前就丢了。渡口的人都说，这雾是江家丫头招来的。"),
    SAY(FERRYMASTER, "她就坐在栈桥尽头，谁劝也不走。你要问船，去问她吧。"),
};
static const h2_xianxia_rpg_line_t k_master_wait[] = {
    SAY(FERRYMASTER, "江篱在栈桥尽头。那丫头脾气倔，说话客气些。"),
};
static const h2_xianxia_rpg_line_t k_master_later[] = {
    SAY(FERRYMASTER,
        "芦苇滩里的旧灯桩，从前是给夜船引路的。真能点亮，雾兴许就散了。"),
};
static const h2_xianxia_rpg_line_t k_master_report[] = {
    SAY(FERRYMASTER, "雾……散了？丙三号也回来了！"),
    SAY(JIANGLI, "雾是从沙洲沉船里的旧渡灯冒出来的。灯重新点亮，雾就散了。"),
    SAY(FERRYMASTER,
        "江丫头，是我们错怪了你。我鲁某当着大伙儿，给你赔个不是。"),
    SAY(HERO, "鲁把头，这是我们在沉船船舱里拣到的剑穗。"),
    SAY(FERRYMASTER, "雨纹穗子……这是剑阁的东西。"),
    SAY(TEAWIFE, "剑阁在西山。这几天，那边正下着连绵的雨。"),
    SAY(JIANGLI, "偷我船的人，我自己去问。"),
    SAY(HERO, "割开古祠封符的那一剑，我也要问个明白。我们一起走。"),
};

static const h2_xianxia_rpg_line_t k_jiangli_meet[] = {
    SAY(JIANGLI, "又来一个问船的？船丢了，雾也不是我放的。"),
    SAY(HERO, "我不是来问罪的。这枚铜牌，是在竹溪古祠的祭坛边捡到的。"),
    SAY(JIANGLI, "这水纹……是我娘刻的。丙三是我娘留下的船，谁偷的，我就找谁。"),
    SAY(HERO, "偷船的人上山割开了古祠的封印。我也要找到他。"),
    SAY(JIANGLI,
        "渡口的人说雾是我放的。可我只会撑船……只是别人的灯一进雾就灭，我的不会。"),
    SAY(NARRATOR, "栈桥下的雾忽然翻涌，一团雾鬼缠了上来！"),
};
static const h2_xianxia_rpg_line_t k_jiangli_join[] = {
    SAY(JIANGLI,
        "芦苇滩里有旧灯桩，从前给夜船引路。我的灯在雾里不会灭，别问为什么，我也不知道。"),
    SAY(JIANGLI, "我带路、提灯，你出剑。走，去西边的芦苇滩找丙三号。"),
    SAY(NARRATOR,
        "江篱同行！她会照破敌人的雾隐，你防御时她会稳船护你。默契灯攒满三盏，就能合击。"),
};

static const h2_xianxia_rpg_line_t k_tea_first[] = {
    SAY(TEAWIFE,
        "坐下喝口热茶吧。这雾是三天前夜里起的……听说，正是上游竹溪放溪灯的那一晚。"),
    SAY(NARRATOR, "热茶下肚，气血与灵力都恢复了。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_tea_rest[] = {
    SAY(TEAWIFE, "雾大路滑，歇够了再走。"),
    SAY(NARRATOR, "热茶下肚，气血与灵力都恢复了。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_tea_clear[] = {
    SAY(TEAWIFE, "雾散了，渡口又热闹起来了！快去跟鲁把头说一声。"),
    SAY(NARRATOR, "气血与灵力都恢复了。（已记录进度）"),
};

static const h2_xianxia_rpg_line_t k_traveler[] = {
    SAY(TRAVELER, "我在这儿等了三天船了。江家的船一丢，雾就起了，真邪门。"),
    SAY(TRAVELER, "听说芦苇滩的高苇丛里常有妖物扑出来，走空地会安全些。"),
};
static const h2_xianxia_rpg_line_t k_traveler_clear[] = {
    SAY(TRAVELER, "雾散了，总算能过河了！多谢二位！"),
};
static const h2_xianxia_rpg_line_t k_sign[] = {
    SAY(NARRATOR, "告示：封渡三日。有能驱散江雾者，渡资全免。——青萍渡 鲁"),
};
static const h2_xianxia_rpg_line_t k_dock_closed[] = {
    SAY(NARRATOR,
        "西边芦苇滩的雾浓得化不开，独自进去只会迷路。先在渡口打听清楚。"),
};

static const h2_xianxia_rpg_line_t k_reeds_tip[] = {
    SAY(JIANGLI,
        "前面就是雾墙，硬闯只会原地打转。灯桩立在雾边，走到旁边轻触它，我来点灯。"),
};
static const h2_xianxia_rpg_line_t k_fog_wall[] = {
    SAY(JIANGLI, "雾墙太浓了。先找附近的灯桩，把它点亮。"),
};
static const h2_xianxia_rpg_line_t k_post1[] = {
    SAY(JIANGLI, "就是这根灯桩。灯芯受了潮……不过，我的灯点得着。"),
    SAY(NARRATOR,
        "江篱把提灯凑近灯芯，青白的火苗一跳，灯桩亮了。西面的雾墙退散了！"),
    SAY(NARRATOR, "点亮的灯桩可以歇息。（已记录进度，战败会回到最近的灯火旁）"),
};
static const h2_xianxia_rpg_line_t k_post2[] = {
    SAY(NARRATOR, "第二根灯桩亮了。光照到哪里，雾就退到哪里。"),
    SAY(JIANGLI, "雾在退！丙三号应该就搁在西边的浅水里。"),
    SAY(NARRATOR, "气血与灵力都恢复了。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_post_rest[] = {
    SAY(NARRATOR, "灯桩的光暖暖地照着，气血与灵力都恢复了。（已记录进度）"),
};

static const h2_xianxia_rpg_line_t k_fisher[] = {
    SAY(FISHER,
        "五天前夜里，有个戴斗笠的人撑走了丙三号。解缆都不用手，一剑就断。"),
    SAY(JIANGLI, "一剑……他往哪儿去了？"),
    SAY(FISHER, "往上游去了。回来的时候船上就没了人，空船漂进了这片雾里。"),
};
static const h2_xianxia_rpg_line_t k_fisher_later[] = {
    SAY(FISHER, "雾里的灯桩，是老辈人给夜船留的路。点亮了，路就出来了。"),
};
static const h2_xianxia_rpg_line_t k_rope_c1[] = {
    SAY(JIANGLI, "缆绳是被一剑削断的，切口平得像镜子。"),
    SAY(HERO, "和古祠里那张符纸的断口一样……是同一个人。"),
};
static const h2_xianxia_rpg_line_t k_rope[] = {
    SAY(JIANGLI, "缆绳是被一剑削断的，切口平得像镜子。"),
    SAY(HERO, "好快的剑。偷船的人，绝不是寻常水贼。"),
};
static const h2_xianxia_rpg_line_t k_rope_later[] = {
    SAY(NARRATOR, "断缆还挂在木桩上，切口平整如镜。"),
};
static const h2_xianxia_rpg_line_t k_trust[] = {
    SAY(JIANGLI, "芦叟的话、这条断缆……你都陪我查到了。谢谢你。"),
    SAY(NARRATOR, "江篱更信任你了：之后每场战斗开始时，默契灯先亮一盏。"),
};
static const h2_xianxia_rpg_line_t k_basket[] = {
    SAY(NARRATOR, "渔篓里压着一个油纸包，里面是一颗回春丹。"),
};
static const h2_xianxia_rpg_line_t k_basket_empty[] = {
    SAY(NARRATOR, "渔篓已经空了。"),
};

static const h2_xianxia_rpg_line_t k_found_boat[] = {
    SAY(JIANGLI, "丙三号！真的是它……船身没坏，只是船头的灯灭了。"),
    SAY(NARRATOR,
        "江篱跳上船头点亮船灯。灯光穿过雾气，照出一条通往江心沙洲的水路。"),
    SAY(JIANGLI,
        "沙洲上搁着一艘沉了多年的老渡船，雾就是从那边涌过来的。准备好了就上船。"),
    SAY(NARRATOR, "船灯也能歇息。（已记录进度）再轻触丙三号即可起航。"),
};
static const h2_xianxia_rpg_line_t k_sail[] = {
    SAY(NARRATOR, "丙三号推开江雾，向沙洲沉船驶去……"),
};
static const h2_xianxia_rpg_line_t k_wreck_land[] = {
    SAY(NARRATOR, "沙洲上搁着半截埋进沙里的老渡船，船舱里透出幽幽寒气。"),
    SAY(JIANGLI, "船头还有一根灯桩，我先点上。退路总得有盏灯。"),
    SAY(NARRATOR,
        "船头灯桩亮了。在此可以歇息，战败也会回到这里。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_guard[] = {
    SAY(NARRATOR, "舱口盘着一团浑浊的雾气，凝成了披甲的雾鬼，挡住了去路！"),
};
static const h2_xianxia_rpg_line_t k_guard_down[] = {
    SAY(JIANGLI, "舱口通了。渡灯就在船舱最里面，寒气也是从那里来的。"),
};
static const h2_xianxia_rpg_line_t k_wraith[] = {
    SAY(NARRATOR,
        "沉没的渡灯半埋在积水里，灯罩裂开，浊雾正从里面一缕缕往外冒。"),
    SAY(WRAITH, "……冷……光……都被拿走了……"),
    SAY(JIANGLI,
        "雾是从灯里出来的！它会藏进雾里，我提灯照它，你看准了再出手！"),
};
static const h2_xianxia_rpg_line_t k_relit[] = {
    SAY(NARRATOR, "雾魇散成千万缕轻烟，沉没的渡灯露出了水面。"),
    SAY(NARRATOR,
        "江篱俯身徒手托起渡灯，灯芯竟自己亮了。她的掌心闪过一缕青光，她却没有察觉。"),
    SAY(HERO, "（她的手……刚才是不是在发光？）"),
    SAY(JIANGLI,
        "雾在退了！咦，船舱角落里挂着一条剑穗，雨纹的。带上它，我们回渡口！"),
};
static const h2_xianxia_rpg_line_t k_revive_boss[] = {
    SAY(NARRATOR, "沈青崖在船头灯桩旁醒来，气血已经恢复。"),
    SAY(JIANGLI,
        "它藏进雾里时，等我提灯再出手。它收拢雾锁时，用合击打断，或者防御。"),
    SAY(NARRATOR,
        "提示：默契灯满三盏，“剑诀”会变成“合击”。气血低于一半就服丹药，防御还能回复灵力。"),
};
static const h2_xianxia_rpg_line_t k_boat_stay[] = {
    SAY(JIANGLI, "雾还没散，先查渡灯。"),
};
static const h2_xianxia_rpg_line_t k_sail_home[] = {
    SAY(NARRATOR, "丙三号载着渡灯，破开薄雾驶回青萍渡。"),
};
static const h2_xianxia_rpg_line_t k_lamp_lit[] = {
    SAY(NARRATOR, "渡灯静静地亮着，船舱里的寒气已经散尽。"),
};
static const h2_xianxia_rpg_line_t k_berth[] = {
    SAY(JIANGLI, "丙三号回家了。等这边的事了结，我再撑它出远门。"),
};

static const h2_xianxia_rpg_script_t
    k_scripts[H2_XIANXIA_RPG_SCRIPT_C3_FIRST - H2_XIANXIA_RPG_SCRIPT_C2_FIRST] =
        {
#define AT(id) [H2_XIANXIA_RPG_SCRIPT_##id - H2_XIANXIA_RPG_SCRIPT_C2_FIRST]
            AT(C2_INTRO) = SCRIPT(k_intro, NONE),
            AT(C2_MASTER_BRIEF) = SCRIPT(k_master_brief, C2_BRIEFED),
            AT(C2_MASTER_WAIT) = SCRIPT(k_master_wait, NONE),
            AT(C2_MASTER_LATER) = SCRIPT(k_master_later, NONE),
            AT(C2_MASTER_REPORT) = SCRIPT(k_master_report, ENDING),
            AT(C2_JIANGLI_MEET) = SCRIPT(k_jiangli_meet, C2_TUTORIAL),
            AT(C2_JIANGLI_JOIN) = SCRIPT(k_jiangli_join, C2_JOINED),
            AT(C2_TEA_FIRST) = SCRIPT(k_tea_first, C2_TEA),
            AT(C2_TEA_REST) = SCRIPT(k_tea_rest, REST),
            AT(C2_TEA_CLEAR) = SCRIPT(k_tea_clear, REST),
            AT(C2_TRAVELER) = SCRIPT(k_traveler, NONE),
            AT(C2_TRAVELER_CLEAR) = SCRIPT(k_traveler_clear, NONE),
            AT(C2_SIGN) = SCRIPT(k_sign, NONE),
            AT(C2_DOCK_CLOSED) = SCRIPT(k_dock_closed, NONE),
            AT(C2_REEDS_TIP) = SCRIPT(k_reeds_tip, C2_REEDS_TIP),
            AT(C2_FOG_WALL) = SCRIPT(k_fog_wall, NONE),
            AT(C2_POST1) = SCRIPT(k_post1, C2_LIGHT_POST1),
            AT(C2_POST2) = SCRIPT(k_post2, C2_LIGHT_POST2),
            AT(C2_POST_REST) = SCRIPT(k_post_rest, REST),
            AT(C2_FISHER) = SCRIPT(k_fisher, C2_FISHER),
            AT(C2_FISHER_LATER) = SCRIPT(k_fisher_later, NONE),
            AT(C2_ROPE_C1) = SCRIPT(k_rope_c1, C2_ROPE),
            AT(C2_ROPE) = SCRIPT(k_rope, C2_ROPE),
            AT(C2_ROPE_LATER) = SCRIPT(k_rope_later, NONE),
            AT(C2_TRUST) = SCRIPT(k_trust, NONE),
            AT(C2_BASKET) = SCRIPT(k_basket, C2_BASKET),
            AT(C2_BASKET_EMPTY) = SCRIPT(k_basket_empty, NONE),
            AT(C2_FOUND_BOAT) = SCRIPT(k_found_boat, C2_FOUND_BOAT),
            AT(C2_SAIL) = SCRIPT(k_sail, C2_SAIL),
            AT(C2_WRECK_LAND) = SCRIPT(k_wreck_land, C2_LIGHT_POST4),
            AT(C2_GUARD) = SCRIPT(k_guard, C2_GUARD_BATTLE),
            AT(C2_GUARD_DOWN) = SCRIPT(k_guard_down, NONE),
            AT(C2_WRAITH) = SCRIPT(k_wraith, C2_WRAITH_BATTLE),
            AT(C2_RELIT) = SCRIPT(k_relit, C2_HOME),
            AT(C2_REVIVE_BOSS) = SCRIPT(k_revive_boss, NONE),
            AT(C2_BOAT_STAY) = SCRIPT(k_boat_stay, NONE),
            AT(C2_SAIL_HOME) = SCRIPT(k_sail_home, C2_HOME),
            AT(C2_LAMP_LIT) = SCRIPT(k_lamp_lit, NONE),
            AT(C2_BERTH) = SCRIPT(k_berth, NONE),
#undef AT
};

static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 2u,
    .first_stage = H2_XIANXIA_RPG_STAGE_C2_ARRIVE,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C2_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_DOCK,
    .map_count = C2_MAPS,
    .start_map = H2_XIANXIA_RPG_MAP_DOCK,
    .rest_map = H2_XIANXIA_RPG_MAP_DOCK,
    .start_x = 7,
    .start_y = 3,
    .rest_x = 13,
    .rest_y = 3,
    .default_level = 4u,
    .default_pills = 3u,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C2_INTRO,
    .label = "第二章",
    .name = "第二章 · 渡口迷雾",
    .title = "渡口迷雾",
    .ending = {"雾散渡开，丙三号回到了青萍渡的泊位。",
               "雨纹剑穗指向西山剑阁，江篱与你同行。"},
    .next = "第二章 完 · 下一章《雨夜剑阁》",
    .recap =
        "竹溪溪水一夜发黑，沈青崖北上墨溪古祠，重新封住了墨蛟。祭坛边落着一枚"
        "“青萍渡·丙三”铜牌，旧符纸被人一剑削开。过了溪灯节，他带着铜牌顺流而下，"
        "来到下游的青萍渡。",
};

static bool c2_map(h2_xianxia_rpg_map_t map) {
  return map >= H2_XIANXIA_RPG_MAP_DOCK && map <= H2_XIANXIA_RPG_MAP_WRECK;
}

char h2_xianxia_rpg_c2_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (!c2_map(map) || x < 0 || y < 0 || x >= H2_XIANXIA_RPG_MAP_COLS ||
      y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_DOCK][y][x];
}

const char* h2_xianxia_rpg_c2_map_name(h2_xianxia_rpg_map_t map) {
  return c2_map(map) ? k_map_names[map - H2_XIANXIA_RPG_MAP_DOCK] : "";
}

const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c2_script(
    h2_xianxia_rpg_script_id_t id) {
  if (id < H2_XIANXIA_RPG_SCRIPT_C2_FIRST ||
      id >= H2_XIANXIA_RPG_SCRIPT_C3_FIRST)
    return NULL;
  return &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C2_FIRST];
}

const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c2_info(void) {
  return &k_info;
}

size_t h2_xianxia_rpg_c2_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  size_t count = 0;
  switch (map) {
    case H2_XIANXIA_RPG_MAP_DOCK:
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_FERRYMASTER, 5,
                                            4, H2_XIANXIA_RPG_FACING_DOWN};
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_TEAWIFE, 13, 2,
                                            H2_XIANXIA_RPG_FACING_DOWN};
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_TRAVELER, 9, 4,
                                            H2_XIANXIA_RPG_FACING_LEFT};
      /* Waits at the end of the pier until she joins the hero. */
      if (game->stage < H2_XIANXIA_RPG_STAGE_C2_TO_REEDS)
        out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_JIANGLI, 9,
                                              8, H2_XIANXIA_RPG_FACING_DOWN};
      break;
    case H2_XIANXIA_RPG_MAP_REEDS:
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_FISHER, 12, 1,
                                            H2_XIANXIA_RPG_FACING_DOWN};
      break;
    case H2_XIANXIA_RPG_MAP_WRECK:
      if (!(game->flags & H2_XIANXIA_RPG_FLAG_C2_GUARD))
        out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_WRECK_GUARD,
                                              6, 5, H2_XIANXIA_RPG_FACING_LEFT};
      break;
    default:
      break;
  }
  return count;
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c2_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  const h2_xianxia_rpg_stage_t stage = game->stage;
  const bool returned = stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN;
  switch (actor) {
    case H2_XIANXIA_RPG_ACTOR_FERRYMASTER:
      if (stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE)
        return H2_XIANXIA_RPG_SCRIPT_C2_MASTER_BRIEF;
      if (stage == H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI)
        return H2_XIANXIA_RPG_SCRIPT_C2_MASTER_WAIT;
      return returned ? H2_XIANXIA_RPG_SCRIPT_C2_MASTER_REPORT
                      : H2_XIANXIA_RPG_SCRIPT_C2_MASTER_LATER;
    case H2_XIANXIA_RPG_ACTOR_TEAWIFE:
      if (returned) return H2_XIANXIA_RPG_SCRIPT_C2_TEA_CLEAR;
      return game->flags & H2_XIANXIA_RPG_FLAG_C2_RUMOR
                 ? H2_XIANXIA_RPG_SCRIPT_C2_TEA_REST
                 : H2_XIANXIA_RPG_SCRIPT_C2_TEA_FIRST;
    case H2_XIANXIA_RPG_ACTOR_TRAVELER:
      return returned ? H2_XIANXIA_RPG_SCRIPT_C2_TRAVELER_CLEAR
                      : H2_XIANXIA_RPG_SCRIPT_C2_TRAVELER;
    case H2_XIANXIA_RPG_ACTOR_JIANGLI:
      return H2_XIANXIA_RPG_SCRIPT_C2_JIANGLI_MEET;
    case H2_XIANXIA_RPG_ACTOR_FISHER:
      return game->flags & H2_XIANXIA_RPG_FLAG_C2_FISHER
                 ? H2_XIANXIA_RPG_SCRIPT_C2_FISHER_LATER
                 : H2_XIANXIA_RPG_SCRIPT_C2_FISHER;
    case H2_XIANXIA_RPG_ACTOR_WRECK_GUARD:
      return H2_XIANXIA_RPG_SCRIPT_C2_GUARD;
    default:
      return H2_XIANXIA_RPG_SCRIPT_COUNT;
  }
}

bool h2_xianxia_rpg_c2_walkable(const h2_xianxia_rpg_game_t* game, char tile) {
  switch (tile) {
    case '.':
    case 'p':
    case 'v':
    case 'w':
      return true;
    case 'F':
      return (game->flags & H2_XIANXIA_RPG_FLAG_C2_POST1) != 0u;
    case 'G':
      return (game->flags & H2_XIANXIA_RPG_FLAG_C2_POST2) != 0u;
    default:
      return false;
  }
}

bool h2_xianxia_rpg_c2_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                        char tile) {
  switch (tile) {
    case 'n':
    case '1':
    case '2':
    case '4':
    case 'U':
    case 'c':
    case 'k':
    case 'Y':
      return true;
    case 'j':
      return game->stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN;
    case 'F':
    case 'G':
      return !h2_xianxia_rpg_c2_walkable(game, tile);
    default:
      return false;
  }
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c2_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile) {
  const uint32_t flags = game->flags;
  const h2_xianxia_rpg_stage_t stage = game->stage;
  switch (tile) {
    case 'n':
      return H2_XIANXIA_RPG_SCRIPT_C2_SIGN;
    case 'F':
    case 'G':
      return h2_xianxia_rpg_c2_walkable(game, tile)
                 ? H2_XIANXIA_RPG_SCRIPT_COUNT
                 : H2_XIANXIA_RPG_SCRIPT_C2_FOG_WALL;
    case '1':
      return flags & H2_XIANXIA_RPG_FLAG_C2_POST1
                 ? H2_XIANXIA_RPG_SCRIPT_C2_POST_REST
                 : H2_XIANXIA_RPG_SCRIPT_C2_POST1;
    case '2':
      return flags & H2_XIANXIA_RPG_FLAG_C2_POST2
                 ? H2_XIANXIA_RPG_SCRIPT_C2_POST_REST
                 : H2_XIANXIA_RPG_SCRIPT_C2_POST2;
    case '4':
      return H2_XIANXIA_RPG_SCRIPT_C2_POST_REST;
    case 'c':
      if (flags & H2_XIANXIA_RPG_FLAG_C2_ROPE)
        return H2_XIANXIA_RPG_SCRIPT_C2_ROPE_LATER;
      return flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED
                 ? H2_XIANXIA_RPG_SCRIPT_C2_ROPE_C1
                 : H2_XIANXIA_RPG_SCRIPT_C2_ROPE;
    case 'k':
      return flags & H2_XIANXIA_RPG_FLAG_C2_BASKET
                 ? H2_XIANXIA_RPG_SCRIPT_C2_BASKET_EMPTY
                 : H2_XIANXIA_RPG_SCRIPT_C2_BASKET;
    case 'U':
      if (game->map == H2_XIANXIA_RPG_MAP_WRECK)
        return stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN
                   ? H2_XIANXIA_RPG_SCRIPT_C2_SAIL_HOME
                   : H2_XIANXIA_RPG_SCRIPT_C2_BOAT_STAY;
      return flags & H2_XIANXIA_RPG_FLAG_C2_BOAT
                 ? H2_XIANXIA_RPG_SCRIPT_C2_SAIL
                 : H2_XIANXIA_RPG_SCRIPT_C2_FOUND_BOAT;
    case 'Y':
      return stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN
                 ? H2_XIANXIA_RPG_SCRIPT_C2_LAMP_LIT
                 : H2_XIANXIA_RPG_SCRIPT_C2_WRAITH;
    case 'j':
      return stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN
                 ? H2_XIANXIA_RPG_SCRIPT_C2_BERTH
                 : H2_XIANXIA_RPG_SCRIPT_COUNT;
    default:
      return H2_XIANXIA_RPG_SCRIPT_COUNT;
  }
}

size_t h2_xianxia_rpg_c2_strings(const char** out, size_t capacity) {
  size_t count = 0;
#define ADD(value)                            \
  do {                                        \
    if (count < capacity) out[count] = value; \
    ++count;                                  \
  } while (0)
  for (size_t i = 0; i < C2_MAPS; ++i) ADD(k_map_names[i]);
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
