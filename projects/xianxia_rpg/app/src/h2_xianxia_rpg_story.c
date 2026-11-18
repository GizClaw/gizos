/* World data and every story line. The Chinese text in this file is the glyph
 * source of the App fonts, so new strings shown on screen belong here or in
 * another file listed as a font symbol source. */
#include "h2_xianxia_rpg_story.h"

#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define C1_MAPS 3

/* Tiles: '.' grass, ',' flowers, '=' path, '~' water, 'b' bridge, 'B'
 * bamboo, 'T' tree, 'r' rock, 'R' roof, 'H' wall, 'D' door, 'W' well,
 * 'f' fence, 'L' lantern, '#' stone wall, '_' stone floor, 'P' pillar,
 * 'A' altar, 'S' spirit spring, 'X' spirit barrier, 's' steps. */
static const char* const k_maps[C1_MAPS][H2_XIANXIA_RPG_MAP_ROWS] = {
    [H2_XIANXIA_RPG_MAP_VILLAGE] =
        {
            "TTTBBBTTTTBBBTT",
            "T,.RRR..L.RRR.T",
            "T..HDH....HDH.T",
            "T...=......=..T",
            "T,..====W======",
            "T...=.....,...T",
            "T.,.=..f.f..,.T",
            "~~~~b~~~~~~~~~~",
            ",.,.=.,..,.r.,T",
            "TTBBBTTTTBBBTTT",
        },
    [H2_XIANXIA_RPG_MAP_FOREST] =
        {
            "BBBBBBB=BBBBBBB",
            "BB..B..X..B..BB",
            "B..BB..=..BB..B",
            "B.B....=.....BB",
            "===....==,...,B",
            "B..BB....=.,..B",
            "B.....BB.=....B",
            "BB.,.....=..B.B",
            "B..BB.B.....BBB",
            "BBBBBBBBBBBBBBB",
        },
    [H2_XIANXIA_RPG_MAP_SHRINE] =
        {
            "###############",
            "#P____PAP____P#",
            "#_____________#",
            "#_P_~~___~~_P_#",
            "#_____________#",
            "#_P__L___L__P_#",
            "####___s___####",
            "BBB#S______#BBB",
            "BBB#_______#BBB",
            "BBB####=####BBB",
        },
};

static const char* const k_map_names[C1_MAPS] = {
    [H2_XIANXIA_RPG_MAP_VILLAGE] = "竹溪村",
    [H2_XIANXIA_RPG_MAP_FOREST] = "幽篁小径",
    [H2_XIANXIA_RPG_MAP_SHRINE] = "墨溪古祠",
};

static const char* const k_actor_names[H2_XIANXIA_RPG_ACTOR_COUNT] = {
    [H2_XIANXIA_RPG_ACTOR_NARRATOR] = "",
    [H2_XIANXIA_RPG_ACTOR_HERO] = "沈青崖",
    [H2_XIANXIA_RPG_ACTOR_ELDER] = "陶伯",
    [H2_XIANXIA_RPG_ACTOR_HERBALIST] = "柳婆婆",
    [H2_XIANXIA_RPG_ACTOR_CHILD] = "阿禾",
    [H2_XIANXIA_RPG_ACTOR_DAOIST] = "白鹤道长",
    [H2_XIANXIA_RPG_ACTOR_DRAGON] = "墨蛟",
    [H2_XIANXIA_RPG_ACTOR_JIANGLI] = "江篱",
    [H2_XIANXIA_RPG_ACTOR_FERRYMASTER] = "鲁把头",
    [H2_XIANXIA_RPG_ACTOR_TEAWIFE] = "桂嫂",
    [H2_XIANXIA_RPG_ACTOR_TRAVELER] = "渡客",
    [H2_XIANXIA_RPG_ACTOR_FISHER] = "芦叟",
    [H2_XIANXIA_RPG_ACTOR_WRECK_GUARD] = "守船雾鬼",
    [H2_XIANXIA_RPG_ACTOR_WRAITH] = "雾魇",
};

static const char* const k_objectives[H2_XIANXIA_RPG_STAGE_COUNT] = {
    [H2_XIANXIA_RPG_STAGE_MEET_ELDER] = "与村长陶伯交谈",
    [H2_XIANXIA_RPG_STAGE_SEEK_DAOIST] = "去东边竹林拜见白鹤道长",
    [H2_XIANXIA_RPG_STAGE_TO_SHRINE] = "北上古祠，平定墨蛟（建议三级）",
    [H2_XIANXIA_RPG_STAGE_RETURN] = "回竹溪村向陶伯复命",
    [H2_XIANXIA_RPG_STAGE_CLEARED] = "溪灯重燃",
    [H2_XIANXIA_RPG_STAGE_C2_ARRIVE] = "找渡口管事鲁把头问问这场雾",
    [H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI] = "去栈桥尽头找丙三号的船主江篱",
    [H2_XIANXIA_RPG_STAGE_C2_TO_REEDS] = "去西边芦苇滩，点亮灯桩寻找丙三号",
    [H2_XIANXIA_RPG_STAGE_C2_TO_WRECK] = "乘丙三号去沙洲沉船",
    [H2_XIANXIA_RPG_STAGE_C2_WRECK] = "查看沉船船舱里的渡灯",
    [H2_XIANXIA_RPG_STAGE_C2_RETURN] = "回青萍渡向鲁把头说明",
    [H2_XIANXIA_RPG_STAGE_C2_CLEARED] = "渡口复通",
};

static const char* const k_command_labels[H2_XIANXIA_RPG_COMMANDS] = {
    "攻击", "剑诀", "丹药", "防御", "逃跑",
};

static const h2_xianxia_rpg_enemy_info_t k_enemies[H2_XIANXIA_RPG_ENEMY_COUNT] =
    {
        [H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE] = {"竹叶精", "飞叶刃", 26, 9, 2, 7,
                                              H2_XIANXIA_RPG_RGB(86, 170, 82),
                                              H2_XIANXIA_RPG_RGB(214, 240,
                                                                 120)},
        [H2_XIANXIA_RPG_ENEMY_CIVET] = {"赤目狸", "裂风爪", 34, 11, 3, 10,
                                        H2_XIANXIA_RPG_RGB(196, 112, 52),
                                        H2_XIANXIA_RPG_RGB(255, 60, 48)},
        [H2_XIANXIA_RPG_ENEMY_WISP] = {"幽火", "灼魂焰", 32, 14, 4, 13,
                                       H2_XIANXIA_RPG_RGB(96, 120, 255),
                                       H2_XIANXIA_RPG_RGB(200, 236, 255)},
        [H2_XIANXIA_RPG_ENEMY_GOLEM] = {"石傀", "崩山拳", 56, 15, 9, 18,
                                        H2_XIANXIA_RPG_RGB(132, 128, 120),
                                        H2_XIANXIA_RPG_RGB(120, 230, 210)},
        [H2_XIANXIA_RPG_ENEMY_DRAGON] = {"墨蛟", "墨浪", 230, 18, 7, 60,
                                         H2_XIANXIA_RPG_RGB(40, 44, 62),
                                         H2_XIANXIA_RPG_RGB(255, 206, 64)},
        /* Chapter 2: reed-flat foes about a fifth above the shrine. */
        [H2_XIANXIA_RPG_ENEMY_FOG_GHOST] = {"雾鬼", "寒雾爪", 44, 16, 4, 16,
                                            H2_XIANXIA_RPG_RGB(200, 210, 222),
                                            H2_XIANXIA_RPG_RGB(120, 230, 255)},
        [H2_XIANXIA_RPG_ENEMY_REED_OTTER] = {"芦獭", "连爪", 40, 15, 5, 15,
                                             H2_XIANXIA_RPG_RGB(132, 110, 70),
                                             H2_XIANXIA_RPG_RGB(250, 200, 90)},
        [H2_XIANXIA_RPG_ENEMY_REED_SPRITE] = {"苇精", "缠足", 50, 14, 6, 17,
                                              H2_XIANXIA_RPG_RGB(176, 170, 96),
                                              H2_XIANXIA_RPG_RGB(236, 226,
                                                                 150)},
        [H2_XIANXIA_RPG_ENEMY_WRECK_GUARD] = {"守船雾鬼", "沉锚", 150, 20, 10,
                                              40,
                                              H2_XIANXIA_RPG_RGB(110, 124, 138),
                                              H2_XIANXIA_RPG_RGB(150, 220,
                                                                 255)},
        [H2_XIANXIA_RPG_ENEMY_FOG_WRAITH] = {"雾魇", "雾锁", 380, 23, 8, 90,
                                             H2_XIANXIA_RPG_RGB(150, 150, 176),
                                             H2_XIANXIA_RPG_RGB(190, 120, 255)},
};

#define SAY(actor, text) {H2_XIANXIA_RPG_ACTOR_##actor, text}
#define SCRIPT(lines, effect) \
  {lines, (uint8_t)ARRAY_SIZE(lines), H2_XIANXIA_RPG_EFFECT_##effect}

static const h2_xianxia_rpg_line_t k_intro[] = {
    SAY(NARRATOR,
        "竹溪村每逢月半都要放溪灯祈福。可今年溪水一夜发黑，灯一沾水便灭，夜里还能听见低沉的龙吟。"),
    SAY(HERO,
        "十年前，爹爹为了封住墨溪古祠下的东西，再也没有回来……溪水变黑，难道是封印出了事？"),
    SAY(HERO, "陶伯说今早有要事相商，先去他家门前看看。"),
    SAY(NARRATOR,
        "轻触地面即可行走，轻触人物即可交谈。头顶金色菱标的人，便是下一步要找的人。"),
};

static const h2_xianxia_rpg_line_t k_elder_brief[] = {
    SAY(ELDER,
        "青崖，你来得正好。墨溪古祠的封印松了，你爹当年镇住的墨蛟，醒了。"),
    SAY(ELDER,
        "溪水发黑，就是它吐出的浊气。再拖下去，药田要毁，今晚的溪灯也点不成了。"),
    SAY(HERO, "爹爹守过的东西，我来守。陶伯，让我去古祠！"),
    SAY(ELDER,
        "好孩子。古祠前有灵障，凡力难破。东边竹林里的白鹤道长与你爹是旧识，去求他相助。"),
    SAY(ELDER,
        "柳婆婆备了丹药，出发前记得去取。累了就到井边歇一歇，井水能养气血。"),
};
static const h2_xianxia_rpg_line_t k_elder_wait[] = {
    SAY(ELDER,
        "白鹤道长就在东边竹林深处。林中妖物被浊气惊动，常会突然扑出来，千万小心。"),
};
static const h2_xianxia_rpg_line_t k_elder_go[] = {
    SAY(ELDER,
        "道长肯传你剑诀？好！古祠就在竹林北面。墨蛟蓄力时，千万别硬拼。"),
};
static const h2_xianxia_rpg_line_t k_elder_report[] = {
    SAY(HERO, "陶伯，墨蛟已重归封印，溪水也清了。"),
    SAY(ELDER, "好，好！你爹若在，也会以你为荣。今晚的溪灯，终于能点起来了。"),
    SAY(HERO,
        "只是……祭坛边落着这枚铜牌，旧符纸的断口平整，像是被人一剑削开的。"),
    SAY(ELDER, "“青萍渡·丙三”……这是下游青萍渡的船牌。有人故意放出了墨蛟。"),
    SAY(ELDER, "此事不简单。过了溪灯节，你便顺流而下，去渡口查个明白。"),
};

static const h2_xianxia_rpg_line_t k_herbalist_early[] = {
    SAY(HERBALIST, "陶伯一早就在找你，快去他家门前看看吧。"),
};
static const h2_xianxia_rpg_line_t k_herbalist_give[] = {
    SAY(HERBALIST, "要去竹林？这三颗回春丹你收好，危急时服下，能续上一口气。"),
    SAY(NARRATOR, "得到回春丹三颗。"),
};
static const h2_xianxia_rpg_line_t k_herbalist_refill[] = {
    SAY(HERBALIST, "丹药快用完了吧？婆婆又配了几颗，拿去。"),
    SAY(NARRATOR, "回春丹补足到三颗。"),
};
static const h2_xianxia_rpg_line_t k_herbalist_enough[] = {
    SAY(HERBALIST, "丹药还够用，省着点吃。受了伤就去井边歇歇。"),
};
static const h2_xianxia_rpg_line_t k_herbalist_thanks[] = {
    SAY(HERBALIST, "溪水清了！婆婆的药田有救了。快去告诉陶伯吧。"),
};

static const h2_xianxia_rpg_line_t k_child[] = {
    SAY(CHILD, "青崖哥哥！竹林里的妖怪会突然跳出来，打不过就快跑！"),
    SAY(CHILD, "听说剑诀很耗灵力，防御的时候还能回一点呢。"),
};
static const h2_xianxia_rpg_line_t k_child_thanks[] = {
    SAY(CHILD, "溪水变清啦！今晚我要放一盏最大的溪灯！"),
};

static const h2_xianxia_rpg_line_t k_daoist_early[] = {
    SAY(DAOIST, "小友步履匆匆，可是村中有事？先回去问问陶老儿吧。"),
};
static const h2_xianxia_rpg_line_t k_daoist_teach[] = {
    SAY(DAOIST, "你是沈家的孩子吧？眉眼和你爹一模一样。墨蛟之事，贫道已知晓。"),
    SAY(DAOIST,
        "古祠前那道灵障，凡力难破。我传你一式“流云剑”，剑气可破障，亦可伤妖。"),
    SAY(HERO, "多谢道长指点！"),
    SAY(DAOIST,
        "记住：墨蛟周身墨气翻涌，便是在蓄力，下一招必是墨浪。那时凝神防御，可保无虞。"),
    SAY(DAOIST, "它若重伤发怒，蓄力会更急。祠中有灵泉，进殿前先去歇息。"),
    SAY(DAOIST,
        "墨蛟道行深厚。若觉吃力，先在林中多斗几场妖物，修为到了三级再进殿。"),
    SAY(NARRATOR,
        "习得剑诀“流云剑”！气血与灵力尽复，另得回春丹两颗。北面的灵障消散了。"),
};
static const h2_xianxia_rpg_line_t k_daoist_later[] = {
    SAY(DAOIST, "见墨蛟蓄力，切记防御。灵力不足时，凝神防御也能回复些许。"),
};
static const h2_xianxia_rpg_line_t k_daoist_return[] = {
    SAY(DAOIST, "封印已复，好。只是这浊气来得蹊跷……回村后，留心下游的消息。"),
};

static const h2_xianxia_rpg_line_t k_dragon[] = {
    SAY(DRAGON, "沈家的小娃娃？十年前那一剑，本座可还记着！"),
    SAY(HERO, "你搅浊溪水、祸害乡里，今日便送你重归封印！"),
    SAY(DRAGON, "封印？哈！解开它的人，可不是你们竹溪村的……先尝尝墨浪的滋味！"),
};

static const h2_xianxia_rpg_line_t k_well[] = {
    SAY(NARRATOR, "井水清凉，喝上一口，气血与灵力都恢复了。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_spring[] = {
    SAY(NARRATOR, "灵泉微光流转，气血与灵力尽数恢复。（已记录进度）"),
};
static const h2_xianxia_rpg_line_t k_barrier[] = {
    SAY(NARRATOR,
        "一道淡青色的灵障拦住了去路，凭蛮力无法通过。也许白鹤道长知道破法。"),
};
static const h2_xianxia_rpg_line_t k_victory[] = {
    SAY(NARRATOR, "墨蛟哀鸣一声，化作一缕黑烟没入祭坛，封印重新亮起金光。"),
    SAY(NARRATOR,
        "祭坛边落着一枚铜牌，刻着“青萍渡·丙三”。旧符纸的断口平整，像是被人一剑削开。"),
    SAY(HERO, "有人故意解开了封印？……先回村向陶伯复命。"),
};
static const h2_xianxia_rpg_line_t k_revive[] = {
    SAY(NARRATOR, "沈青崖在歇息处醒来，气血已经恢复。重整旗鼓，再去一试！"),
};
static const h2_xianxia_rpg_line_t k_revive_boss[] = {
    SAY(NARRATOR,
        "沈青崖在歇息处醒来，气血已经恢复。墨蛟道行深厚，单凭蛮力赢不了它。"),
    SAY(HERO, "先去竹林和古祠多斗几场妖物，修为到了三级再来！"),
    SAY(NARRATOR,
        "提示：墨蛟蓄力时选“防御”；气血低于一半就服丹药；防御还能回复灵力。"
        "水井和灵泉随时可以歇息。"),
};

static const h2_xianxia_rpg_script_t k_scripts[H2_XIANXIA_RPG_SCRIPT_C2_FIRST] =
    {
        [H2_XIANXIA_RPG_SCRIPT_INTRO] = SCRIPT(k_intro, NONE),
        [H2_XIANXIA_RPG_SCRIPT_ELDER_BRIEF] = SCRIPT(k_elder_brief, BRIEFED),
        [H2_XIANXIA_RPG_SCRIPT_ELDER_WAIT] = SCRIPT(k_elder_wait, NONE),
        [H2_XIANXIA_RPG_SCRIPT_ELDER_GO] = SCRIPT(k_elder_go, NONE),
        [H2_XIANXIA_RPG_SCRIPT_ELDER_REPORT] = SCRIPT(k_elder_report, ENDING),
        [H2_XIANXIA_RPG_SCRIPT_HERBALIST_EARLY] =
            SCRIPT(k_herbalist_early, NONE),
        [H2_XIANXIA_RPG_SCRIPT_HERBALIST_GIVE] =
            SCRIPT(k_herbalist_give, GIVE_PILLS),
        [H2_XIANXIA_RPG_SCRIPT_HERBALIST_REFILL] =
            SCRIPT(k_herbalist_refill, GIVE_PILLS),
        [H2_XIANXIA_RPG_SCRIPT_HERBALIST_ENOUGH] =
            SCRIPT(k_herbalist_enough, NONE),
        [H2_XIANXIA_RPG_SCRIPT_HERBALIST_THANKS] =
            SCRIPT(k_herbalist_thanks, NONE),
        [H2_XIANXIA_RPG_SCRIPT_CHILD] = SCRIPT(k_child, NONE),
        [H2_XIANXIA_RPG_SCRIPT_CHILD_THANKS] = SCRIPT(k_child_thanks, NONE),
        [H2_XIANXIA_RPG_SCRIPT_DAOIST_EARLY] = SCRIPT(k_daoist_early, NONE),
        [H2_XIANXIA_RPG_SCRIPT_DAOIST_TEACH] = SCRIPT(k_daoist_teach, TAUGHT),
        [H2_XIANXIA_RPG_SCRIPT_DAOIST_LATER] = SCRIPT(k_daoist_later, NONE),
        [H2_XIANXIA_RPG_SCRIPT_DAOIST_RETURN] = SCRIPT(k_daoist_return, NONE),
        [H2_XIANXIA_RPG_SCRIPT_DRAGON] = SCRIPT(k_dragon, BOSS_BATTLE),
        [H2_XIANXIA_RPG_SCRIPT_WELL] = SCRIPT(k_well, REST),
        [H2_XIANXIA_RPG_SCRIPT_SPRING] = SCRIPT(k_spring, REST),
        [H2_XIANXIA_RPG_SCRIPT_BARRIER] = SCRIPT(k_barrier, NONE),
        [H2_XIANXIA_RPG_SCRIPT_VICTORY] = SCRIPT(k_victory, NONE),
        [H2_XIANXIA_RPG_SCRIPT_REVIVE] = SCRIPT(k_revive, NONE),
        [H2_XIANXIA_RPG_SCRIPT_REVIVE_BOSS] = SCRIPT(k_revive_boss, NONE),
};

/* UI and battle strings drawn by the renderer and battle model. Formats
 * keep their placeholders ASCII so the glyph check covers every literal. */
const char* const h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_COUNT] = {
    [H2_XIANXIA_RPG_TEXT_TITLE] = "竹溪仙缘",
    [H2_XIANXIA_RPG_TEXT_SUBTITLE] = "原创国风仙侠 · 回合制角色扮演",
    [H2_XIANXIA_RPG_TEXT_CHAPTER] = "竹溪灯影 · 渡口迷雾",
    [H2_XIANXIA_RPG_TEXT_NEW_GAME] = "新的旅程",
    [H2_XIANXIA_RPG_TEXT_CONTINUE] = "继续旅程",
    [H2_XIANXIA_RPG_TEXT_TITLE_HINT] = "轻触选择",
    [H2_XIANXIA_RPG_TEXT_HP] = "气血",
    [H2_XIANXIA_RPG_TEXT_MP] = "灵力",
    [H2_XIANXIA_RPG_TEXT_PILLS] = "丹药 %u",
    [H2_XIANXIA_RPG_TEXT_LEVEL] = "Lv%u",
    [H2_XIANXIA_RPG_TEXT_OBJECTIVE] = "任务：%s",
    [H2_XIANXIA_RPG_TEXT_TAP_NEXT] = "轻触继续",
    [H2_XIANXIA_RPG_TEXT_OBJECTIVE_UPDATE] = "任务更新",
    [H2_XIANXIA_RPG_TEXT_TIPS] =
        "轻触地面即可行走，轻触人物或物件即可交谈、歇息。金色菱标指明下一步该去哪里。",
    [H2_XIANXIA_RPG_TEXT_APPEARS] = "%s出现了！",
    [H2_XIANXIA_RPG_TEXT_BOSS_APPEARS] = "%s挡住了去路！此战无路可退。",
    [H2_XIANXIA_RPG_TEXT_CHOOSE] = "沈青崖要如何应对？",
    [H2_XIANXIA_RPG_TEXT_ATTACK] = "沈青崖挥剑斩向%s，造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_CRITICAL] = "会心一击！斩向%s，造成 %u 点伤害！",
    [H2_XIANXIA_RPG_TEXT_SKILL] = "流云剑！三道剑光连斩，共造成 %u 点伤害！",
    [H2_XIANXIA_RPG_TEXT_NO_SKILL] = "尚未习得剑诀，去竹林拜见白鹤道长吧。",
    [H2_XIANXIA_RPG_TEXT_NO_MP] = "灵力不足，无法施展流云剑。",
    [H2_XIANXIA_RPG_TEXT_PILL] = "服下回春丹，气血恢复 %u 点。",
    [H2_XIANXIA_RPG_TEXT_NO_PILLS] = "没有回春丹了！",
    [H2_XIANXIA_RPG_TEXT_GUARD] = "沈青崖凝神防御，灵力回复 %u 点。",
    [H2_XIANXIA_RPG_TEXT_FLED] = "沈青崖抽身而退，脱离了战斗。",
    [H2_XIANXIA_RPG_TEXT_FLEE_FAILED] = "没能逃脱！",
    [H2_XIANXIA_RPG_TEXT_NO_FLEE] = "%s的气势笼罩四周，无路可逃！",
    [H2_XIANXIA_RPG_TEXT_ENEMY_ATTACK] = "%s扑了过来，造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_ENEMY_SPECIAL] = "%s施展“%s”，造成 %u 点伤害！",
    [H2_XIANXIA_RPG_TEXT_BOSS_TAIL] = "墨蛟甩尾横扫，造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_GUARDED] = "（防御中）",
    [H2_XIANXIA_RPG_TEXT_CHARGE] =
        "墨蛟周身墨气翻涌，正在蓄力！（下回合宜防御）",
    [H2_XIANXIA_RPG_TEXT_ENRAGE] = "墨蛟怒吼一声，攻势更猛，蓄力更急了！",
    [H2_XIANXIA_RPG_TEXT_VICTORY] = "击败了%s！获得修为 %u 点。",
    [H2_XIANXIA_RPG_TEXT_DROP] = "得到回春丹一颗。",
    [H2_XIANXIA_RPG_TEXT_LEVEL_UP] = "修为精进，升至 %u 级！",
    [H2_XIANXIA_RPG_TEXT_DEFEAT] =
        "沈青崖力竭倒下……轻触屏幕，于歇息处重整旗鼓。",
    [H2_XIANXIA_RPG_TEXT_ENDING_TITLE] = "竹溪灯影",
    [H2_XIANXIA_RPG_TEXT_ENDING_1] = "溪水复清，家家户户重新放起了溪灯。",
    [H2_XIANXIA_RPG_TEXT_ENDING_2] = "铜牌指向青萍渡，有人蓄意破了封印。",
    [H2_XIANXIA_RPG_TEXT_ENDING_NEXT] = "第一章 完 · 下一章《渡口迷雾》",
    [H2_XIANXIA_RPG_TEXT_ENDING_STATS] = "等级 %u · 胜战 %u 场 · 回合 %u",
    [H2_XIANXIA_RPG_TEXT_ENDING_HINT] = "轻触屏幕，回到标题",
    [H2_XIANXIA_RPG_TEXT_ENDING_ONWARD] = "轻触屏幕，继续第二章《渡口迷雾》",
    [H2_XIANXIA_RPG_TEXT_CHAPTERS] = "章节",
    [H2_XIANXIA_RPG_TEXT_CHAPTERS_HEAD] = "选择章节",
    [H2_XIANXIA_RPG_TEXT_CONTINUE_AT] = "继续旅程 · %s",
    [H2_XIANXIA_RPG_TEXT_BACK] = "返回",
    [H2_XIANXIA_RPG_TEXT_RESTART] = "从头",
    [H2_XIANXIA_RPG_TEXT_STATUS_CLEARED] = "已完成",
    [H2_XIANXIA_RPG_TEXT_STATUS_PLAYING] = "进行中",
    [H2_XIANXIA_RPG_TEXT_STATUS_OPEN] = "可开始",
    [H2_XIANXIA_RPG_TEXT_STATUS_SOON] = "敬请期待",
    [H2_XIANXIA_RPG_TEXT_UPCOMING] = "第三章 · 雨夜剑阁",
    [H2_XIANXIA_RPG_TEXT_UNREADABLE] =
        "有一份存档无法读取，已跳过；其他记录不受影响。",
    [H2_XIANXIA_RPG_TEXT_RECAP_HEAD] = "前情提要",
    [H2_XIANXIA_RPG_TEXT_CARRY] =
        "承接第一章：等级 %u · 丹药 %u · 已习得流云剑",
    [H2_XIANXIA_RPG_TEXT_DEFAULTS] =
        "没有第一章通关记录，以默认状态开始：等级 %u · 丹药 %u · 已习得流云剑",
    [H2_XIANXIA_RPG_TEXT_PROLOGUE_HINT] = "轻触屏幕，开始本章",
    [H2_XIANXIA_RPG_TEXT_JOINT] = "合击",
    [H2_XIANXIA_RPG_TEXT_RAPPORT] = "默契",
    [H2_XIANXIA_RPG_TEXT_INTENT] = "江篱：%s",
    [H2_XIANXIA_RPG_TEXT_INTENT_LANTERN] = "提灯",
    [H2_XIANXIA_RPG_TEXT_INTENT_POLE] = "撑篙",
    [H2_XIANXIA_RPG_TEXT_INTENT_STEADY] = "稳船",
    [H2_XIANXIA_RPG_TEXT_APPEARS_VEILED] = "%s出现了！它藏在雾里，看不真切。",
    [H2_XIANXIA_RPG_TEXT_VEILED_HIT] = "%s藏在雾里，这一击只造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_VEILED_SKILL] = "流云剑没入雾中，只造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_HINDERED] = "（脚下被缠，力道减弱）",
    [H2_XIANXIA_RPG_TEXT_JOINT_STRIKE] =
        "流云·渡灯！剑光随灯火齐落，造成 %u 点伤害！",
    [H2_XIANXIA_RPG_TEXT_JOINT_BREAK] = "蓄力被打断了！",
    [H2_XIANXIA_RPG_TEXT_ALLY_LANTERN] = "江篱高举提灯，照破了%s的雾隐！",
    [H2_XIANXIA_RPG_TEXT_ALLY_POLE] = "江篱撑篙一点，打中%s %u 点。",
    [H2_XIANXIA_RPG_TEXT_ALLY_STEADY] =
        "江篱稳住身形护在你身侧，下一击会更轻。",
    [H2_XIANXIA_RPG_TEXT_RAPPORT_UP] = "默契 +1",
    [H2_XIANXIA_RPG_TEXT_ENEMY_VEIL] = "%s又隐入了雾中。",
    [H2_XIANXIA_RPG_TEXT_ENEMY_DOUBLE] = "%s施展“%s”，连抓两下，共 %u 点伤害！",
    [H2_XIANXIA_RPG_TEXT_ENEMY_ENTANGLE] =
        "%s施展“%s”，造成 %u 点伤害，缠住了你的脚！",
    [H2_XIANXIA_RPG_TEXT_WRAITH_VEIL] = "雾魇隐入雾中，身形模糊了。",
    [H2_XIANXIA_RPG_TEXT_WRAITH_ATTACK] = "雾魇卷起寒雾扑来，造成 %u 点伤害。",
    [H2_XIANXIA_RPG_TEXT_WRAITH_CHARGE] = "雾魇将雾气收拢成锁……正在蓄力！",
    [H2_XIANXIA_RPG_TEXT_WRAITH_LOCK] =
        "雾魇施展“雾锁”，造成 %u 点伤害！灵力被封住了。",
    [H2_XIANXIA_RPG_TEXT_WRAITH_SPLIT] = "雾魇一分为二！江篱的灯光照着真身。",
    [H2_XIANXIA_RPG_TEXT_WRAITH_SHIFT] = "两道影子交错游走……",
    [H2_XIANXIA_RPG_TEXT_WRONG_SHADOW] =
        "打中的是假身！影子溃散，雾魇借雾回复了 %u 点。",
    [H2_XIANXIA_RPG_TEXT_SEALED] = "灵力被雾锁封住，这回合不能施展剑诀。",
    [H2_XIANXIA_RPG_TEXT_TUTORIAL_HOLD] =
        "雾鬼只剩一缕残雾，用合击彻底驱散它！",
    [H2_XIANXIA_RPG_TEXT_HINT_VEILED] = "雾隐：攻击只剩三成，江篱会先提灯照破",
    [H2_XIANXIA_RPG_TEXT_HINT_JOINT] = "默契已满：合击威力大，还能打断蓄力",
    [H2_XIANXIA_RPG_TEXT_HINT_CHARGE] = "它在蓄力：用合击打断，或选择防御",
    [H2_XIANXIA_RPG_TEXT_HINT_SEALED] = "灵力被封：本回合不能用剑诀",
    [H2_XIANXIA_RPG_TEXT_HINT_TARGET] = "点选江篱灯光照着的影子",
    [H2_XIANXIA_RPG_TEXT_HINT_SPLIT] = "出剑后，再点灯光下的那道影子",
    [H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL1] =
        "雾鬼藏在雾里，难以命中。看江篱的提灯",
    [H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL2] = "雾隐已破！出剑攒满三盏默契灯",
    [H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL3] = "合击已就绪！点“合击”与江篱联手",
};

static const h2_xianxia_rpg_chapter_info_t k_chapter1 = {
    .number = 1u,
    .first_stage = H2_XIANXIA_RPG_STAGE_MEET_ELDER,
    .cleared_stage = H2_XIANXIA_RPG_STAGE_CLEARED,
    .first_map = H2_XIANXIA_RPG_MAP_VILLAGE,
    .map_count = C1_MAPS,
    .start_map = H2_XIANXIA_RPG_MAP_VILLAGE,
    .rest_map = H2_XIANXIA_RPG_MAP_VILLAGE,
    .start_x = 7,
    .start_y = 5,
    .rest_x = 8,
    .rest_y = 5,
    .default_level = 1u,
    .default_pills = 0u,
    .intro_script = H2_XIANXIA_RPG_SCRIPT_INTRO,
    .label = "第一章",
    .name = "第一章 · 竹溪灯影",
    .title = "竹溪灯影",
    .ending = {"溪水复清，家家户户重新放起了溪灯。",
               "铜牌指向青萍渡，有人蓄意破了封印。"},
    .next = "第一章 完 · 下一章《渡口迷雾》",
    .recap = NULL,
};

static bool c1_map(h2_xianxia_rpg_map_t map) { return (unsigned)map < C1_MAPS; }

char h2_xianxia_rpg_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  if (!c1_map(map)) return h2_xianxia_rpg_c2_tile(map, x, y);
  if (x < 0 || y < 0 || x >= H2_XIANXIA_RPG_MAP_COLS ||
      y >= H2_XIANXIA_RPG_MAP_ROWS)
    return '#';
  return k_maps[map][y][x];
}

uint32_t h2_xianxia_rpg_map_chapter(h2_xianxia_rpg_map_t map) {
  if (c1_map(map)) return 1u;
  return (unsigned)map < H2_XIANXIA_RPG_MAP_COUNT ? 2u : 0u;
}

uint32_t h2_xianxia_rpg_stage_chapter(h2_xianxia_rpg_stage_t stage) {
  if (stage <= H2_XIANXIA_RPG_STAGE_CLEARED) return 1u;
  return stage < H2_XIANXIA_RPG_STAGE_COUNT ? 2u : 0u;
}

const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_chapter_info(
    uint32_t chapter) {
  switch (chapter) {
    case 1u:
      return &k_chapter1;
    case 2u:
      return h2_xianxia_rpg_c2_info();
    default:
      return NULL;
  }
}

const char* h2_xianxia_rpg_map_name(h2_xianxia_rpg_map_t map) {
  return c1_map(map) ? k_map_names[map] : h2_xianxia_rpg_c2_map_name(map);
}

const char* h2_xianxia_rpg_actor_name(h2_xianxia_rpg_actor_t actor) {
  return (unsigned)actor < H2_XIANXIA_RPG_ACTOR_COUNT ? k_actor_names[actor]
                                                      : "";
}

const char* h2_xianxia_rpg_objective(h2_xianxia_rpg_stage_t stage) {
  return (unsigned)stage < H2_XIANXIA_RPG_STAGE_COUNT ? k_objectives[stage]
                                                      : "";
}

const h2_xianxia_rpg_enemy_info_t* h2_xianxia_rpg_enemy_info(
    h2_xianxia_rpg_enemy_t enemy) {
  return (unsigned)enemy < H2_XIANXIA_RPG_ENEMY_COUNT ? &k_enemies[enemy]
                                                      : &k_enemies[0];
}

const char* h2_xianxia_rpg_command_label(h2_xianxia_rpg_command_t command) {
  return (unsigned)command < H2_XIANXIA_RPG_COMMANDS ? k_command_labels[command]
                                                     : "";
}

const h2_xianxia_rpg_script_t* h2_xianxia_rpg_script(
    h2_xianxia_rpg_script_id_t id) {
  if (id >= H2_XIANXIA_RPG_SCRIPT_C2_FIRST) return h2_xianxia_rpg_c2_script(id);
  return (unsigned)id < H2_XIANXIA_RPG_SCRIPT_C2_FIRST ? &k_scripts[id] : NULL;
}

size_t h2_xianxia_rpg_npcs(const h2_xianxia_rpg_game_t* game,
                           h2_xianxia_rpg_map_t map,
                           h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]) {
  size_t count = 0;
  switch (map) {
    case H2_XIANXIA_RPG_MAP_VILLAGE:
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_ELDER, 4, 3,
                                            H2_XIANXIA_RPG_FACING_DOWN};
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_HERBALIST, 11,
                                            3, H2_XIANXIA_RPG_FACING_DOWN};
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_CHILD, 6, 8,
                                            H2_XIANXIA_RPG_FACING_DOWN};
      break;
    case H2_XIANXIA_RPG_MAP_FOREST:
      out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_DAOIST, 11, 6,
                                            H2_XIANXIA_RPG_FACING_LEFT};
      break;
    case H2_XIANXIA_RPG_MAP_SHRINE:
      if (game->stage < H2_XIANXIA_RPG_STAGE_RETURN)
        out[count++] = (h2_xianxia_rpg_npc_t){H2_XIANXIA_RPG_ACTOR_DRAGON, 7, 2,
                                              H2_XIANXIA_RPG_FACING_DOWN};
      break;
    default:
      return h2_xianxia_rpg_c2_npcs(game, map, out);
  }
  return count;
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor) {
  const h2_xianxia_rpg_stage_t stage = game->stage;
  switch (actor) {
    case H2_XIANXIA_RPG_ACTOR_ELDER:
      if (stage == H2_XIANXIA_RPG_STAGE_MEET_ELDER)
        return H2_XIANXIA_RPG_SCRIPT_ELDER_BRIEF;
      if (stage >= H2_XIANXIA_RPG_STAGE_RETURN)
        return H2_XIANXIA_RPG_SCRIPT_ELDER_REPORT;
      return stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST
                 ? H2_XIANXIA_RPG_SCRIPT_ELDER_WAIT
                 : H2_XIANXIA_RPG_SCRIPT_ELDER_GO;
    case H2_XIANXIA_RPG_ACTOR_HERBALIST:
      if (stage == H2_XIANXIA_RPG_STAGE_MEET_ELDER)
        return H2_XIANXIA_RPG_SCRIPT_HERBALIST_EARLY;
      if (stage >= H2_XIANXIA_RPG_STAGE_RETURN)
        return H2_XIANXIA_RPG_SCRIPT_HERBALIST_THANKS;
      if (!(game->flags & H2_XIANXIA_RPG_FLAG_PILLS_GIVEN))
        return H2_XIANXIA_RPG_SCRIPT_HERBALIST_GIVE;
      return game->hero.pills < 3u ? H2_XIANXIA_RPG_SCRIPT_HERBALIST_REFILL
                                   : H2_XIANXIA_RPG_SCRIPT_HERBALIST_ENOUGH;
    case H2_XIANXIA_RPG_ACTOR_CHILD:
      return stage >= H2_XIANXIA_RPG_STAGE_RETURN
                 ? H2_XIANXIA_RPG_SCRIPT_CHILD_THANKS
                 : H2_XIANXIA_RPG_SCRIPT_CHILD;
    case H2_XIANXIA_RPG_ACTOR_DAOIST:
      if (stage == H2_XIANXIA_RPG_STAGE_MEET_ELDER)
        return H2_XIANXIA_RPG_SCRIPT_DAOIST_EARLY;
      if (stage >= H2_XIANXIA_RPG_STAGE_RETURN)
        return H2_XIANXIA_RPG_SCRIPT_DAOIST_RETURN;
      return stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST
                 ? H2_XIANXIA_RPG_SCRIPT_DAOIST_TEACH
                 : H2_XIANXIA_RPG_SCRIPT_DAOIST_LATER;
    case H2_XIANXIA_RPG_ACTOR_DRAGON:
      return H2_XIANXIA_RPG_SCRIPT_DRAGON;
    default:
      return h2_xianxia_rpg_c2_talk_script(game, actor);
  }
}

bool h2_xianxia_rpg_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                     int32_t x, int32_t y) {
  const char tile = h2_xianxia_rpg_tile(game->map, x, y);
  if (!c1_map(game->map)) return h2_xianxia_rpg_c2_tile_interactive(game, tile);
  if (tile == 'W' || tile == 'S') return true;
  return tile == 'X' && game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE;
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_tile_script(
    const h2_xianxia_rpg_game_t* game, int32_t x, int32_t y) {
  const char tile = h2_xianxia_rpg_tile(game->map, x, y);
  if (!c1_map(game->map)) return h2_xianxia_rpg_c2_tile_script(game, tile);
  switch (tile) {
    case 'W':
      return H2_XIANXIA_RPG_SCRIPT_WELL;
    case 'S':
      return H2_XIANXIA_RPG_SCRIPT_SPRING;
    case 'X':
      return game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE
                 ? H2_XIANXIA_RPG_SCRIPT_BARRIER
                 : H2_XIANXIA_RPG_SCRIPT_COUNT;
    default:
      return H2_XIANXIA_RPG_SCRIPT_COUNT;
  }
}

bool h2_xianxia_rpg_exit_at(h2_xianxia_rpg_map_t map, int32_t x, int32_t y,
                            h2_xianxia_rpg_map_t* to_map, int32_t* to_x,
                            int32_t* to_y) {
  static const struct {
    uint8_t map, x, y, to_map, to_x, to_y;
  } exits[] = {
      {H2_XIANXIA_RPG_MAP_VILLAGE, 14, 4, H2_XIANXIA_RPG_MAP_FOREST, 1, 4},
      {H2_XIANXIA_RPG_MAP_FOREST, 0, 4, H2_XIANXIA_RPG_MAP_VILLAGE, 13, 4},
      {H2_XIANXIA_RPG_MAP_FOREST, 7, 0, H2_XIANXIA_RPG_MAP_SHRINE, 7, 8},
      {H2_XIANXIA_RPG_MAP_SHRINE, 7, 9, H2_XIANXIA_RPG_MAP_FOREST, 7, 1},
      {H2_XIANXIA_RPG_MAP_DOCK, 0, 3, H2_XIANXIA_RPG_MAP_REEDS, 13, 4},
      {H2_XIANXIA_RPG_MAP_REEDS, 14, 4, H2_XIANXIA_RPG_MAP_DOCK, 1, 3},
  };
  for (size_t i = 0; i < ARRAY_SIZE(exits); ++i) {
    if (exits[i].map != (uint8_t)map || exits[i].x != x || exits[i].y != y)
      continue;
    *to_map = (h2_xianxia_rpg_map_t)exits[i].to_map;
    *to_x = exits[i].to_x;
    *to_y = exits[i].to_y;
    return true;
  }
  return false;
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_exit_closed(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map) {
  /* The reed flats wait until the companion who can light the posts. */
  if (map == H2_XIANXIA_RPG_MAP_DOCK &&
      game->stage < H2_XIANXIA_RPG_STAGE_C2_TO_REEDS)
    return H2_XIANXIA_RPG_SCRIPT_C2_DOCK_CLOSED;
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}

h2_xianxia_rpg_script_id_t h2_xianxia_rpg_enter_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map) {
  if (map == H2_XIANXIA_RPG_MAP_REEDS &&
      !(game->flags & H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP))
    return H2_XIANXIA_RPG_SCRIPT_C2_REEDS_TIP;
  return H2_XIANXIA_RPG_SCRIPT_COUNT;
}

size_t h2_xianxia_rpg_encounters(
    h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_enemy_t out[H2_XIANXIA_RPG_ENCOUNTER_MAX]) {
  switch (map) {
    case H2_XIANXIA_RPG_MAP_FOREST:
      out[0] = H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE;
      out[1] = H2_XIANXIA_RPG_ENEMY_CIVET;
      return 2;
    case H2_XIANXIA_RPG_MAP_SHRINE:
      out[0] = H2_XIANXIA_RPG_ENEMY_WISP;
      out[1] = H2_XIANXIA_RPG_ENEMY_GOLEM;
      return 2;
    case H2_XIANXIA_RPG_MAP_REEDS:
      out[0] = H2_XIANXIA_RPG_ENEMY_FOG_GHOST;
      out[1] = H2_XIANXIA_RPG_ENEMY_REED_OTTER;
      out[2] = H2_XIANXIA_RPG_ENEMY_REED_SPRITE;
      return 3;
    default:
      return 0;
  }
}

bool h2_xianxia_rpg_ambush_cell(const h2_xianxia_rpg_game_t* game,
                                h2_xianxia_rpg_map_t map, int32_t x,
                                int32_t y) {
  switch (map) {
    case H2_XIANXIA_RPG_MAP_FOREST:
    case H2_XIANXIA_RPG_MAP_SHRINE:
      return game->stage < H2_XIANXIA_RPG_STAGE_RETURN;
    case H2_XIANXIA_RPG_MAP_REEDS:
      /* Only the tall reeds hide ambushes, so open ground is a safe way. */
      return game->stage < H2_XIANXIA_RPG_STAGE_C2_RETURN &&
             h2_xianxia_rpg_tile(map, x, y) == 'v';
    default:
      return false;
  }
}

bool h2_xianxia_rpg_costly_cell(h2_xianxia_rpg_map_t map, int32_t x,
                                int32_t y) {
  return h2_xianxia_rpg_tile(map, x, y) == 'v';
}

size_t h2_xianxia_rpg_story_strings(const char** out, size_t capacity) {
  size_t count = 0;
#define ADD(value)                            \
  do {                                        \
    if (count < capacity) out[count] = value; \
    ++count;                                  \
  } while (0)
  for (size_t i = 0; i < C1_MAPS; ++i) ADD(k_map_names[i]);
  for (size_t i = 0; i < H2_XIANXIA_RPG_ACTOR_COUNT; ++i) ADD(k_actor_names[i]);
  for (size_t i = 0; i < H2_XIANXIA_RPG_STAGE_COUNT; ++i) ADD(k_objectives[i]);
  for (size_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i) ADD(k_command_labels[i]);
  for (size_t i = 0; i < H2_XIANXIA_RPG_ENEMY_COUNT; ++i) {
    ADD(k_enemies[i].name);
    ADD(k_enemies[i].special);
  }
  for (size_t i = 0; i < H2_XIANXIA_RPG_SCRIPT_C2_FIRST; ++i)
    for (size_t j = 0; j < k_scripts[i].count; ++j)
      ADD(k_scripts[i].lines[j].text);
  for (size_t i = 0; i < H2_XIANXIA_RPG_TEXT_COUNT; ++i)
    ADD(h2_xianxia_rpg_text[i]);
  ADD(k_chapter1.label);
  ADD(k_chapter1.name);
  ADD(k_chapter1.title);
  ADD(k_chapter1.ending[0]);
  ADD(k_chapter1.ending[1]);
  ADD(k_chapter1.next);
#undef ADD
  const size_t room = count < capacity ? capacity - count : 0u;
  return count + h2_xianxia_rpg_c2_strings(
                     out + (count < capacity ? count : capacity), room);
}
