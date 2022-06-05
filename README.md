# Pluto
Pluto is a fork of the Lua 5.4.4 programming language. Internally, it's 99.5% PuC-Rio Lua 5.4, but as I continue to make small modifications, over time it'll be its own little thing. This is why I ascribed the name 'Pluto', because it'll always be something small & neat. The main changes right now are overhauls to Lua's operators, which will be adjusted to meet the C standard.

Pluto will not have a heavy focus on light-weight and embeddability, but it'll be kept in mind. I intend on adding features to this that'll make Lua (or in this case, Pluto) more general-purpose and bring more attention to the Lua programming language, which I deeply love. Pluto will not grow to the enormous size of Python, obviously, but the standard library will most definitely be expanded. Standardized object orientation in the form of classes is planned, but there's no implementation abstract yet.

### Breaking changes:
- `!=` is the new inequality operator.
- The `^` operator now performs bitwise XOR instead of exponentiation.
- The former `~=` inequality operator has been changed to an augmented bitwise NOT assignment.

### Optimizations:
- Cases of `x = x ** 2` are now optimized into `x = x * x` by the parser. This is 35% faster.
  - There's one major caveat: Users hooking `__mul` may become confused.

### New Operators:
#### Arithmetic Operators:
- Exponent: `**`

#### Augmented Operators:
- Modulo: `%=`
- Addition: `+=`
- Bitwise OR: `|=`
- Subtraction: `-=`
- Bitwise XOR: `^=`
- Bitwise NOT: `~=`
- Bitshift left: `<<=`
- Bitwise AND: `&=`
- Float division: `/=`
- Bitshift right: `>>=`
- Multiplication: `*=`
- Integer division: `//=`

### Standard Library Additions:
#### `_G`
- `newuserdata` function.
#### `string`
- `endswith` function.
- `startswith` function.