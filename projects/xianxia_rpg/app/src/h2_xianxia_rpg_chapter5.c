/* Chapter 5 keeps permanent stone landings between two tide-only routes.
 * 'd' is the exposed street, 'r' the floating roof bridge, 'B' a tide bell.
 * All exits land on permanent '=' platforms, including after a tide switch. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_maps[3][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"~~~~~~~~~~~~~~~", "~hhh~~~~hhh~~~~", "~hhh====hhh~~~~", "~===B======~~~~",
     "===============", "~==========~~~~", "~~~hhh==hhh~~~~", "~~~hhh==hhh~~~~",
     "~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~"},
    {"###############", "#hhh~~~~hhh~~~~#", "#~~~rrrrrrrr===",
     "#~~Brr~~~~~~==#", "eee==r~~~~~~==#", "#~~~d~~~~~~~~~#", "#~~~dddddddddd#",
     "#~~~~~~~~~~p~~#", "#~~~~~~~~~~~~~#", "###############"},
    {"###############", "#____P___P____#", "#_____________#", "#_____________#",
     "=======_______#", "#_____________#", "#____P___P____#", "#_____________#",
     "###############", "###############"},
};
static const char* const k_names[] = {"照水·水上集市", "照水·半淹旧城",
                                      "闻家灯楼"};
static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "竹溪的三盏祭灯还在身后，照水的半座城却沉在水里。船屋之间，买卖声隔着水传来。"),
    SAY(HERO,
        "旧灯簿上写着“归灯人，照水闻氏”。我们带来了上半册，也带来了上游亮灯后的消息。"),
    SAY(JIANGLI,
        "先问药铺船的大夫吧。我心口的寒意又来了……这次，我不会瞒着你。"),
};
static const h2_xianxia_rpg_line_t k_brief[] = {
    SAY(BOAT_DOCTOR,
        "闻家住在东边旧城灯楼。十年前水淹进门，闻婆却不肯搬，她说总要有人记得岸在哪里。"),
    SAY(BOAT_DOCTOR,
        "两口金色潮钟相通，集市一口，旧城左岸一口。点钟换水位，站着等不会涨水。钟边石台永远安全。"),
    SAY(JIANGLI,
        "退潮走褐色街面，右下角能找到灯簿残页。涨潮走青色浮桥，右上方才接得上灯楼。"),
    SAY(BOAT_DOCTOR,
        "先向东去旧城，退潮取残页，再回钟边涨潮上楼。点药铺的我可以歇息记录；下方“查看水路”随时看提示。"),
    SAY(HERO,
        "我不是来把最后一点灯也拿走的。只是这一路越救越暗，总得有人把两半灯簿放在一起。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "金钟换潮，灰白石台永远能走。退潮：褐色街面开放，青色浮桥落在墙外。涨潮：街面淹没，浮桥接上屋脊。"),
    SAY(NARRATOR,
        "旧城左上金钟旁是安全检查点。右下金色残页只在退潮可取；右上出口沿涨潮浮桥通往灯楼。点钟后进度立即记录。"),
    SAY(JIANGLI,
        "闻舟与青崖对剑，我从旁相助。撑过六回合，或逼到半血就停手。垂雨守，穿檐合击，回风别出剑。"),
    SAY(NARRATOR,
        "救援时：先退潮，去右下找闻婆；背起后十四步内回左岸钟台。敲钟涨潮架起撤离浮桥，再向西回集市。"),
    SAY(NARRATOR,
        "背人时只计算走完的格子，等待和对白不计步。步数用完，两人被闻舟接回钟台，闻婆仍等在下层，可重新救。"),
};
static const h2_xianxia_rpg_line_t k_low[] = {
    SAY(NARRATOR,
        "潮钟低鸣，水退了。褐色街面露出，屋脊浮桥落下。两人已站稳钟台，气血恢复。（已记录进度）"),
    SAY(JIANGLI,
        "从钟台向下走街面，右下角有残页；救援时闻婆也在那里。要上灯楼，得回钟台再敲一次。"),
};
static const h2_xianxia_rpg_line_t k_high[] = {
    SAY(NARRATOR,
        "潮钟长鸣，水涨了。街面淹没，青色浮桥接上屋脊。钟台仍然安全，水位与落点已记录。"),
    SAY(JIANGLI,
        "从钟台往右上走青色浮桥，就能到灯楼。背着闻婆时，左边撤离桥也接好了，向西回药铺船。"),
};
static const h2_xianxia_rpg_line_t k_page[] = {
    SAY(NARRATOR,
        "泥中残页写着“归灯队，雪岭未至”。边角有一枚潮纹，与上半册缺口恰好相合。"),
    SAY(HERO, "十年前不是平白断了来往。先把纸护好，带给闻婆。"),
    SAY(JIANGLI,
        "残页已收好。我们回到左岸潮钟，再敲成涨潮，走右上浮桥上灯楼。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_need_page[] = {
    SAY(WEN_GRANDMA,
        "竹溪来的？隔着门说吧。你们点起三灯，我们又沉了三级台阶。没有那张归灯残页，我不信半册话。"),
    SAY(JIANGLI, "去旧城右下街面找残页。先回左岸敲钟退潮，拿到后再涨潮回来。"),
};
static const h2_xianxia_rpg_line_t k_question[] = {
    SAY(WEN_GRANDMA,
        "这纸是我家的。上半册也在你手里……十年了，终于有人肯过来看一眼。"),
    SAY(WEN_GRANDMA,
        "你是来要灯，还是来还灯？别急着替竹溪答，想想水下面那些屋子，再说你的话。"),
};
static const h2_xianxia_rpg_line_t k_take[] = {
    SAY(HERO,
        "我是来求灯的。竹溪的人需要它，照水的人也需要。我还不知道两岸怎么都保住，但不会再只拿走答案。"),
    SAY(WEN_GRANDMA,
        "肯说自己不知道，比说一切都好听。把这句话记着，以后到了两岸面前，还要敢这样说。"),
};
static const h2_xianxia_rpg_line_t k_return_light[] = {
    SAY(HERO,
        "我是来还灯的。还不是把一盏火放下就算完，是弄清我们多得的光，从谁家窗里灭掉了。"),
    SAY(WEN_GRANDMA,
        "那就先看清，再许诺。我会记着你今天的话；日后两岸相见，也愿意多听你一句。"),
};
static const h2_xianxia_rpg_line_t k_evidence[] = {
    SAY(NARRATOR,
        "闻婆打开下半册。两册接缝处，字迹完整了：十年前，阵改，引下游之光。旁边盖着竹溪祭台印。"),
    SAY(HERO,
        "祭台印……上游的灯，是从这里引去的。那一夜是谁改了阵？为何没有写名字？"),
    SAY(WEN_GRANDMA,
        "我不知道是谁。我只记得那夜旧城的灯一盏盏灭，水一寸寸进来。我们把孩子举在屋梁上，等到天亮。"),
    SAY(JIANGLI,
        "先是青萍渡，再是这座城。铜牌、断符、剑穗，到头来都绕着同一道伤口。"),
    SAY(WENZHOU,
        "现在明白了？你们再点一次灯，我家就再沉一次。把簿子给我，烧了它，至少不用再给竹溪找借口。"),
    SAY(HERO,
        "烧掉证据，水就会退么？你要问剑，我接；但这本簿子还得留给活着的人。"),
    SAY(NARRATOR,
        "证据已收，灯楼检查点已记录。点右侧闻舟对决：撑过六回合，或把他逼到半血，双方就会收剑。"),
};
static const h2_xianxia_rpg_line_t k_duel[] = {
    SAY(WENZHOU,
        "六合之内，把你的答案给我看。我的剑还记得听雨的三式，你若读过残碑，就该认得。"),
    SAY(JIANGLI,
        "我从旁相助，稳住这盏灯。垂雨时防御，穿檐可合击断势，回风别抢攻。不用拼到谁倒下。"),
};
static const h2_xianxia_rpg_line_t k_flood[] = {
    SAY(NARRATOR,
        "双剑分开，楼下一声裂响。失衡的水脉冲进旧城，闻婆被困在下层。两人退至左岸钟台。"),
    SAY(WENZHOU, "祖母！下层的路……都没了。"),
    SAY(HERO, "先把剑收起来。江篱，守住灯，我去敲钟。"),
    SAY(NARRATOR,
        "救援检查点已记录。先点左岸金钟退潮，沿下方褐色街面去右下找闻婆；背人后十四步内回钟台。"),
};
static const h2_xianxia_rpg_line_t k_pickup[] = {
    SAY(WEN_GRANDMA, "别管簿子，水在往上走……你是竹溪的人，为什么还回来？"),
    SAY(HERO, "因为您在这里。抓紧我，我们先到岸上再说。"),
    SAY(NARRATOR,
        "已背起闻婆，检查点已记录。十四步内返回左岸潮钟，再敲成涨潮架桥。右下到钟台最短九步，等待不计步。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {
    SAY(NARRATOR,
        "浪头截断街面。闻舟抛来绳索，两人退回左岸钟台；闻婆暂在下层石梁上，无人失散。"),
    SAY(WENZHOU, "我托住这根梁。你们再敲一次钟，趁水退下去接她！"),
    SAY(NARRATOR,
        "救援已回到涨潮检查点，证据和对决结果保留。退潮再救；背起后直接返回钟台，别在街面绕路。"),
};
static const h2_xianxia_rpg_line_t k_safe[] = {
    SAY(NARRATOR,
        "浮桥接住了三个人的重量。闻婆终于踏上药铺船，闻舟站在对岸，剑垂了下来。"),
    SAY(WENZHOU,
        "你明明可以拿着簿子走……我偷了丙三，割了锁阵，只想让上游也知道疼。"),
    SAY(JIANGLI,
        "丙三是我娘的船。你欠我的话，等大家都活着再说。把绳子系牢，别再割了。"),
    SAY(WEN_GRANDMA, "这枚潮纹交给你。两岸的旧账还在，可救命的事，我也记着。"),
    SAY(NARRATOR,
        "闻婆获救，灯纹“潮”已记入灯簿。（已记录进度）去问药铺船的大夫，江篱的手越来越冷。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(BOAT_DOCTOR,
        "船上的药火还暖。两人歇一歇，气血灵力恢复，当前水位已记录。别忘了右下残页，右上灯楼。"),
};
static const h2_xianxia_rpg_line_t k_end[] = {
    SAY(NARRATOR,
        "江篱接过灯簿，书页深处忽然亮出一行小字：渡灯，江氏女，系灯。她的名字从光里一闪而过。"),
    SAY(JIANGLI, "青崖……这不是外面的灯。它在我心里，越来越冷……"),
    SAY(NARRATOR,
        "她倒进青崖臂弯，掌心却烫得像火。半册旧账还未问完，眼前的人已不能再等。"),
    SAY(BOAT_DOCTOR,
        "这不是寒病，是灯阵反噬。我能护住她一程，解不了系灯。去百草寒谷，找药师荀七娘！"),
    SAY(HERO,
        "柳婆婆也说过这个名字。改阵是谁做的，我会继续查；现在先带江篱去寒谷。"),
    SAY(WEN_GRANDMA,
        "灯簿带上。你来要灯也好，还灯也好，今天你先把人背回来了。这比那句回答更重。"),
    SAY(WENZHOU,
        "上游的路我还认得。寒谷那边……我替你们探一段。别以为这就算两清。"),
    SAY(NARRATOR,
        "水上集市的药火送他们离岸。证据、潮纹与未答完的旧怨都留在灯簿里，下一程先为一人求生。"),
};
static const h2_xianxia_rpg_line_t k_revive[] = {
    SAY(WENZHOU, "站稳了再来。我问的是你的剑心，不要你的命。"),
    SAY(NARRATOR,
        "两人在最近检查点恢复。对决重新计算六回合，证据与选择保留；药物消耗要到下一次记录才保存。"),
};
static const h2_xianxia_rpg_line_t k_stay[] = {
    SAY(JIANGLI,
        "先救闻婆！退潮到右下背起她，再回钟台涨潮，左边浮桥就能撤回集市。右上的灯楼暂时别去了。"),
};
static const h2_xianxia_rpg_line_t k_otter[] = {
    SAY(NARRATOR,
        "一只水獭守在右侧石台。可绕开，也可点击挑战；涨潮时水系攻击增强两成，退潮时较弱。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),       SCRIPT(k_brief, C5_BRIEF),
    SCRIPT(k_help, NONE),        SCRIPT(k_low, NONE),
    SCRIPT(k_high, NONE),        SCRIPT(k_page, NONE),
    SCRIPT(k_need_page, NONE),   SCRIPT(k_question, C5_QUESTION),
    SCRIPT(k_take, C5_EVIDENCE), SCRIPT(k_return_light, C5_EVIDENCE),
    SCRIPT(k_evidence, NONE),    SCRIPT(k_duel, C5_DUEL),
    SCRIPT(k_flood, NONE),       SCRIPT(k_pickup, NONE),
    SCRIPT(k_retry, NONE),       SCRIPT(k_safe, NONE),
    SCRIPT(k_rest, REST),        SCRIPT(k_end, C5_END),
    SCRIPT(k_revive, NONE),      SCRIPT(k_stay, NONE),
    SCRIPT(k_otter, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 5u,
    .first_stage = H2_XIANXIA_RPG_STAGE_C5_ARRIVE,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C5_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_WATER_MARKET,
    .map_count = 3,
    .start_map = H2_XIANXIA_RPG_MAP_WATER_MARKET,
    .rest_map = H2_XIANXIA_RPG_MAP_WATER_MARKET,
    .start_x = 6,
    .start_y = 4,
    .rest_x = 6,
    .rest_y = 4,
    .default_level = 8u,
    .default_pills = 3u,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C5_INTRO,
    .label = "第五章",
    .name = "第五章 · 照水旧城",
    .title = "照水旧城",
    .ending = {"两册合一，旧阵改动初现。", "救下闻婆，携江篱急赴寒谷。"},
    .next = "第五章 完 · 下一章《百草寒谷》",
    .recap =
        "竹溪三灯重燃，下游却更暗。陶伯交出旧灯簿上半册，两人与江篱的寒意一同抵达照水，寻找闻家。",
};
char h2_xianxia_rpg_c5_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (map < H2_XIANXIA_RPG_MAP_WATER_MARKET ||
      map > H2_XIANXIA_RPG_MAP_WEN_TOWER || x < 0 || y < 0 ||
      x >= H2_XIANXIA_RPG_MAP_COLS || y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_WATER_MARKET][y][x];
}
const char* h2_xianxia_rpg_c5_map_name(h2_xianxia_rpg_map_t map) {
  return map >= H2_XIANXIA_RPG_MAP_WATER_MARKET &&
                 map <= H2_XIANXIA_RPG_MAP_WEN_TOWER
             ? k_names[map - H2_XIANXIA_RPG_MAP_WATER_MARKET]
             : "";
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c5_info(void) {
  return &k_info;
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c5_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C5_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_COUNT
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C5_FIRST]
             : NULL;
}
bool h2_xianxia_rpg_c5_walkable(const h2_xianxia_rpg_game_t* game, char tile) {
  return tile == '=' || tile == '_' ||
         (tile == 'e' && (game->stage < H2_XIANXIA_RPG_STAGE_C5_RESCUE ||
                          game->stage >= H2_XIANXIA_RPG_STAGE_C5_RETURN ||
                          (game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH))) ||
         (tile == 'd' && !(game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH)) ||
         (tile == 'r' && (game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH));
}
size_t h2_xianxia_rpg_c5_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  size_t n = 0;
#define NPC(a, x, y)                                           \
  out[n++] = (h2_xianxia_rpg_npc_t) {                          \
    H2_XIANXIA_RPG_ACTOR_##a, x, y, H2_XIANXIA_RPG_FACING_DOWN \
  }
  if (map == H2_XIANXIA_RPG_MAP_WATER_MARKET) {
    NPC(BOAT_DOCTOR, 7, 3);
    if (game->stage >= H2_XIANXIA_RPG_STAGE_C5_RETURN) {
      NPC(WEN_GRANDMA, 9, 4);
    }
  } else if (map == H2_XIANXIA_RPG_MAP_WEN_TOWER) {
    NPC(WEN_GRANDMA, 7, 3);
    if (game->stage == H2_XIANXIA_RPG_STAGE_C5_DUEL) {
      NPC(WENZHOU, 11, 3);
    }
  } else if (map == H2_XIANXIA_RPG_MAP_OLD_CITY &&
             game->stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE) {
    NPC(WEN_GRANDMA, 11, 6);
  }
#undef NPC
  return n;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c5_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  if (actor == H2_XIANXIA_RPG_ACTOR_BOAT_DOCTOR)
    return game->stage == H2_XIANXIA_RPG_STAGE_C5_ARRIVE
               ? H2_XIANXIA_RPG_SCRIPT_C5_BRIEF
           : game->stage >= H2_XIANXIA_RPG_STAGE_C5_RETURN
               ? H2_XIANXIA_RPG_SCRIPT_C5_END
               : H2_XIANXIA_RPG_SCRIPT_C5_REST;
  if (actor == H2_XIANXIA_RPG_ACTOR_WENZHOU)
    return H2_XIANXIA_RPG_SCRIPT_C5_DUEL;
  if (game->stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE)
    return H2_XIANXIA_RPG_SCRIPT_C5_PICKUP;
  if (game->stage >= H2_XIANXIA_RPG_STAGE_C5_RETURN)
    return H2_XIANXIA_RPG_SCRIPT_C5_SAFE;
  if (!(game->flags & H2_XIANXIA_RPG_FLAG_C5_PAGE))
    return H2_XIANXIA_RPG_SCRIPT_C5_NEED_PAGE;
  return game->stage >= H2_XIANXIA_RPG_STAGE_C5_DUEL
             ? H2_XIANXIA_RPG_SCRIPT_C5_EVIDENCE
             : H2_XIANXIA_RPG_SCRIPT_C5_QUESTION;
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c5_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile) {
  if (tile == 'B')
    return game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH
               ? H2_XIANXIA_RPG_SCRIPT_C5_LOW
               : H2_XIANXIA_RPG_SCRIPT_C5_HIGH;
  if (tile == 'p') return H2_XIANXIA_RPG_SCRIPT_C5_PAGE;
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}
size_t h2_xianxia_rpg_c5_strings(const char** out, size_t capacity) {
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
