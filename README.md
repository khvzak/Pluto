# Pluto
Pluto is a fork of the Lua 5.4.4 programming language. Internally, it's mostly PUC-Rio Lua 5.4, but as I continue to make small modifications, over time it'll be its own little thing. This is why I ascribed the name 'Pluto', because it'll always be something small & neat. The main changes right now are expansions to Lua's syntax, but performance optimizations and QoL features like optional compile-time warnings are under development too.

Pluto will retain a heavy focus on portability, but it will be larger & less light-weight than Lua. I intend on adding features to this that'll make Lua (or in this case, Pluto) more general-purpose and bring more attention to the Lua programming language, which I deeply love. Pluto will not grow to the enormous size of Python, obviously, but the standard library will most definitely be expanded. So far, many operators have been added & overhauled. There are new virtual machine optimizations, new syntax such as lambda expressions & string indexing, and more that you can read below.

### Note
This is a learner's project concerning the internals of Lua. I will often add and change things simply to learn more about my environment. As a result, there will be breaking changes very often. There may be bugs, and there will be design choices that people probably don't agree with. However, this will become a good base to write more Lua patches from. I do welcome other people in using Pluto as a reference for their own Lua patches — because Pluto offers some neat improvements over Lua already.

## Breaking changes:
- None.

Pluto is fully backwards-compatible (inc. C-ABI) with Lua 5.4. This includes compiled bytecode, because Pluto does not implement any new opcodes, or alter the fundamental behavior of any opcodes.
## New Features:
### Dedicated Exponent Operator
The `**` operator has been implemented into the operator set. It's roughly an alternative to `^` now.
### Arbitrary Characters in Numeral Literals
Long numbers can get confusing to read for some people. `1_000_000` is now a valid alternative to `1000000`.
### String Indexing
String indexing makes character-intensive tasks nearly 3x faster. The syntax is as follows:
```lua
local str = "hello world"
assert(str[5] == "o")
assert(str[200] == nil)
```
This is a very nice addition because it avoids a lookup and function call for each character. Things like (i.e, hash algorithms) will significantly benefit from this change. Furthermore, it's friendly with normal instance lookups. So, things like `str["char"]` still work fine. String indexing is invoked whenever a string index is invoked with an integer.
### Lambda Expressions
Without the size constraint of Lua, there's no need to hold weary of shorthand expressions.
Here's example usage of the new lambda expressions:
```lua
local str = "123"
local inc_str = str:gsub(".", |c| -> tonumber(c) + 1)
assert(inc_str == "234")
```
- The '|' token was chosen because it's not commonly used as an unary operator in programming.
- The '->' arrow syntax looked better and didn't resemble any operators. It also plays along with common lambda tokens.
### Ternary Expressions
Ternary expressions allow seamless implementation of short statements which may assign falsy values. It also allows shorter, and more concise logic. Here is an example.
```lua
local max
if a > b then
  max = a
else
  max = b
end
```
Can be translated into this:
```lua
local max = if a > b then a else b
```
Essentially, `if` symbols inside expressions are ternary. Otherwise, they are statements. Ternary expressions don't accept `end` as a termination, the control structure will jump to evaluate the sub-expression. Sub-expressions are evaluated on a need-to-know basis, such that the `else` block will never run if the preceding logic is true. Unlike the `if` statement, ternary expressions do not support of `else if`, this is outside the purpose of ternary expressions in C.

Traditionally, this would be possible with the `and` symbol, however I personally dislike this syntax because it's inconsistent with other languages. Until informed otherwise, people will assume `and` exclusively returns booleans. It's not immediately obvious — in fact, I meet many good Lua programmers who are unaware — that `and` can be used to emulate ternary expressions. The `if/then/else` syntax is imperfect, but I believe it's better. Not only visually, but also in functionality; such that `and` depends on the truthyness of its operands.
### Continue Statement
Although very similar to goto usage, the continue statement has been implemented into Pluto. Usage follows:
```lua
-- Print every number besides five.
for i = 1, 10 do
    if i == 5 then
        continue
    end
    print(i)
    -- continue jumps here.
end
```
The new statement will jump to the end of the loop, so it may proceed to the next iteration. For all intents and purposes, it's the same as this:
```lua
for i = 1, 10 do
    if i == 5 then
        goto continue
    end
    print(i)
    ::continue::
end
```
However, the dedicated statement doesn't complicate pre-defined goto labels, aligns with other language routines, and is slightly more user-friendly. The `continue` statement also isn't limited by the negatives a label would imply, so you don't need to manage local scopes and other pedantry like you would a label. It's important to note, this new statement will jump over any code neccesary to end the loop. Meaning, if you jump over vital code that determines the conditional for your loop, then you will produce a bug.
### Compiler Warnings
Pluto now offers optional compiler warnings for certain misbehaviors. Currently, this is applied only to duplicated local definitions. These internal checks are faster, and more reliable than analytical third-party software. Compiler warnings need to be explicity enabled with the `-D` flag, which is optimal for developers and users alike. For an example, see this code:
```lua
local variable = "hello world"
do
    local variable = "shadowed"
end
```
Given you run the file like this: `pluto -D file.plu`, the parser will emit the following message:
```
tests/quick.lua:3: warning: duplicate local declaration [-D]
         3 | local variable =
           | ^^^^^^^^^^^^^^^^ here: this shadows the value of the initial declaration on line 1.
           |
```
This feature can be removed from Pluto via the `PLUTO_PARSER_WARNING_LOCALDEF` macro in `luaconf.h`.
### Augmented Operators
The following augmented operators have been added:
- Modulo: `%=`
- Addition: `+=`
- Exponent: `^=`
- Bitwise OR: `|=`
- Subtraction: `-=`
- Bitshift left: `<<=`
- Bitwise AND: `&=`
- Float division: `/=`
- Bitshift right: `>>=`
- Multiplication: `*=`
- Integer division: `//=`

These are all syntactic sugar for the usual binary operation & manual assignment, as such they produce nearly the same bytecode. In some instances, Pluto's augmented operators are faster than Lua. This happens because augmented operators temporarily store the left-hand operand inside of a register & share the value to the expression, whereas Lua would request the value twice.
## Optimizations:
### For Loops
Xmilia Hermit discovered an interesting for loop optimization on June 7th, 2022. It has been implemented in Pluto.
For example, take this code:
```lua
local t = {}
for i = 1, 10000000 do
    t[i] = i
end

local hidetrace
local start = os.clock()
for key, value in ipairs(t) do
    hidetrace = key
    hidetrace = value
end
print(os.clock() - start)
```
It takes roughly 650ms to run on my machine. Following the VM optimization, it takes roughly 160ms.
This optimization takes place in any loop that doesn't access the TBC variable returned by the `pairs` and `ipairs` function. See these returns:
```
pairs: next, table, nil, nil
ipairs: ipairsaux, table, integer, nil
```
When the latter `nil` TBC variable is never accessed, this optimization will occur.
## QoL Improvements
These are modifications that don't really add something new, but improve existing behavior.

### Excessive Iteration Prevention
This is an optional, but fairly simple heuristic that limits the amount of backward-jumps performed prior to a forward-jump, which applies to loops. Every backward-jump is usually a new iteration, and forward jumps are usually escaping statements. Taking advantage of this circumstance, Pluto can bottleneck the amount of backward-jumps permitted to avoid excessive iterations — which are usually problematic for blocking game threads — and prevent the operation of infinite loops, or loops which may crash the game thread.

This supports a statically-defined bottleneck — `MAX_LOOP_ITERATIONS` in `luaconf.h` — which defines the amount of permitted iterations. This also supports hooking the `OP_CALL` opcode, and preventing loop termination if a certain function is called. This was implemented because game threads usually force users to call some sort of `yield` mechanism to prevent a crash — `FUNCTION_NAME_TO_HOOK` in `luaconf.h` — but, the function must be avalible in the Pluto runtime. `ERROR_FOR_PREVENTION` can also be enabled to throw a runtime error during exception, otherwise Pluto will simply break out of the loop.

### Syntax Errors
Certain syntax errors now have more descriptive error messages. For example:
```lua
if a < b and t == 5 return "Gottem" end
```
Produces the following result:
```
pluto: tests/quick.lua:1: syntax error: expected 'then' to delimit condition.
         1 | if a < b and t == 5 return
           | ^^^^^^^^^^^^^^^^^^^^^^^^^^ here: expected 'then' symbol.
           |
```
Another example, with a mistyped lambda expression:
```lua
local fn = |a, b, c| => (a == b and a < c)
```
Emits this message:
```
pluto: tests/quick.lua:4: syntax error: impromper lambda definition
         4 | local b = |a, b, c| =>
           | ^^^^^^^^^^^^^^^^^^^^^^ here: expected '->' arrow syntax for lambda expression.
           |
```
This also supports ANSI color codes, however this is disabled by default in order to encourage portability. For example, ANSI color codes do not work on most Windows command prompts. Define the `PLUTO_USE_COLORED_OUTPUT` macro in `luaconf.h` to enable colored error messages — they look quite nice.
- For most Windows 10 users, you can enable ANSI color code support with this shell command:
  - `REG ADD HKCU\CONSOLE /f /v VirtualTerminalLevel /t REG_DWORD /d 1`

## Standard Library Additions
### `_G`
- `newuserdata` function.
### `string`
- `endswith` function.
- `startswith` function.

## Building Pluto
Lua, and hence Pluto is very easy to build. Pluto was built on C++17, with no backwards-compatibility ensured.
### Windows
- Install `MSYS` & add it to path.
- Install `chocolatey` & add it to path.
- Run `make` in the root directory of Pluto.
```
git clone https://github.com/well-in-that-case/Pluto.git
make
```

For other platforms, use `make PLAT=xxx` where `xxx` is a valid platform, found inside the makefile configuration.