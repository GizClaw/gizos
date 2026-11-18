/* Snow Ridge uses step-counted weather, never a real-time rescue clock.
 * s is a closing shortcut; the lower traverse remains open in every state. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_maps[3][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"###############", "#~~~~.....~~~~#", "#~~~~..B..~~~~#", "#.............#",
     "#.............#", "#....J.R.W....#", "#.............#", "#...=======...#",
     "#.............#", "###############"},
    {"###############", "#~~~~~~#~~~~~~#", "#......#......#", "#..R...#....T.#",
     "#......s......#", "#..ii..#......#", "#......#......#", "#......=......#",
     "#......#......#", "###############"},
    {"###############", "#~~~~~~#~~~~~~#", "#......#......#", "#..L...f....T.#",
     "#......s......#", "#..R...#......#", "#......#......#", "#......f......#",
     "#......#......#", "###############"},
};
static const char* const k_names[] = {"雪岭营地·断桥", "东冰崖·竹溪灯队",
                                      "西雪洞·照水灯队"};
static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "镜湖里的桥索还在眼前颤动，雪岭的风已扑到脸上。东边冰崖挂着竹溪的灯，西边雪洞传来照水人的呼声。断桥中间只剩两根绳。"),
    SAY(HERO,
        "陶伯、闻婆都来了。我们在镜湖看见的古约，终于有人重新走起来。先把两队接到营地，再修桥传灯。"),
    SAY(JIANGLI,
        "两边都能救。先东，陶伯能早些修桥；先西，闻婆能早些点起归灯。先到的一边会给药和炭，留一份照顾后到的人。"),
    SAY(NARRATOR,
        "下方选择先东或先西。走路才增加雪势，停下来读字不会变险；雪势十二时近路封住，下排绕路永远可走。各图的暖火都能歇息存档。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "营地下方选东或西。先救的队伍必须接出，才能去另一侧。东冰崖点右上灯队；西雪洞先点左上洞灯，照开下排暗路，再去右上灯队。"),
    SAY(NARRATOR,
        "先东得两药一炭，先西得一药一炭。第二队有人冻伤，在下方选择用炭或用药；零药也能用预留炭火救人。两队都安全后，回营地点上方断桥。"),
    SAY(NARRATOR,
        "每走一格雪势加一，十二封住中排近路，路会停下让你重新选。点下排石路绕行；封雪不会吞人、清掉已救结果，也没有倒计时。"),
    SAY(JIANGLI,
        "双灯战看上方敌人目标。护灯替两堆灯挡伤，防御只护自己。先东撑六回合，先西撑八回合且灯伤减弱；也可出剑驱散墨影。"),
    SAY(NARRATOR,
        "西队救出后，营地点江篱或闻舟切换同行者。江篱每回合补较弱的灯八点，闻舟削弱扑灯伤害八点。两人都不会占用你的回合。"),
};
static const h2_xianxia_rpg_line_t k_east[] = {
    SAY(NARRATOR,
        "东冰崖。陶伯的灯队在右上。中排近路雪势十二便封，下排石路始终连通。蓝冰可以走，旁边石地可以停；点左侧暖火歇息。"),
    SAY(ELDER,
        "青崖！我们听说照水失灯，不能再只等别人把灯送来。人还齐，只是风把绳冻住了。"),
};
static const h2_xianxia_rpg_line_t k_west[] = {
    SAY(NARRATOR,
        "西雪洞。先点左上洞灯，江篱会照亮暗路。中排近路封雪后，沿下排亮起的路绕到右上照水灯队。灯一旦点亮就不会熄。"),
    SAY(WENZHOU,
        "别急着往黑处走。我祖母带了灯，可这洞里连脚下都看不清。我认得退路，等你们接近。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(NARRATOR,
        "暖火旁，气血灵力恢复，路线、雪势和补给已经记录。歇息不减雪势，也不消耗炭火。回营地按钮始终能带你安全返回。"),
};
static const h2_xianxia_rpg_line_t k_light[] = {
    SAY(JIANGLI,
        "洞灯亮了。下排的白色石沿一直通到另一边，我们沿光走。闻舟，你在那头接一下！"),
    SAY(NARRATOR, "暗路已亮，位置已记录。雪势再大也不会封住下排路。"),
};
static const h2_xianxia_rpg_line_t k_block[] = {
    SAY(NARRATOR,
        "雪势十二，碎雪盖住中排近路！脚下的石地还稳，刚才的行走已停。改点下排绕路，两边仍可往返。此变化已记录。"),
};
static const h2_xianxia_rpg_line_t k_first_east[] = {
    SAY(ELDER,
        "人都接出来了！这两颗药和一包炭你带着，给雪洞里的人。我们先回营地接绳，不等别人替竹溪做完。"),
    SAY(HERO,
        "先东，随后西。把炭留给多等了一程的人。陶伯，这次桥不是你一个人修。"),
    SAY(NARRATOR,
        "竹溪队已安全，丹药加二（最多九），获得一份炭火。东队提前修桥：双灯战撑六回合可通桥。回营地，再去西雪洞。"),
};
static const h2_xianxia_rpg_line_t k_first_west[] = {
    SAY(WEN_GRANDMA,
        "照水的人齐了。这颗药和一包炭拿去，给冰崖的人暖暖手。我们先回营地点归灯，不能叫别人摸黑来接。"),
    SAY(WENZHOU,
        "我曾割断剑阵，想叫竹溪也尝尝失灯。今天先把绳接起来。要算的账，等所有人活着下山再说。"),
    SAY(JIANGLI,
        "你来带路，我来照路。丙三的事还没算清，但此刻，我们先做同一件事。"),
    SAY(NARRATOR,
        "照水队已安全，丹药加一（最多九），获得一份炭火；闻舟加入。先西的归灯令扑灯伤害减少百分之十五；修桥需撑八回合。回营地，再去东冰崖。"),
};
static const h2_xianxia_rpg_line_t k_injury[] = {
    SAY(NARRATOR,
        "后到的队伍多等了一程。一位挑灯人手脚冻僵，仍把灯举在胸口。人都找到了，还要给伤员暖身才能撤离。"),
    SAY(HERO,
        "先后有代价，可没有谁被放弃。把先前留下的炭拿来；也可以用一颗丹药，把炭留到以后。"),
    SAY(NARRATOR,
        "下方选择用炭或用药。已经发现伤员，刷新后仍在此处选择；不会重复领取前队补给。"),
};
static const h2_xianxia_rpg_line_t k_rescued[] = {
    SAY(NARRATOR,
        "暖意透过冻衣，挑灯人终于松开僵硬的手。两支队伍全部撤到营地，两岸的灯都保住了。治疗消耗与救援顺序已经记录。"),
    SAY(WENZHOU,
        "你救我祖母，也救竹溪的人。下一个地方，我和你一起去。先让我守住这座桥。"),
    SAY(JIANGLI,
        "营地可以点我的身影或闻舟，选一人并肩。另一个人照顾伤员，不是谁被丢在身后。"),
    SAY(NARRATOR,
        "回营地，点上方“断桥”开始双灯战。暖火可回满；两堆灯都不能灭，败后救援结果保留。"),
};
static const h2_xianxia_rpg_line_t k_need_rescue[] = {
    SAY(NARRATOR,
        "先把已经答应的队伍接出来，再往另一边。两队都安全后才守桥。下方可查看路线，不会消耗步数。"),
};
static const h2_xianxia_rpg_line_t k_battle[] = {
    SAY(NARRATOR,
        "桥下涌出墨影，直扑两堆灯。不是哪岸来抢光，是积在地脉里的浊气找到了裂口。陶伯拉索，闻婆举灯，第一次站在同一边。"),
    SAY(HERO,
        "上方写着下一次扑向哪边。点“护灯”，我替两堆灯挡；“防御”只护自己。不要让任何一堆灯归零！"),
    SAY(NARRATOR,
        "先东六回合通桥，先西八回合通桥且扑灯减伤。击散墨影也可获胜。江篱补较弱灯，闻舟削弱扑灯；所选同行者的援手自动生效。"),
    SAY(NARRATOR,
        "本次战前已存档，战中刷新从满灯重新开始。败后回营地暖火，两边救援与补给选择保留，可以换人再战。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {
    SAY(JIANGLI,
        "人都在，灯芯也还在。先退到暖火边重新点灯。这一次，看清墨影扑向哪边，护灯能替两边一起挡。"),
    SAY(NARRATOR,
        "已回营地暖火，气血灵力恢复，救援顺序与物资消耗保留。点上方断桥重试，两堆灯恢复六十。检查点已记录。"),
};
static const h2_xianxia_rpg_line_t k_reveal[] = {
    SAY(NARRATOR,
        "最后一根绳被拉直，两堆灯仍亮着。竹溪的挑灯人先把手伸过去，照水的人把灯递进他掌心；接着，他又接回一盏朝山上的归灯。"),
    SAY(ELDER,
        "原来我们以为送完就算尽责。灯要有人接，路也要一起修。闻家大姐，这一程一起走。"),
    SAY(WEN_GRANDMA,
        "十年前雪埋了桥，我们便把埋在雪下的话也忘了。今日先记下：两岸都有人肯回头接人。"),
    SAY(WENZHOU, "我把剑带上。以后该断的是锁住人的旧阵，不是让人相见的路。"),
    SAY(NARRATOR,
        "桥下的雪壳缓缓裂开，露出一口旧竖井。井壁刻着墨、雾、剑、风，还有水与镜的灯纹，一圈一圈向山腹延伸。"),
    SAY(JIANGLI, "下面有人在叫……不是叫灯，是叫你的名字。青崖，他说，别过来。"),
    SAY(HERO,
        "这次我不一个人去。把灯留给两岸的人，我们一起下去，问清谁在地下等了十年。"),
    SAY(NARRATOR,
        "灯纹“雪”已收入灯簿。救援顺序、剩余炭火、闻舟入队与同行选择已经记录。第八章完，下一程《地脉回声》。"),
};
static const h2_xianxia_rpg_line_t k_jiangli[] = {
    SAY(JIANGLI,
        "我和你守桥。每回合给较弱的那堆灯补八点；你护灯时，我正好能接上灯芯。闻舟留守伤员。同行选择已记录。"),
};
static const h2_xianxia_rpg_line_t k_wenzhou[] = {
    SAY(WENZHOU,
        "我和你守桥。每次墨影扑灯，我先削去八点力道；你再护灯。江篱留守伤员。同行选择已记录。"),
};
static const h2_xianxia_rpg_line_t k_no_pill[] = {
    SAY(NARRATOR,
        "没有丹药，未消耗任何物资。先队留下的炭仍在，点“用炭暖身”即可救人。"),
};
static const h2_xianxia_rpg_line_t k_recall_give[] = {
    SAY(WEN_GRANDMA,
        "你说过来还灯。今日我看见你把手递到另一岸了。灯簿里这一笔，我替你们记着。"),
};
static const h2_xianxia_rpg_line_t k_recall_take[] = {
    SAY(WEN_GRANDMA,
        "那天你来问答案，今天你来接人。无论先说了什么，这一路做过的事，我都看在眼里。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),       SCRIPT(k_help, NONE),
    SCRIPT(k_east, NONE),        SCRIPT(k_west, NONE),
    SCRIPT(k_rest, NONE),        SCRIPT(k_light, NONE),
    SCRIPT(k_block, NONE),       SCRIPT(k_first_east, NONE),
    SCRIPT(k_first_west, NONE),  SCRIPT(k_injury, NONE),
    SCRIPT(k_rescued, NONE),     SCRIPT(k_need_rescue, NONE),
    SCRIPT(k_battle, C8_BATTLE), SCRIPT(k_retry, NONE),
    SCRIPT(k_reveal, ENDING),    SCRIPT(k_jiangli, NONE),
    SCRIPT(k_wenzhou, NONE),     SCRIPT(k_no_pill, NONE),
    SCRIPT(k_recall_give, NONE), SCRIPT(k_recall_take, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 8,
    .first_stage = H2_XIANXIA_RPG_STAGE_C8_ORDER,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C8_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_SNOW_CAMP,
    .map_count = 3,
    .start_map = H2_XIANXIA_RPG_MAP_SNOW_CAMP,
    .rest_map = H2_XIANXIA_RPG_MAP_SNOW_CAMP,
    .start_x = 7,
    .start_y = 6,
    .rest_x = 7,
    .rest_y = 6,
    .default_level = 10,
    .default_pills = 3,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C8_INTRO,
    .label = "第八章",
    .name = "第八章 · 雪岭断桥",
    .title = "雪岭断桥",
    .ending = {"两岸伸手，双灯越过断桥。", "雪下旧井，传来唤名回声。"},
    .next = "第八章 完 · 下一章《地脉回声》",
    .recap =
        "镜湖照见双向传灯的古约，也照见十年前的断桥与独自改阵的守灯人。最后一幕却是此刻：陶伯与闻婆的两支灯队在雪岭遇险。两人立刻赶往断桥。",
};
char h2_xianxia_rpg_c8_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (map < H2_XIANXIA_RPG_MAP_SNOW_CAMP ||
      map > H2_XIANXIA_RPG_MAP_SNOW_CAVE || x < 0 || y < 0 ||
      x >= H2_XIANXIA_RPG_MAP_COLS || y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_SNOW_CAMP][y][x];
}
const char* h2_xianxia_rpg_c8_map_name(h2_xianxia_rpg_map_t map) {
  return k_names[map - H2_XIANXIA_RPG_MAP_SNOW_CAMP];
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c8_info(void) {
  return &k_info;
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c8_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C8_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_COUNT
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C8_FIRST]
             : NULL;
}
bool h2_xianxia_rpg_c8_walkable(const h2_xianxia_rpg_game_t* game,
                                h2_xianxia_rpg_map_t map, char tile) {
  const uint32_t blocked = map == H2_XIANXIA_RPG_MAP_ICE_CLIFF
                               ? H2_XIANXIA_RPG_FACT_C8_EAST_BLOCK
                               : H2_XIANXIA_RPG_FACT_C8_WEST_BLOCK;
  return tile == '.' || tile == '=' || tile == 'i' ||
         (tile == 's' && !(game->facts & blocked) &&
          (map != H2_XIANXIA_RPG_MAP_SNOW_CAVE ||
           (game->facts & H2_XIANXIA_RPG_FACT_C8_LIGHT))) ||
         (tile == 'f' && (game->facts & H2_XIANXIA_RPG_FACT_C8_LIGHT));
}
size_t h2_xianxia_rpg_c8_strings(const char** out, size_t capacity) {
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
