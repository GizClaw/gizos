-- Text inside literals must be copied, including escaped whitespace.
local short = "quote: \" -- not a comment"
local escaped = 'backslash: \\ and quote: \' -- literal'
local skipped = "left\z
      right"
local long = [==[
  -- literal ]=] bracket
]==]
assert(short == 'quote: " -- not a comment')
assert(escaped == "backslash: \\ and quote: ' -- literal")
assert(skipped == "leftright")
assert(long == "  -- literal ]=] bracket\n")
local difference = 7 - --[[ retain separated minus tokens ]] - 2
assert(difference == 9)
local function value() return--[=[a long comment
with a second line]=]42 end
assert(value() == 42)
assert((1 .. .5) == "10.5")
local a, b = "left", "right"
assert(a..b == "leftright")
assert(0x1p4 == 16 and 1e-3 == 0.001 and 0xe+1 == 15)
assert("\x22\u{22}\034\092" == '\"\"\"\\')
assert("\u{1F642}" == "🙂")
assert("\1x\12x\1234" == "\001x\012x\1234")
assert(#"\000\255" == 2)
local folded = "first\
second"
assert(folded == "first\nsecond")
--[ This is a short comment, despite the bracket and unmatched quote: '
--[==[ An arbitrary-level long comment with a wrong closing delimiter: ]=]
]==]
-- This error must identify the same original source line after compaction.
error("compact fixture complete")
