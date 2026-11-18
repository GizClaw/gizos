#include "h2_fruit_slice_text.h"

#include <string.h>

const h2_fruit_slice_translation_t h2_fruit_slice_translations[] = {
    {"FRUIT SLICE", "切水果"},
    {"中文", "中文"},
    {"SOUND OFF", "声关"},
    {"SOUND ON", "声开"},
    {"MUSIC OFF", "乐关"},
    {"MUSIC ON", "乐开"},
    {"R+%u %u COINS", "范围+%u %u币"},
    {"10 FRUIT REPEAT", "切10果可重领"},
    {"3 COMBO REPEAT", "连击3次重领"},
    {"20 SCORE ONCE", "20分仅一次"},
    {"SWIPE THE MELON TO START", "划开西瓜，开始游戏"},
    {"BEST %u", "最高 %u"},
    {"GAME OVER", "本局结束"},
    {"SCORE %u", "得分 %u"},
    {"NEW BEST!", "新纪录！"},
    {"SLICE TO PLAY AGAIN", "划开水果，再来一局"},
    {"SFX OFF", "音效关"},
    {"SFX ON", "音效开"},
    {"BGM OFF", "音乐关"},
    {"BGM ON", "音乐开"},
    {"COINS %u", "金币 %u"},
    {"COIN TASKS", "金币任务"},
    {"WEAPON SHOP", "武器商店"},
    {"CLASSIC KNIFE", "经典小刀"},
    {"STANDARD CUT", "标准切割范围"},
    {"JADE KATANA", "翡翠太刀"},
    {"+10 CUT REACH", "切割范围 +10"},
    {"FLAME AXE", "烈焰战斧"},
    {"+20 CUT REACH", "切割范围 +20"},
    {"SLICE 10 FRUIT", "切开10个水果"},
    {"MAKE 3 COMBOS", "完成3次连击"},
    {"SCORE 20 IN RUN", "单局获得20分"},
    {"DONE", "已领"},
    {"CLAIM", "领取"},
    {"FULL", "已满"},
    {"PLAY", "未完"},
    {"ACTIVE", "在用"},
    {"EQUIP", "装备"},
    {"BUY", "购买"},
    {"LOCKED", "未购"},
    {"%u/%u +%u COINS", "%u/%u 奖励%u币"},
    {"REPEATABLE", "可重复领取"},
    {"ONE-TIME", "仅可领取一次"},
    {"%u COINS", "%u金币"},
    {"SESSION ONLY - NO STORAGE", "无存储，仅本次有效"},
    {"SAVE FAILED - RETRYING", "保存失败，正在重试"},
    {"SAVE UNREADABLE - REOPEN", "存档异常，请重新打开"},
    {"COMBO = 3+ FRUIT/SWIPE", "一划切开3个水果可连击"},
    {"WIDE CUTS HIT BOMBS", "范围越大，越要小心炸弹"},
    {"BACK", "返回"},
    {"CLAIM!", "领奖！"},
    {"TASKS", "任务"},
    {"NEW CARD", "新卡片"},
    {"CARDS", "图鉴"},
    {"CARDS %u/%u", "图鉴 %u/%u"},
    {"SHOP", "商店"},
    {"COMBO +%u", "连击 +%u"},
    {"WATERMELON", "西瓜"},
    {"SLICE WATERMELONS", "切开西瓜"},
    {"ORANGE", "橙子"},
    {"SLICE ORANGES", "切开橙子"},
    {"APPLE", "苹果"},
    {"SLICE APPLES", "切开苹果"},
    {"KIWI", "猕猴桃"},
    {"SLICE KIWIS", "切开猕猴桃"},
    {"LEMON", "柠檬"},
    {"SLICE LEMONS", "切开柠檬"},
    {"FRUIT SALAD", "水果沙拉"},
    {"SLICE 30 IN ONE RUN", "单局切开30个水果"},
    {"SCORE 10 WITH KNIFE", "用小刀单局获得10分"},
    {"SCORE 10 WITH KATANA", "用太刀单局获得10分"},
    {"SCORE 10 WITH AXE", "用战斧单局获得10分"},
    {"BOMB", "炸弹"},
    {"HIT A BOMB", "切中一次炸弹"},
    {"CLOSE CALL", "惊险闪避"},
    {"DODGE 3 BOMBS IN A RUN", "单局躲过3个炸弹"},
    {"BOMB SQUAD", "拆弹专家"},
    {"DODGE 8 BOMBS IN A RUN", "单局躲过8个炸弹"},
    {"COMBO", "连击"},
    {"SLICE 3 IN ONE SWIPE", "一划切开3个水果"},
    {"MEGA COMBO", "超级连击"},
    {"SLICE 5 IN ONE SWIPE", "一划切开5个水果"},
    {"HIGH SCORE", "高分达人"},
    {"SCORE 30 IN ONE RUN", "单局获得30分"},
    {"FRUIT LEGEND", "水果传奇"},
    {"SCORE 60 IN ONE RUN", "单局获得60分"},
    {"FRUIT", "水果"},
    {"WEAPON", "武器"},
    {"SKILL", "技巧"},
    {"COMMON", "普通"},
    {"RARE", "稀有"},
    {"EPIC", "史诗"},
    {"LEGENDARY", "传说"},
    {"MELON", "西瓜"},
    {"ORNG", "橙子"},
    {"SALAD", "沙拉"},
    {"KNIFE", "小刀"},
    {"JADE", "太刀"},
    {"AXE", "战斧"},
    {"DODGE", "闪避"},
    {"SQUAD", "拆弹"},
    {"MEGA", "超连"},
    {"SCORE", "高分"},
    {"CROWN", "传奇"},
    {"NEW", "新"},
    {"NEW!", "新卡！"},
    {"NEW CARD!", "获得新卡！"},
    {"CARDS +%u  NEW %u", "卡片 +%u 新卡 %u"},
    {"CARDS +%u", "卡片 +%u"},
    {"TAP TO CONTINUE %u/%u", "轻点继续 %u/%u"},
    {"TAP TO CONTINUE", "轻点继续"},
    {"CARD ALBUM", "卡片图鉴"},
    {"%s - LOCKED", "%s 未解锁"},
    {"SESSION ONLY", "仅本次有效"},
    {"UNSAVED - RETRYING", "未保存，正在重试"},
    {"UNSAVED - PROTECTED", "存档异常，已保护"},
};

const size_t h2_fruit_slice_translation_count =
    sizeof(h2_fruit_slice_translations) /
    sizeof(h2_fruit_slice_translations[0]);

const char* h2_fruit_slice_translate(h2_fruit_slice_language_t language,
                                     const char* english) {
  if (language == H2_FRUIT_SLICE_LANGUAGE_EN || english == NULL) return english;
  for (size_t i = 0; i < h2_fruit_slice_translation_count; ++i)
    if (strcmp(english, h2_fruit_slice_translations[i].en) == 0)
      return h2_fruit_slice_translations[i].zh;
  return english;
}

uint32_t h2_fruit_slice_utf8_next(const char** text) {
  if (text == NULL || *text == NULL || **text == '\0') return 0u;
  const unsigned char* s = (const unsigned char*)*text;
  const uint32_t first = *s++;
  *text = (const char*)s;
  if (first < 0x80u) return first;
  unsigned extra;
  uint32_t cp, minimum;
  if (first >= 0xC2u && first <= 0xDFu) {
    extra = 1u;
    cp = first & 0x1Fu;
    minimum = 0x80u;
  } else if (first >= 0xE0u && first <= 0xEFu) {
    extra = 2u;
    cp = first & 0x0Fu;
    minimum = 0x800u;
  } else if (first >= 0xF0u && first <= 0xF4u) {
    extra = 3u;
    cp = first & 7u;
    minimum = 0x10000u;
  } else {
    return 0xFFFDu;
  }
  for (unsigned i = 0; i < extra; ++i) {
    if ((*s & 0xC0u) != 0x80u) return 0xFFFDu;
    cp = (cp << 6) | (*s++ & 0x3Fu);
  }
  *text = (const char*)s;
  return cp < minimum || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)
             ? 0xFFFDu
             : cp;
}
