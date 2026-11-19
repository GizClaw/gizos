/* Mirror Lake: two places, two eras. b is the old bridge, d the lit
 * present passage, p the past ford revealed by the present water wheel.
 * Mirrors stand beside permanent landings; only those landings time-shift. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_maps[2][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"###############", "#~~~~~~#~~~~~~#", "#......#......#", "#..M...#...L..#",
     "#......b......#", "#......#...M..#", "#......d......E", "#......#......#",
     "#~~~~~~#~~~~~~#", "###############"},
    {"###############", "#......#......#", "#......p...A..#", "#..M.W.#......#",
     "E......d......#", "#......#...M.H#", "#......#......#", "#......#......#",
     "#~~~~~~#~~~~~~#", "###############"},
};
static const char* const k_names[] = {"镜湖岸·湖心亭", "湖底镜廊·立约室"};
static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "药绳还系在灯柄上，江篱已能自己走到湖岸。镜湖没有倒映天空，水里却亮着一场早已散去的灯会。"),
    SAY(JIANGLI,
        "荀七娘救回了我的命，系灯还没解开。闻舟让我们来看谁先失约，可我想先看看，两岸原来怎样一起生活。"),
    SAY(HERO, "同一座亭，水里是新木，岸上却断了。爹留下的结，也许要从两边看。"),
    SAY(NARRATOR,
        "当前是今时。点左侧标着“镜石”的石面，进入昔时灯会；过中间旧栈桥，点右上“灯龛”帮童子安灯。右侧也有镜石。"),
    SAY(NARRATOR,
        "镜石会回满气血、记录进度，并将人放在它旁边的共有地面。今昔没有时限，所有问题都能回答，没有答错卡关。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "镜湖岸：左镜石入昔时，过中排木桥，点右上灯龛安灯；右镜石回今时，蓝色暗路被照亮，点右下“镜廊”进入湖底。"),
    SAY(NARRATOR,
        "湖底镜廊：今时先点左侧“水轮”理水，再点镜石入昔时。走上排浅水路，点右上“古约”。今时中排暗路与昔时上排浅路都能往返。"),
    SAY(NARRATOR,
        "古约读完后出现三道问心。每次选择都立即记录，再听江篱的回应。答完可点右侧“心影”开战；镜石随时歇息，败后也回到它旁边。"),
    SAY(JIANGLI,
        "心影总慢一回合：你这一回合防御，下一回合它就防御，不会伤人。那时用剑诀或合击；再下一回合记得防住它学来的剑。"),
};
static const h2_xianxia_rpg_line_t k_past[] = {
    SAY(NARRATOR,
        "昔时·两岸灯会。旧栈桥重新连起，水面浮着暖金色的灯。位置、时代与已做的事已记录。镜石旁始终有路。"),
    SAY(JIANGLI, "这里没有上游的席、下游的席。人们把空位留给还没来的人。"),
};
static const h2_xianxia_rpg_line_t k_now[] = {
    SAY(NARRATOR,
        "今时·镜湖遗迹。旧桥又成断木，蓝色石路沿湖底伸去。昔时安下的灯仍在；理过的水也不会再堵。气血灵力恢复，已记录。"),
};
static const h2_xianxia_rpg_line_t k_lamp[] = {
    SAY(CHILD,
        "这是归山的灯，不是往下游放的灯。我够不到石龛，你能帮我放好么？娘说，山上的人也在等我们。"),
    SAY(HERO, "原来溪灯下山以后，还要有人把灯送上来。给，我扶着，你来放。"),
    SAY(NARRATOR,
        "童子将归灯安进石龛。昔时的灯与今时的微光重合，照开湖岸下排和镜廊中排的暗路。灯已安好，检查点已记录。"),
    SAY(JIANGLI,
        "原来这不是谁欠谁一盏灯。有人送来，也有人接着送回去。点右侧镜石回今时，我们沿亮路入镜廊。"),
};
static const h2_xianxia_rpg_line_t k_water[] = {
    SAY(NARRATOR,
        "青崖拨正今时的水轮，积水沿两道旧槽分开。镜面水纹退去，昔时石室的上排浅路也显出完整石阶。理水结果已记录。"),
    SAY(JIANGLI,
        "我们没改掉已经发生的事，只是擦去了蒙住旧影的水。原来一条路看不清，可以换一边照它。"),
};
static const h2_xianxia_rpg_line_t k_treaty[] = {
    SAY(NARRATOR,
        "昔时的石室没有宝座，只有两岸人各自带来的灯。古约写着：春放溪灯下山，冬迎归灯上山；灯沿水去，人越岭归。"),
    SAY(ELDER,
        "上游守水头，下游守归路。哪一岸有难，另一岸暂替一程；不许借灯为名，把别人的一生抵进阵里。"),
    SAY(JIANGLI,
        "这才是原来的灯阵……我是被救下的人，不该一直做一根无人问过的灯芯。"),
    SAY(HERO, "可后来为何只剩抽光？为何十年的灯簿都空了？"),
    SAY(NARRATOR,
        "古约已记入灯簿。湖心传来三问：它不会判对错，只照出你愿意承担什么。下方选一个回答，江篱会说出她的想法。"),
};
static const h2_xianxia_rpg_line_t k_need_past[] = {
    SAY(NARRATOR,
        "今时只剩石上的空痕。点镜石进入昔时，才能看见安灯的人和立约的字。操作没有消耗。"),
};
static const h2_xianxia_rpg_line_t k_need_now[] = {
    SAY(NARRATOR,
        "昔时的水轮是完整的旧影，不能拨动。点镜石回今时理水；进入镜廊也要循今时亮起的路。"),
};
static const h2_xianxia_rpg_line_t k_need_lamp[] = {
    SAY(NARRATOR,
        "暗处没有路。先在昔时过木桥，点右上灯龛安灯，再回今时；灯会照开同地的暗路。"),
};
static const h2_xianxia_rpg_line_t k_need_treaty[] = {
    SAY(NARRATOR,
        "先在今时拨动水轮，再入昔时走上排浅路读古约。三道问心答完，才可与心影相见。"),
};
static const h2_xianxia_rpg_line_t k_answer1a[] = {
    SAY(HERO, "若眼前只能保一岸，我先保竹溪。那里有等我回去的人。"),
    SAY(JIANGLI,
        "你肯说出偏心，比假装没有好。但青萍渡也有人等我。你护故乡的时候，我会提醒你看看水的另一边。"),
    SAY(NARRATOR,
        "已记录：先保竹溪。江篱保留异议，信任降低一格，最低为零；仍会同行，后续道路全部开放。"),
};
static const h2_xianxia_rpg_line_t k_answer1b[] = {
    SAY(HERO, "我去找另一条路。若只许选一岸，也许是题目漏掉了可以一起做的事。"),
    SAY(JIANGLI,
        "我愿意陪你找。但别把这句话当成谁都不会受伤的保证。找不到时，也要把难处告诉我们。"),
    SAY(NARRATOR,
        "已记录：另寻共守之路。江篱愿意分担，信任增加一格，最高为三。回答会留到归途。"),
};
static const h2_xianxia_rpg_line_t k_answer2a[] = {
    SAY(HERO,
        "若旧阵出自沈家，我先替父亲还债。救人的手也可能伤人，不能只记好的一面。"),
    SAY(JIANGLI,
        "你可以先伸手，可你不是债契。别替十年前的人把今天的自己也押进去。我会拉住你。"),
    SAY(NARRATOR,
        "已记录：自己先还。江篱担心你独担，信任降低一格，最低为零；这份担心不会阻止同行。"),
};
static const h2_xianxia_rpg_line_t k_answer2b[] = {
    SAY(HERO,
        "我会把知道的告诉两岸，请大家一起商量怎样还。真相不能藏在我一个人的剑鞘里。"),
    SAY(JIANGLI,
        "那也让我开口。我不是沈家的证物，我有自己的话，要对救过我的人、受过伤的人说。"),
    SAY(NARRATOR,
        "已记录：邀两岸共担。江篱得到发言的位置，信任增加一格，最高为三。"),
};
static const h2_xianxia_rpg_line_t k_answer3a[] = {
    SAY(HERO, "若解系有险，我想先替你挡住。至少这一回，不要再叫你受伤。"),
    SAY(JIANGLI,
        "我知道你是好意。但先问我，好么？寒谷里我说过，我还想自己撑船回去。护着我，也给我留握篙的位置。"),
    SAY(NARRATOR,
        "已记录：先替她挡险。江篱提出自己的边界，信任降低一格，最低为零。她仍愿意并肩面对心影。"),
};
static const h2_xianxia_rpg_line_t k_answer3b[] = {
    SAY(HERO, "把每条路的代价告诉你，由你决定。我会在旁边，但不会替你活。"),
    SAY(JIANGLI,
        "好。等真有那一天，我会自己说。现在我选陪你走到镜廊尽头——也是为了我自己的活路。"),
    SAY(NARRATOR,
        "已记录：由她决定。江篱确认同行是自己的选择，信任增加一格，最高为三。三问已答完，点右侧心影。"),
};
static const h2_xianxia_rpg_line_t k_battle[] = {
    SAY(NARRATOR,
        "镜中走出另一个青崖，提剑的动作总慢一拍。心影只复现你上一回合实际用出的指令；开场是防御，不会攻击。"),
    SAY(JIANGLI,
        "看上方提示。它防御时不会伤你，趁这空隙出剑；它学会剑诀或合击时，你先防御，下一回合又能安心出手。"),
    SAY(NARRATOR,
        "它学到丹药会给自己回少量气血，但不会动你的药袋。不能逃跑；败后回右镜石，三问与开路结果保留，心影重新从防御开始。战中刷新从战前检查点重试。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {
    SAY(JIANGLI,
        "它只会重复，而我们可以改变。先歇口气，再试一次。它出剑时防御，下一回合它也会停手。"),
    SAY(NARRATOR,
        "已回右镜石，气血灵力恢复。问心回答和两态通路保留；点心影重试。丹药不是必需品，防御能回灵力。检查点已记录。"),
};
static const h2_xianxia_rpg_line_t k_reveal[] = {
    SAY(NARRATOR,
        "心影收剑，镜纹浮出水面。三道回答仍在灯簿里，它们不是得分，是一同走下去时可以重提的话。"),
    SAY(NARRATOR,
        "镜廊尽头映出十年前的雪夜：归灯队困在断桥下，风雪盖住了上山的足印。没有人故意失约，但归灯终究没有送到。"),
    SAY(JIANGLI,
        "传灯断了……墨溪发黑，墨蛟才会破封。闻舟说的失约，不是简单的谁先背弃谁。"),
    SAY(NARRATOR,
        "雾里，一个看不清脸的守灯人独自走进山腹。他把剑插入阵枢，灯光转了方向：上游亮起，下游的窗一扇扇暗下。"),
    SAY(HERO, "那人的握剑手势……我见过。可这只是旧影，还不能替他把所有话说完。"),
    SAY(JIANGLI,
        "他为何独自改阵，现在又在哪里？我们得当面问。还有我的灯，不能只靠猜。"),
    SAY(NARRATOR,
        "湖面忽然照出此刻的雪岭。陶伯背着竹溪的灯往下走，闻婆带着照水的人往上行。两队隔桥举灯，桥索却一根根绷断。"),
    SAY(HERO,
        "陶伯读了我们带回的消息，闻婆也终于肯送灯了。两岸正在靠近，不能让他们再错过一次。"),
    SAY(JIANGLI, "往雪岭！这次先让人活着相见，再让灯一起过桥。"),
    SAY(NARRATOR,
        "灯纹“镜”已记入灯簿。双向传灯的古约、三道回答和雪岭险情均已记录。第七章完，下一程是断桥两侧的救援。"),
};
static const h2_xianxia_rpg_line_t k_recall_give[] = {
    SAY(JIANGLI,
        "你在照水说过要还灯。现在知道了，它原本就要在两岸来回。我会记得你的回答，也会看我们下一步怎样走。"),
};
static const h2_xianxia_rpg_line_t k_recall_take[] = {
    SAY(JIANGLI,
        "在照水，你想先要一个答案。可你还是先救了闻婆。现在镜湖让我们再答一次，我想听你此刻怎么想。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),
    SCRIPT(k_help, NONE),
    SCRIPT(k_past, NONE),
    SCRIPT(k_now, NONE),
    SCRIPT(k_lamp, NONE),
    SCRIPT(k_water, NONE),
    SCRIPT(k_treaty, NONE),
    SCRIPT(k_need_past, NONE),
    SCRIPT(k_need_now, NONE),
    SCRIPT(k_need_lamp, NONE),
    SCRIPT(k_need_treaty, NONE),
    SCRIPT(k_answer1a, C7_FEEDBACK),
    SCRIPT(k_answer1b, C7_FEEDBACK),
    SCRIPT(k_answer2a, C7_FEEDBACK),
    SCRIPT(k_answer2b, C7_FEEDBACK),
    SCRIPT(k_answer3a, C7_FEEDBACK),
    SCRIPT(k_answer3b, C7_FEEDBACK),
    SCRIPT(k_battle, C7_BATTLE),
    SCRIPT(k_retry, NONE),
    SCRIPT(k_reveal, ENDING),
    SCRIPT(k_recall_give, NONE),
    SCRIPT(k_recall_take, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 7,
    .first_stage = H2_XIANXIA_RPG_STAGE_C7_LAMP,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C7_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_MIRROR_SHORE,
    .map_count = 2,
    .start_map = H2_XIANXIA_RPG_MAP_MIRROR_SHORE,
    .rest_map = H2_XIANXIA_RPG_MAP_MIRROR_SHORE,
    .start_x = 3,
    .start_y = 4,
    .rest_x = 3,
    .rest_y = 4,
    .default_level = 9,
    .default_pills = 3,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C7_INTRO,
    .label = "第七章",
    .name = "第七章 · 镜湖问心",
    .title = "镜湖问心",
    .ending = {"昔日共守，照见十年断灯。", "心有回声，两岸同赴雪岭。"},
    .next = "第七章 完 · 下一章《雪岭断桥》",
    .recap =
        "寒谷救治让江篱脱险，荀七娘认出竹溪沈家的“以剑系灯”。为找解系的活路，两人循闻舟的信来到镜湖，寻找两岸立阵的旧影。",
};
char h2_xianxia_rpg_c7_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (map < H2_XIANXIA_RPG_MAP_MIRROR_SHORE ||
      map > H2_XIANXIA_RPG_MAP_MIRROR_HALL || x < 0 || y < 0 ||
      x >= H2_XIANXIA_RPG_MAP_COLS || y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_MIRROR_SHORE][y][x];
}
const char* h2_xianxia_rpg_c7_map_name(h2_xianxia_rpg_map_t map) {
  return map == H2_XIANXIA_RPG_MAP_MIRROR_SHORE ? k_names[0] : k_names[1];
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c7_info(void) {
  return &k_info;
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c7_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C7_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_C8_FIRST
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C7_FIRST]
             : NULL;
}
bool h2_xianxia_rpg_c7_walkable(const h2_xianxia_rpg_game_t* game, char tile) {
  const bool past = (game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST) != 0;
  return tile == '.' || (tile == 'b' && past) ||
         (tile == 'd' && !past &&
          (game->facts & H2_XIANXIA_RPG_FACT_C7_LAMP)) ||
         (tile == 'p' && past && (game->facts & H2_XIANXIA_RPG_FACT_C7_WATER));
}
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c7_feedback(
    const h2_xianxia_rpg_game_t* game) {
  uint32_t question =
      game->stage > H2_XIANXIA_RPG_STAGE_C7_QUESTION1
          ? (uint32_t)(game->stage - H2_XIANXIA_RPG_STAGE_C7_QUESTION1 - 1)
          : 0;
  if (question > 2u) question = 2u;
  return (h2_xianxia_rpg_script_id_t)(H2_XIANXIA_RPG_SCRIPT_C7_ANSWER1A +
                                      2u * question +
                                      ((game->facts >>
                                        (H2_XIANXIA_RPG_C7_ANSWERS_SHIFT +
                                         question)) &
                                       1u));
}
size_t h2_xianxia_rpg_c7_strings(const char** out, size_t capacity) {
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
