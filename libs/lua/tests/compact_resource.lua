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
-- This error must identify the same original source line after compaction.
error("compact fixture complete")
