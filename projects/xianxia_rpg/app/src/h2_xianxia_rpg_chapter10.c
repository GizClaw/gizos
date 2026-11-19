/* Final chapter: shared lamps replace a solitary wick. Earlier maps remain
 * untouched; these five states borrow their tiles and pixel artwork. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_names[] = {"竹溪村·共守之夜", "墨溪古祠·浊潮", "竹溪祭台·归灯", "青萍渡·新航", "照水灯楼·同簿"};
const uint8_t h2_xianxia_rpg_c10_helpers[4] = {H2_XIANXIA_RPG_ACTOR_ELDER, H2_XIANXIA_RPG_ACTOR_WEN_GRANDMA, H2_XIANXIA_RPG_ACTOR_DAOIST, H2_XIANXIA_RPG_ACTOR_FERRYMASTER};
const char* const h2_xianxia_rpg_c10_destinations[3] = {"古祠", "祭台", "下游"};
const char* const h2_xianxia_rpg_c10_hints[3][3] = {
  {"墨浪压低：防御，蓄满默契", "蛟角聚光：合击，截断浊潮", "鳞隙露出：攻击，松开墨环"},
  {"守位：防御，让灯路相接", "传火：合击，把光送向对岸", "三灯同亮：结阵，完成一轮"},
  {"河心开路：攻击，疏通回流", "两岸接力：防御，等归灯来", "溪灯归灯：结阵，交还灯芯"},
};
const char* const h2_xianxia_rpg_c10_text[H2_XIANXIA_RPG_C10_TEXT_COUNT] = {
  "轻触屏幕，进入最终章《长明归途》", "共守灯簿", "歇息", "古祠", "祭台", "往前走",
  "结阵", "%s · 点此换人", "援手 古%u 祭%u 下%u", "请%s带队", "各处都有村民；援手加强恢复。", "%s赴%s",
  "接力成功，灯路更亮了。", "阵位未合，浊潮反扑；看上方提示再试。", "此段已稳，下一检查点已记录。",
  "阵位移向%s：点右上换人", "第%u段 · 接力%u/2 · 第%u拍/3", "先东援手：首次失误保住阵位", "先西援手：接力额外恢复气血",
  "解下系灯", "自愿留火", "她来选", "江篱解系，归途号自在行船。", "江篱留火，归途号轮值渡灯。", "十章完 · 轻触回标题",
};

static const h2_xianxia_rpg_line_t k_intro[] = {
  SAY(NARRATOR, "竹溪的屋檐还是旧模样，井旁却多了照水的灯架、青萍渡的船篙。陶伯把长桌搬到路中央，桌上两册旧灯簿终于合在一起。"),
  SAY(HERO, "古祠、祭台、下游，要在同一夜各守各的灯。不是请你们替我爹留下，是请大家把原本一起做的事接回来。"),
  SAY(WEN_GRANDMA, "照水的人已经在下游生火。竹溪也有人守古祠和祭台。我们四个带来的援手，你来安排，哪里都去得。"),
  SAY(NARRATOR, "点陶伯、闻婆、白鹤道长、鲁把头，选派往古祠、祭台或下游。每人加强对应战段的恢复，没有错误派法；出发前可改派。四人都安排好，再点右侧古祠。"),
  SAY(JIANGLI, "井旁可歇息。下方共守灯簿能随时重看规则。哪怕是第一次来，今夜要做的事，我们一起从头学。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
  SAY(NARRATOR, "村中四人各选一处支援。古祠加强第一段，祭台加强第二段，下游加强第三段；每个援手让正确接力多恢复八气血。各处本就有守灯人，所以可以自由分配。"),
  SAY(JIANGLI, "第一段看招：墨浪防御，聚光合击，露隙攻击。防御蓄满默契并回灵，合击只耗七灵。指令会亮边，不用记顺序。"),
  SAY(WENZHOU, "后两段每轮三拍。照上方提示行动，第三拍点结阵。阵位换人时点右上队友框，占一回合；新队友立刻就位。每段都要完成两轮。"),
  SAY(NARRATOR, "正确应对可恢复气血，无需刷怪或用药。失误会受伤，丹药也占一回合。败后或刷新回当前段前，已赢的段不重打；已用丹药在败后保留。"),
};
static const h2_xianxia_rpg_line_t k_need[] = {SAY(ELDER, "四队援手还没全领到去处。点村里的四个人，选好三处中的一处，再往古祠去。不必赶，灯会等人齐。")};
static const h2_xianxia_rpg_line_t k_shrine[] = {
  SAY(NARRATOR, "古祠的石阶被墨浪一遍遍洗过。青崖摸到怀里的铜牌，青萍渡、丙三，字缝里仍有第一夜留下的黑泥。"),
  SAY(DRAGON, "十年前那一剑，又是沈家人。解开它的人，可不是你们竹溪村的！"),
  SAY(WENZHOU, "是我。丙三号是我偷的，断符剑也是我落的。我以为叫另一岸痛，自己的灯就会亮。可水淹进来，不认谁欠谁。"),
  SAY(HERO, "这块铜牌不再是找一个人偿命的凭据。它提醒我们，船本来就该往返两岸。今夜我不来补那一剑，我来打开归路。"),
  SAY(NARRATOR, "第一段：墨浪压低时防御，蛟角聚光时合击，鳞隙露出时攻击。上方会逐拍提示，推荐按钮亮边。两次循环松开墨环；点完对白开战，战前已经记录。"),
};
static const h2_xianxia_rpg_line_t k_array[] = {
  SAY(NARRATOR, "墨环松动，古祠的水退到石阶以下。远处三座祭灯同时亮起。白鹤道长守住祠门，三人沿旧灯路回到祭台。"),
  SAY(DAOIST, "我曾只叫你镇住墨蛟，却没问阵另一头的人。今夜古祠交给我们，你不必回头替每个人挡。"),
  SAY(JIANGLI, "第二段，先让我站传灯位。防御接路，合击传火，第三拍结阵。墨蛟会把阵位推向闻舟，到时点右上换他接住，再做一轮。"),
  SAY(NARRATOR, "第二段检查点已记录。两轮结阵各三拍，提示一直在上方。选错仍可重试；气血耗尽只重来这一段。"),
};
static const h2_xianxia_rpg_line_t k_return[] = {
  SAY(NARRATOR, "祭台灯路接通，井底的回声第一次有了脚步声。可最后一股浊潮逆流而上，沈怀川每走一步，墨溪便抬高一寸。"),
  SAY(WENZHOU, "第三段由我先开河心，攻击疏水，防御等归灯，最后结阵。第二轮江篱接灯，仍照同样的三拍。"),
  SAY(HERO, "各守一处，也照看下一处。谁站累了就换人，没有一个阵位必须守一辈子。"),
  SAY(NARRATOR, "最终段检查点已记录。先前雪岭先东救援的人会替第一次失误补阵位；先西的灯队会在每次正确接力时多恢复八气血。两边都已平安抵达。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {SAY(NARRATOR, "援手把三人拉回本段灯台。气血灵力已满，分派和前面赢过的战段都保留，已用丹药不返还。点中央阵心重试；先看上方招式与亮边指令，换人请点右上。")};
static const h2_xianxia_rpg_line_t k_reunion[] = {
  SAY(NARRATOR, "最后一轮灯从下游回到山上。墨色剥落，水中只剩一条透亮的长影。它绕过石阶，没有再扑向任何一盏灯。"),
  SAY(DRAGON, "我曾是这条溪的河灵。我恨的不是沈家，是所有浊水都只许流进我一人身上。如今水有去处，我也可以回河里了。"),
  SAY(NARRATOR, "灯路不再汇到一个胸口，而是穿过无数只捧灯的手。沈怀川跌坐在祭台边，掌心有温度，呼吸却很轻。柳婆婆把热粥塞给他，先不准任何人问话。"),
  SAY(FATHER, "青崖，我听得见你每年在祠前说的话。长高了，学会剑了，柳婆婆又叫你吃饭……我记得。只是一直没能答。"),
  SAY(HERO, "那就慢慢答。先回家。你留下的担子，我没有接；我们把它分回了两岸。"),
  SAY(FATHER, "江篱，系灯如今可以解开。两岸的灯不再向你索火。留也好，走也好，都不用报答我。"),
  SAY(JIANGLI, "我想先听你的心意，然后把最后一句留给我。解系，我仍可以撑船救人；留一缕火，也只做轮值的守灯人。两条都是自己的路。"),
};
static const h2_xianxia_rpg_line_t k_release[] = {
  SAY(JIANGLI, "我选解下它。以后我渡人，是因为我会撑船，不是因为身体里藏着一盏灯。也许有一天会远行，累了还回青萍渡。"),
  SAY(NARRATOR, "那缕火从江篱掌心飞进众人的溪灯。她按了按胸口，那里只剩自己的心跳。她笑着给旧船换了名字：归途。"),
};
static const h2_xianxia_rpg_line_t k_keep[] = {
  SAY(JIANGLI, "我选留一缕火，但不再把自己系住。我愿做青萍渡这一季的守灯人；下一季交班，想远行时也会出发。"),
  SAY(NARRATOR, "系灯化作可以解下的小灯，挂在船头。江篱把丙三的旧牌收进船舱，写下新名：归途。灯随船走，人有去处。"),
};
static const h2_xianxia_rpg_line_t k_dock[] = {
  SAY(NARRATOR, "青萍渡天亮了。归途号第一次不用顶着雾出航，船上有竹溪的竹篮，也有照水新烤的饼。"),
  SAY(FERRYMASTER, "桂嫂把茶棚开回老地方，芦叟也来帮忙修船。以后去一岸收灯，得在另一岸留个能接班的人。"),
  SAY(JIANGLI, "船牌终于不必证明我没有做过什么了。青崖，下一趟想往哪儿走，到码头叫我。"),
};
static const h2_xianxia_rpg_line_t k_tower[] = {
  SAY(NARRATOR, "照水灯楼把两册灯簿抄成同样的两份，一份留城，一份送往竹溪。谁家当值，谁家休息，都记得清楚。"),
  SAY(WEN_GRANDMA, "房子要慢慢修，旧账也不会一夜不疼。但今天谁做了什么，不再只写在一岸的簿上。"),
  SAY(WENZHOU, "石砚在剑阁等我回去修断符，我会去，也会把那一剑的事说清。之后我来做归灯人，第一程送到竹溪。不是还完便走，是以后还会来。"),
  SAY(NARRATOR, "寒谷的荀七娘送来药方，药舟大夫沿溪探病。沈怀川需要很久才能走稳，沿岸却不再有人催他回井底。"),
};
static const h2_xianxia_rpg_line_t k_festival[] = {
  SAY(NARRATOR, "又到溪灯节。阿禾抱着一盏比自己还宽的竹灯，蹲在第一章那座小桥边。沈怀川坐在岸上，慢慢教他把纸角折平。"),
  SAY(CHILD, "我说过，要放最大的一盏！这回里面有大家写的名字，爹说太重了，我就多扎了一层竹子。"),
  SAY(HERO, "大也要有人接。你看，下游的灯已经往山上来了。"),
  SAY(NARRATOR, "溪灯顺水下山，归灯沿路上山。江篱在船头回望，闻舟举灯相应。陶伯和闻婆隔着桥喊交班，白鹤道长笑着把剑挂回墙上。"),
  SAY(FATHER, "青崖，回家吃饭。"),
  SAY(NARRATOR, "没有人再留在灯里。那条走了十章的长路，终于把人送回了家。《竹溪仙缘》全篇完。结局和十章记录已经保存，轻触结算可回标题。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {SAY(NARRATOR, "热茶与药汤沿灯路传来。气血灵力已恢复，当前阶段和分派已经记录。没有添药，也不需要刷怪。")};
static const h2_xianxia_rpg_line_t k_give[] = {SAY(JIANGLI, "你在镜湖说，要找另一条路，把选择留给同行的人。今夜我看见了：另一条路，是大家都能轮流回家。")};
static const h2_xianxia_rpg_line_t k_take[] = {SAY(JIANGLI, "你在镜湖惦记竹溪，我懂那份怕失去家人的心。如今你也在等另一岸的灯，我愿和这样的你一起把路走完。")};
static const h2_xianxia_rpg_script_t k_scripts[] = {
  SCRIPT(k_intro, NONE), SCRIPT(k_help, NONE), SCRIPT(k_need, NONE),
  SCRIPT(k_shrine, C10_BATTLE), SCRIPT(k_array, C10_BATTLE), SCRIPT(k_return, C10_BATTLE),
  SCRIPT(k_retry, NONE), SCRIPT(k_reunion, C10_CHOICE), SCRIPT(k_release, NONE), SCRIPT(k_keep, NONE),
  SCRIPT(k_dock, NONE), SCRIPT(k_tower, NONE), SCRIPT(k_festival, ENDING), SCRIPT(k_rest, NONE),
  SCRIPT(k_give, NONE), SCRIPT(k_take, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
  .number = 10, .first_stage = H2_XIANXIA_RPG_STAGE_C10_ARRANGE, .cleared_stage = H2_XIANXIA_RPG_STAGE_C10_CLEARED,
  .first_map = H2_XIANXIA_RPG_MAP_HOME_VILLAGE, .map_count = 5,
  .start_map = H2_XIANXIA_RPG_MAP_HOME_VILLAGE, .rest_map = H2_XIANXIA_RPG_MAP_HOME_VILLAGE,
  .start_x = 7, .start_y = 5, .rest_x = 7, .rest_y = 5, .default_level = 13, .default_pills = 3,
  .intro_script = H2_XIANXIA_RPG_SCRIPT_C10_INTRO,
  .label = "第十章", .name = "第十章 · 长明归途", .title = "长明归途",
  .ending = {"溪灯下山，归灯上山。", "两岸共守，每个人都有归途。"},
  .next = "《竹溪仙缘》全篇完",
  .recap = "地下灯阵里，沈青崖终于找到独守十年的父亲。江篱的系灯不是债，闻舟也决定留下承担。三人约定：古祠、竹溪祭台、下游渡口同时共守，恢复双向传灯。如今灯队已经过桥，他们回到竹溪，准备最后一夜。",
};
h2_xianxia_rpg_map_t h2_xianxia_rpg_c10_base_map(h2_xianxia_rpg_map_t map) {
  static const h2_xianxia_rpg_map_t bases[] = {H2_XIANXIA_RPG_MAP_VILLAGE, H2_XIANXIA_RPG_MAP_SHRINE, H2_XIANXIA_RPG_MAP_LANTERN_ALTAR, H2_XIANXIA_RPG_MAP_DOCK, H2_XIANXIA_RPG_MAP_WEN_TOWER};
  return map >= H2_XIANXIA_RPG_MAP_HOME_VILLAGE && map <= H2_XIANXIA_RPG_MAP_HOME_TOWER ? bases[map - H2_XIANXIA_RPG_MAP_HOME_VILLAGE] : H2_XIANXIA_RPG_MAP_VILLAGE;
}
char h2_xianxia_rpg_c10_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= 15 || y >= 10) return '#';
  if (map == H2_XIANXIA_RPG_MAP_HOME_VILLAGE) {
    if (y == 3) { if (x == 4) return 'a'; if (x == 6) return 'b'; if (x == 9) return 'c'; if (x == 11) return 'd'; }
    if (x == 13 && y == 4) return 'E';
    if (x == 7 && y == 4) return 'R';
  } else {
    if (x == 7 && y == 3) return 'F';
    if (x == 5 && y == 5) return 'R';
    if (y >= 3 && y <= 6 && x >= 5 && x <= 9) return '_';
  }
  const char t = h2_xianxia_rpg_tile(h2_xianxia_rpg_c10_base_map(map), x, y);
  /* Old chapter objects are scenery in the finale, never old quest triggers. */
  return t == 'b' ? '=' : t == 'W' || t == 'S' || t == '3' || t == 'w' ? '_' : t;
}
const char* h2_xianxia_rpg_c10_map_name(h2_xianxia_rpg_map_t map) { return k_names[map - H2_XIANXIA_RPG_MAP_HOME_VILLAGE]; }
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c10_info(void) { return &k_info; }
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c10_script(h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C10_FIRST && id < H2_XIANXIA_RPG_SCRIPT_COUNT ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C10_FIRST] : NULL;
}
size_t h2_xianxia_rpg_c10_strings(const char** out, size_t capacity) {
  size_t count = 0;
#define ADD(s) do { if (count < capacity) out[count] = (s); ++count; } while (0)
  for (size_t i = 0; i < ARRAY_SIZE(k_names); ++i) ADD(k_names[i]);
  for (size_t i = 0; i < ARRAY_SIZE(k_scripts); ++i)
    for (size_t j = 0; j < k_scripts[i].count; ++j) ADD(k_scripts[i].lines[j].text);
  for (size_t i = 0; i < H2_XIANXIA_RPG_C10_TEXT_COUNT; ++i) ADD(h2_xianxia_rpg_c10_text[i]);
  for (size_t i = 0; i < 3; ++i) { ADD(h2_xianxia_rpg_c10_destinations[i]); for (size_t j = 0; j < 3; ++j) ADD(h2_xianxia_rpg_c10_hints[i][j]); }
  ADD(k_info.label); ADD(k_info.name); ADD(k_info.title); ADD(k_info.ending[0]); ADD(k_info.ending[1]); ADD(k_info.next); ADD(k_info.recap);
#undef ADD
  return count;
}
