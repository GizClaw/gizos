/* The same glyph definitions feed the wall, book and door comparison.
 * No prior save or remembered artwork is needed to solve a door. */
#include "h2_xianxia_rpg_story.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const char* const k_maps[4][H2_XIANXIA_RPG_MAP_ROWS] = {
    {"###############", "#~~~~~~=~~~~~~#", "#~~~~~~=~~~~~~#", "#..I...=..R...#",
     "#......=......#", "#~~~~~~=~~~~~~#", "#~~~...D...~~~#", "#~~~...=...~~~#",
     "#~~~~~~~~~~~~~#", "###############"},
    {"###############", "#~~~~~~U~~~~~~#", "#~~~~~~=~~~~~~#", "#~~~...=...~~~#",
     "#~~~...=...~~~#", "#~~~~~~=~~~~~~#", "#..R...=....E.#", "#......=......#",
     "#.............#", "###############"},
    {"###############", "###...###...###", "##.A...#.B...##", "#......#......#",
     "#.............#", "#E....R......F#", "#..C.....D....#", "##.....#.....##",
     "###...###...###", "###############"},
    {"###############", "#####.....#####", "###....F....###", "##...........##",
     "#.............#", "#..R.......R..#", "##.....E.....##", "###.........###",
     "#####.....#####", "###############"},
};
static const char* const k_names[] = {"旧竖井·上层", "旧竖井·下层",
                                      "回声廊·四门", "地下灯阵·阵枢"};
const char* const h2_xianxia_rpg_c9_glyph_names[8] = {"墨", "雾", "剑", "风",
                                                      "潮", "泉", "镜", "雪"};
const char* const h2_xianxia_rpg_c9_room_names[4] = {"雾室", "潮室", "寒室",
                                                     "镜室"};
const char* const h2_xianxia_rpg_c9_phase_hints[4] = {
    "雾相：江篱破雾，再出剑", "潮相：闻舟断潮，防御减伤",
    "寒相：江篱暖灯，防御回灵", "镜相：闻舟读招，防住旧令"};
const char* const h2_xianxia_rpg_c9_text[H2_XIANXIA_RPG_C9_TEXT_COUNT] = {
    "灯簿·八纹对照",
    "八纹对照，门上点变形纹。",
    "上排对照，下排点变形纹",
    "这是正纹，请再对照。",
    "返回",
    "灯簿",
    "路线与队友",
    "换人",
    "%s接替出战，消耗一回合。",
    "江篱·破雾暖灯",
    "闻舟·断潮读镜",
    "试炼 %u/4 · 同行%s",
    "%s · 第%u拍/2",
    "江篱提灯，雾隐解除；本轮可安心出剑。",
    "闻舟读出潮势，截断强潮；本轮扑击减弱。",
    "江篱暖灯，寒意消退，恢复三十二气血。",
    "闻舟读镜，削弱旧令的反击。",
    "队友协击，伤害 %u。",
    "阵中回声造成 %u 点伤害。",
    "雾隐仍在，伤害减弱。换江篱能照开。",
    "四相浊气散去，灯芯暂稳。",
    "此室已通，规则收入灯簿。",
    "灯龛",
    "下井",
    "上井",
    "回声廊",
    "阵枢",
    "刻纹",
    "灯魂",
    "已通",
    "轻触屏幕，继续第九章《地脉回声》",
    "%s · 旧令%s",
    "沿梯下井，循回声入廊",
};

static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "两岸的灯刚越过雪岭，桥下旧井便传来一声唤名。绳梯垂进青黑色的山腹，八种灯纹在井壁依次亮起。"),
    SAY(JIANGLI,
        "青崖，下面还在说：别过来。可这次绳子握在三个人手里。先点左侧刻纹，把井壁的正纹拓进灯簿。"),
    SAY(WENZHOU,
        "我认得剑纹，却从未见过它和雾纹排在一起。这座阵原本让不同的灯互相接力，不是锁住一岸。"),
    SAY(NARRATOR,
        "点有名字的物件会自动走近。灯龛可回满并存档；点下方绳梯口下井。灯簿随时可看，选章直入也能在井壁取得全部线索。"),
};
static const h2_xianxia_rpg_line_t k_book[] = {
    SAY(NARRATOR,
        "井壁八纹已拓入灯簿：墨的三点、雾的横带、剑的直锋、风的回钩、潮的双波、泉的涌口、镜的中线、雪的六枝。"),
    SAY(JIANGLI,
        "门上的三纹混进了一枚变形纹。门页上排会重画本室的两枚正纹，下排才是要点的三枚。比形状，不用凭颜色猜。点错可以重试，不伤人也不扣药。"),
    SAY(NARRATOR,
        "下方“灯簿”可查看八纹。正纹和错误提示都随时可回看；此处已记录。"),
};
static const h2_xianxia_rpg_line_t k_lower[] = {
    SAY(NARRATOR,
        "绳梯穿过一段没有星光的黑暗。井底的灯路向四面散开，又在同一座圆厅汇合。回声越来越清楚：青崖……别替我。"),
    SAY(HERO, "我不是来替你。我来问，能不能把独自撑着的事，交回所有人的手里。"),
};
static const h2_xianxia_rpg_line_t k_hall[] = {
    SAY(NARRATOR,
        "四扇门分别通向雾、潮、寒、镜石室。点门辨纹，再完成一场单相试炼。门与试炼的结果分别保存，点错、战败或刷新都不会关掉已经打开的门。"),
    SAY(WENZHOU,
        "先看上方提示再下令。江篱擅长雾和寒，我擅长潮和镜。战斗里点“换人”，后备的人就接上，消耗一回合。"),
    SAY(JIANGLI,
        "换上来的队友当回合就能援手。四间石室都通过后，右边的阵枢会打开。中央灯龛能歇息。"),
};
static const h2_xianxia_rpg_line_t k_mist[] = {
    SAY(JIANGLI,
        "雾室。浊灵披雾时，攻击只剩三成力。让我提灯破雾，再出剑。防御或换人也是一回合；灯在我手里，这一轮雾就不会重新合上。"),
    SAY(NARRATOR,
        "上方持续显示相位与队友能力。第五个指令是换人，不会逃离试炼。战中刷新回到门前检查点。"),
};
static const h2_xianxia_rpg_line_t k_tide[] = {
    SAY(WENZHOU,
        "潮室。回声每两拍涌一次强潮。我能看出水势最薄的一线，截掉强潮；不换人也可防御挡下，再趁退潮出剑。"),
};
static const h2_xianxia_rpg_line_t k_cold[] = {
    SAY(JIANGLI,
        "寒室。寒意每回合压来，我的灯能暖身，恢复三十二气血。药少时先防御，等暖灯回稳再出剑。防御也会回灵，别把自己留在冰里。"),
};
static const h2_xianxia_rpg_line_t k_mirror[] = {
    SAY(WENZHOU,
        "镜室复现你上一回合的指令。先出剑，再防御接住自己的旧招。我能读镜削弱反击；上一令若是防御或换人，镜面便不会攻击。"),
};
static const h2_xianxia_rpg_line_t k_trial_won[] = {
    SAY(NARRATOR,
        "石室安静下来，一条铜金色灯路通向阵枢。这扇门和它的试炼都已记录，再点已通的门可重听规则，不会重复战斗或领修为。"),
};
static const h2_xianxia_rpg_line_t k_need[] = {
    SAY(NARRATOR,
        "阵路尚未接通。先在井壁取得灯簿，再辨认四门、完成四间试炼。已通的门会亮起，其他门可以任意顺序尝试。"),
};
static const h2_xianxia_rpg_line_t k_father[] = {
    SAY(NARRATOR,
        "圆厅没有灯柱。所有灯路汇到一个人的胸口。那道光转过身，眉眼和青崖藏了十年的记忆一样，却薄得能看见身后的石壁。"),
    SAY(FATHER,
        "青崖。我叫你别过来，是怕你把这根灯芯接过去。十年前桥断，两岸的灯都送不到，墨蛟已醒。阵要一根活灯芯，我不能叫任何人替我。"),
    SAY(HERO, "所以你改了阵，独自留在这里。你说过，一年就回来。"),
    SAY(FATHER,
        "我以为一年能等到两岸重开传灯。可一年又一年，大家只看见自己那岸亮着，就以为路还通。灯芯可以撑住光，撑不住人忘记另一岸。"),
    SAY(JIANGLI, "那我身上的系灯……不是叫我来替你？"),
    SAY(FATHER,
        "那夜你在青萍渡替人捡灯，快被浊水卷走。我把最后一缕能离开的火系给你，是想让你活下去，不是要你还债。以后怎么用它，由你自己选。"),
    SAY(WENZHOU,
        "我割开剑阵，想讨照水的灯，却把积压的浊气放了出来。这一次，我留下接住。"),
    SAY(NARRATOR,
        "灯魂周围四道墨环骤然收紧。雾、潮、寒、镜，每相两回合，循环不息。父亲的声音被吞没，三人同时握紧了手中的灯与剑。"),
};
static const h2_xianxia_rpg_line_t k_battle[] = {
    SAY(HERO,
        "雾、潮、寒、镜，每相两拍。看清上方相位，再选队友。江篱破雾暖灯，闻舟断潮读镜，换人占一回合但援手立即接上。"),
    SAY(NARRATOR,
        "点灯魂开始四相战。战前已记录；败后在本厅灯龛重整，四间试炼不重打。战中刷新从战前重试。"),
};
static const h2_xianxia_rpg_line_t k_retry[] = {
    SAY(NARRATOR,
        "队友把青崖拉回灯龛。气血灵力恢复，已开石门和已通试炼保留，已用丹药不退回。看相位提示，换上擅长此相的人，再试一次。"),
};
static const h2_xianxia_rpg_line_t k_reveal[] = {
    SAY(NARRATOR,
        "四道墨环终于散开。灯魂落回阵心，光不再颤动，却仍不能离开铜金色的灯路。青崖伸手，只触到一阵像溪水一样温的风。"),
    SAY(FATHER,
        "这根灯芯撑不过下一个溪灯节。但还有一个办法：古祠、竹溪祭台、下游渡口，同一夜各点各的灯。溪灯下山，归灯上山，恢复双向传灯。"),
    SAY(HERO, "把灯芯交回整座灯阵。不是再找一个人留下。"),
    SAY(FATHER,
        "是。只是我一离开，积在古祠下的浊气会一起涌向墨蛟。三个阵位都要有人守，任何一岸都不能再独自关门。"),
    SAY(WENZHOU,
        "我去请照水的人。那一剑是我落的，这一次我会站进阵里，把话当面说清。"),
    SAY(JIANGLI,
        "我去青萍渡。我的灯怎么留下，等大家都平安了，再由我来决定。青崖，你去叫竹溪的人，我们在祭台相见。"),
    SAY(HERO, "爹，灯队已经过桥了。这次我们不会叫你再等一年。"),
    SAY(NARRATOR,
        "灯簿八纹齐全，父亲相认与三人同行已经记录。带着两岸救援的顺序、余炭和剩药，回到竹溪，准备最后一夜的共守。第九章完，下一程《长明归途》。"),
};
static const h2_xianxia_rpg_line_t k_rest[] = {
    SAY(NARRATOR,
        "灯龛照住三个人的影子。气血灵力恢复，灯纹、试炼与当前同行者已经记录。歇息不耗药、不耗炭。"),
};
static const h2_xianxia_rpg_line_t k_help[] = {
    SAY(NARRATOR,
        "上层点刻纹取得灯簿，再点下井；下层右侧去回声廊。四扇门上排两枚正纹供对照，下排点变形纹。点错不扣物资，返回或刷新后仍能重新辨认。"),
    SAY(JIANGLI,
        "四间石室通过后点右侧阵枢，听灯魂说，再点他开战。平时点我的身影可换同行者；战中“换人”会占一回合。"),
    SAY(WENZHOU,
        "江篱破雾、暖寒；我断潮、读镜。四相战每相两拍。灯簿随时可看，所有灯龛都能歇息存档。"),
};
static const h2_xianxia_rpg_script_t k_scripts[] = {
    SCRIPT(k_intro, NONE),       SCRIPT(k_book, NONE),
    SCRIPT(k_lower, NONE),       SCRIPT(k_hall, NONE),
    SCRIPT(k_mist, C9_TRIAL),    SCRIPT(k_tide, C9_TRIAL),
    SCRIPT(k_cold, C9_TRIAL),    SCRIPT(k_mirror, C9_TRIAL),
    SCRIPT(k_trial_won, NONE),   SCRIPT(k_need, NONE),
    SCRIPT(k_father, C9_FATHER), SCRIPT(k_battle, C9_BATTLE),
    SCRIPT(k_retry, NONE),       SCRIPT(k_reveal, ENDING),
    SCRIPT(k_rest, NONE),        SCRIPT(k_help, NONE),
};
static const h2_xianxia_rpg_chapter_info_t k_info = {
    .number = 9,
    .first_stage = H2_XIANXIA_RPG_STAGE_C9_DESCEND,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_C9_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_SHAFT_UPPER,
    .map_count = 4,
    .start_map = H2_XIANXIA_RPG_MAP_SHAFT_UPPER,
    .rest_map = H2_XIANXIA_RPG_MAP_SHAFT_UPPER,
    .start_x = 7,
    .start_y = 3,
    .rest_x = 7,
    .rest_y = 3,
    .default_level = 11,
    .default_pills = 3,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_C9_INTRO,
    .label = "第九章",
    .name = "第九章 · 地脉回声",
    .title = "地脉回声",
    .ending = {"回声有名，守灯人不再独候。", "三处共守，溪灯归灯将相逢。"},
    .next = "第九章 完 · 下一章《长明归途》",
    .recap =
        "雪岭两岸的灯队终于渡过断桥。江篱与闻舟同行，桥下旧井露出八种灯纹。井底有人唤着青崖的名字，叫他别过来。三人沿绳梯深入地下，寻找守灯人独自改阵的答案。",
};
char h2_xianxia_rpg_c9_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (map < H2_XIANXIA_RPG_MAP_SHAFT_UPPER ||
      map > H2_XIANXIA_RPG_MAP_LANTERN_CORE || x < 0 || y < 0 ||
      x >= H2_XIANXIA_RPG_MAP_COLS || y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map - H2_XIANXIA_RPG_MAP_SHAFT_UPPER][y][x];
}
const char* h2_xianxia_rpg_c9_map_name(h2_xianxia_rpg_map_t map) {
  return k_names[map - H2_XIANXIA_RPG_MAP_SHAFT_UPPER];
}
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c9_info(void) {
  return &k_info;
}
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c9_script(
    h2_xianxia_rpg_script_id_t id) {
  return id >= H2_XIANXIA_RPG_SCRIPT_C9_FIRST &&
                 id < H2_XIANXIA_RPG_SCRIPT_COUNT
             ? &k_scripts[id - H2_XIANXIA_RPG_SCRIPT_C9_FIRST]
             : NULL;
}
uint32_t h2_xianxia_rpg_c9_reference(uint32_t room, uint32_t index) {
  static const uint8_t refs[4][2] = {{0, 1}, {3, 4}, {5, 7}, {2, 6}};
  return refs[room % 4u][index % 2u];
}
uint32_t h2_xianxia_rpg_c9_glyph(uint32_t room, uint32_t choice) {
  static const uint8_t glyphs[4][3] = {
      {0, 8, 1}, {9, 4, 3}, {5, 7, 10}, {6, 11, 2}};
  return glyphs[room % 4u][choice % 3u];
}
bool h2_xianxia_rpg_c9_wrong_glyph(uint32_t room, uint32_t choice) {
  return room < 4u && choice < 3u &&
         h2_xianxia_rpg_c9_glyph(room, choice) >= 8u;
}
size_t h2_xianxia_rpg_c9_strings(const char** out, size_t capacity) {
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
  for (size_t i = 0; i < H2_XIANXIA_RPG_C9_TEXT_COUNT; ++i)
    ADD(h2_xianxia_rpg_c9_text[i]);
  for (size_t i = 0; i < 8; ++i) ADD(h2_xianxia_rpg_c9_glyph_names[i]);
  for (size_t i = 0; i < 4; ++i) {
    ADD(h2_xianxia_rpg_c9_room_names[i]);
    ADD(h2_xianxia_rpg_c9_phase_hints[i]);
  }
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
