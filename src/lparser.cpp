/*
** $Id: lparser.c $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <string>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include "lua.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lauxlib.h"



/* maximum number of local variables per function (must be smaller
   than 250, due to the bytecode format) */
#define MAXVARS		249


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/*
** Invokes the lua_writestring macro with a std::string.
*/
#define write_std_string(std_string) lua_writestring(std_string.data(), std_string.size())


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


#define luaO_fmt luaO_pushfstring


/*
** nodes for block list (list of active blocks)
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* chain */
  int scopeend;  /* delimits the end of this scope, for 'continue' to jump before. */
  int firstlabel;  /* index of first label in this block */
  int firstgoto;  /* index of first pending goto in this block */
  lu_byte nactvar;  /* # active locals outside the block */
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  lu_byte isloop;  /* true if 'block' is a loop */
  lu_byte insidetbc;  /* true if inside the scope of a to-be-closed var. */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement (LexState *ls, lu_byte *prop = nullptr);
static void expr (LexState *ls, expdesc *v, lu_byte *prop = nullptr);


/*
** Formats an error with the appropriate source code snippet.
*/
static const char *format_line_error (LexState *ls, const char *msg, const char *token, const char *here,
                                      LexState::SourceInfoStrategy strat = LexState::CURRENT) {
  const auto linenumber = ls->GetLineNumber(strat);
  std::string pad_str(std::to_string(linenumber).length(), ' ');
  const char *pad = pad_str.c_str();
  const char *text = luaG_addinfo(ls->L, msg, ls->source, linenumber);
#ifdef PLUTO_SHORT_ERRORS
#ifdef PLUTO_USE_COLORED_OUTPUT
  text = luaO_fmt(ls->L, "%s%s%s", YEL, text, RESET);
#endif // PLUTO_USE_COLORED_OUTPUT
  return text;
#endif
#ifndef PLUTO_USE_COLORED_OUTPUT
  text = luaO_fmt(ls->L, "%s\n\t%s%d | %s\n\t%s%s | %s\n\t%s%s |",
                          text, pad, linenumber, token, pad, pad, here, pad, pad);
#else // PLUTO_USE_COLORED_OUTPUT
  text = luaO_fmt(ls->L, "%s%s%s\n\t%s%d | %s\n\t%s%s | %s\n\t%s%s |",
                          YEL, text, RESET, pad, linenumber, token, pad, pad, here, pad, pad);
#endif // PLUTO_USED_COLORED_OUTPUT
  return text;
}


/*
** Applies coloring (if permitted) to 's'.
*/
static std::string make_here(const std::string& linebuff, const char *s) {
  std::string here = std::string(linebuff.size(), '^');
  here.append(" here: ");
#ifdef PLUTO_USE_COLORED_OUTPUT
  here.insert(0, std::string(RED));
  here.append(s);
  here.append(RESET);
#else
  here.append(s);
#endif
  return here;
}


/*
** Applies coloring (if permitted) to an invalid synax error message.
*/
static std::string make_err(const char *s) {
  std::string error = std::string(s);
  error.insert(0, "syntax error: ");
#ifdef PLUTO_USE_COLORED_OUTPUT
  error.insert(0, std::string(RED));
  error.insert(19, std::string(BWHT));
  error.append(RESET);
#endif
  return error;
}


/*
** Applies coloring (if permitted) to a warning message.
*/
static std::string make_warn(const char *s) {
  std::string error = std::string(s);
  error.insert(0, "warning: ");
#ifdef PLUTO_USE_COLORED_OUTPUT
  error.insert(0, std::string(RED));
  error.insert(error.find("warning:") + 8, std::string(BWHT));
  error.append(RESET);
#endif
  return error;
}


/*
** Throws an exception into Lua, which will promptly close the program.
** This is only called for vital errors, like lexer and/or syntax problems.
*/
[[noreturn]] static void throwerr (LexState *ls, const char *err, const char *here) {
  ls->linenumber = ls->GetLastLineNumber();
  const std::string& linebuff = ls->GetLatestLine();
  std::string error = make_err(err);
  std::string rhere = make_here(linebuff, here);
  format_line_error(ls, error.c_str(), linebuff.c_str(), rhere.c_str());
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}


/*
** Throws an warning into standard output, which will not close the program.
*/
static void throw_warn (LexState *ls, const char *err, const char *here, LexState::SourceInfoStrategy strat = LexState::CURRENT) {
  const std::string& linebuff = ls->GetLineBuff(strat);
  std::string error = make_warn(err);
  std::string rhere = make_here(linebuff, here);
  lua_warning(ls->L, format_line_error(ls, error.c_str(), linebuff.c_str(), rhere.c_str(), strat), 0);
  ls->L->top -= 2; /* remove warning from stack */
}

static void throw_warn(LexState* ls, const char* err, int linenumber) {
  auto msg = luaG_addinfo(ls->L, err, ls->source, linenumber);
  lua_warning(ls->L, msg, 0);
  ls->L->top -= 1; /* remove warning from stack */
}


/*
** This function will throw an exception and terminate the program.
*/
[[noreturn]] static void error_expected (LexState *ls, int token) {
  switch (token) {
    case '|': {
      throwerr(ls,
        "expected '|' to control parameters.",
        "expected '|' to begin & terminate the lambda's paramater list.");
    }
    case '-': {
      if (luaX_lookahead(ls) == '>') {
        throwerr(ls,
          "impromper lambda definition",
          "expected '->' arrow syntax for lambda expression.");
      }
      goto _default; // Run-through default case, no more work to be done.
    }
    case TK_IN: {
      throwerr(ls,
        "expected 'in' to delimit loop iterator.", "expected 'in' symbol.");
    }
    case TK_DO: {
      throwerr(ls,
        "expected 'do' to establish block.", "you need to append this with the 'do' symbol.");
    }
    case TK_END: {
      throwerr(ls,
        "expected 'end' to terminate block.", "expected 'end' symbol after or on this line.");
    }
    case TK_THEN: {
      throwerr(ls,
        "expected 'then' to delimit condition.", "expected 'then' symbol.");
    }
    case TK_NAME: {
      throwerr(ls,
        "expected an identifier.", "this needs a name.");
    }
    case TK_PCONTINUE: {
      throwerr(ls,
        "expected 'continue' inside a loop.", "this is not within a loop.");
    }
    default: {
      _default:
      throwerr(ls,
        luaO_fmt(ls->L, "%s expected (got %s)",
          luaX_token2str(ls, token), luaX_token2str(ls, ls->t.token)), "this is invalid syntax.");
    }
  }
}


[[noreturn]] static void errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  luaX_syntaxerror(fs->ls, msg);
}


static void checklimit (FuncState *fs, int v, int l, const char *what) {
  if (v > l) errorlimit(fs, l, what);
}


/*
** Test whether next token is 'c'; if so, skip it.
*/
static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check that next token is 'c'.
*/
static void check (LexState *ls, int c) {
  if (ls->t.token != c) {
    error_expected(ls, c);
  }
}


/*
** Check that next token is 'c' and skip it.
*/
static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
static void check_match (LexState *ls, int what, int who, int where) {
  if (l_unlikely(!testnext(ls, what))) {
    if (where == ls->linenumber)  /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else {
      switch (what) {
        case TK_END: {
          /*
          ** We need the previous buffers.
          ** The error is only thrown after meeting a new line.
          */
          ls->linebuff = ls->lastlinebuff; // Line of last statement.
          ls->linenumber = ls->lastlinebuffnum; // Line number of last statement.
          switch (who) {
            case TK_IF: {
              throwerr(ls,
                "missing 'end' to terminate 'if' statement.", "this was the last statement.");
            }
            case TK_DO: {
              throwerr(ls,
                "missing 'end' to terminate 'do' block.", "this was the last statement.");
            }
            case TK_FOR: {
              throwerr(ls,
                "missing 'end' to terminate 'for' block.", "this was the last statement.");
            }
            case TK_WHILE: {
              throwerr(ls,
                "missing 'end' to terminate 'while' block.", "this was the last statement.");
            }
            case TK_FUNCTION: {
              throwerr(ls,
                "missing 'end' to terminate 'function' block.", "this was the last statement.");
            }
            default: {
              throwerr(ls,
                "missing 'end' to terminate block.", "missing termination.");
            }
          }
        }
        default: {
          std::string err = make_err("%s expected (to close %s at line %d)");
          luaK_semerror(ls,
            luaO_fmt(ls->L, err.c_str(),
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
        }
      }
    }
  }
}


static TString *str_checkname (LexState *ls, bool strict = false) {
  TString *ts;
  if (ls->t.token != TK_NAME && (strict || !ls->t.IsReservedNonValue())) {
    error_expected(ls, TK_NAME);
  }
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}


static void codestring (expdesc *e, TString *s) {
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->u.strval = s;
}


static void codename (LexState *ls, expdesc *e) {
  codestring(e, str_checkname(ls));
}


/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
static int registerlocalvar (LexState *ls, FuncState *fs, TString *varname) {
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->ndebugvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  while (oldsize < f->sizelocvars)
    f->locvars[oldsize++].varname = NULL;
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}


#define new_localvarliteral(ls,v) \
    new_localvar(ls,  \
      luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char)) - 1));



/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
static Vardesc *getlocalvardesc (FuncState *fs, int vidx) {
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}


[[nodiscard]] static lu_byte gettypehint(LexState *ls) noexcept {
  /* TYPEHINT -> [':' Typename] */
  if (testnext(ls, ':')) {
    const char* tname = getstr(str_checkname(ls));
    if (strcmp(tname, "number") == 0)
      return VKINT ;
    else if (strcmp(tname, "table") == 0)
      return VNONRELOC;
    else if (strcmp(tname, "string") == 0)
      return VKSTR;
    else if (strcmp(tname, "userdata") == 0)
      return 0xFF;
    else if (strcmp(tname, "boolean") == 0 || strcmp(tname, "bool") == 0)
      return VTRUE;
    else if (strcmp(tname, "nil") == 0)
      return VNIL;
    else if (strcmp(tname, "function") == 0)
      return 0xFF;
    else
      luaK_semerror(ls,
        luaO_pushfstring(ls->L, "unknown type hint '%s'", tname));
  }
  return 0xFF;
}


[[nodiscard]] static const char* vk_toTypeString(lu_byte kind) noexcept {
  switch (kind)
  {
  case VKINT: case VKFLT: return "number";
  case VNONRELOC: return "table";
  case VKSTR: return "string";
  case VTRUE: case VFALSE: return "boolean";
  case VNIL: return "nil";
  }
  return "ERROR";
}


static void process_assign(LexState* ls, Vardesc* var, lu_byte k) {
  if (var->vd.typehint != 0xFF && /* var has type hint? */
      k != 0xFF && /* e has known return type? */
      var->vd.typehint != k /* type mismatch? */
      ) {
    std::string err = var->vd.name->toCpp();
    err.append(" was type-hinted as ");
    err.append(vk_toTypeString(var->vd.typehint));
    err.append(" but is assigned a ");
    err.append(vk_toTypeString(k));
    err.append(" value");
    throw_warn(ls, err.c_str(), "type mismatch", LexState::LAST);
  }

  var->vd.typeprop = k; /* propagate type */
}


/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
static int reglevel (FuncState *fs, int nvar) {
  while (nvar-- > 0) {
    Vardesc *vd = getlocalvardesc(fs, nvar);  /* get previous variable */
    if (vd->vd.kind != RDKCTC)  /* is in a register? */
      return vd->vd.ridx + 1;
  }
  return 0;  /* no variables in registers */
}


/*
** Return the number of variables in the register stack for the given
** function.
*/
int luaY_nvarstack (FuncState *fs) {
  return reglevel(fs, fs->nactvar);
}


/*
** Get the debug-information entry for current variable 'vidx'.
*/
static LocVar *localdebuginfo (FuncState *fs, int vidx) {
  Vardesc *vd = getlocalvardesc(fs, vidx);
  if (vd->vd.kind == RDKCTC)
    return NULL;  /* no debug info. for constants */
  else {
    int idx = vd->vd.pidx;
    lua_assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}


/*
** Create a new local variable with the given 'name'. Return its index
** in the function.
*/
static int new_localvar (LexState *ls, TString *name) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  Vardesc *var;
#ifndef PLUTO_NO_PARSER_WARNINGS
  int locals = luaY_nvarstack(fs);
  for (int i = fs->firstlocal; i < locals; i++) {
    Vardesc *desc = getlocalvardesc(fs,  i);
    LocVar *local = localdebuginfo(fs, i);
    std::string n = name->toCpp();
    if ((n != "(for state)" && n != "(switch)") && (local && local->varname == name)) { // Got a match.
      throw_warn(ls,
        "duplicate local declaration",
          luaO_fmt(L, "this shadows the value of the initial declaration on line %d.", desc->vd.linenumber));
      L->top--; /* pop result of luaO_fmt */
    }
  }
#endif
  checklimit(fs, dyd->actvar.n + 1 - fs->firstlocal,
                 MAXVARS, "local variables");
  luaM_growvector(L, dyd->actvar.arr, dyd->actvar.n + 1,
                  dyd->actvar.size, Vardesc, USHRT_MAX, "local variables");
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->vd.kind = VDKREG;  /* default */
  var->vd.typehint = 0xFF;
  var->vd.typeprop = 0xFF;
  var->vd.name = name;
  var->vd.linenumber = ls->linenumber;
  return dyd->actvar.n - 1 - fs->firstlocal;
}


/*
** Create an expression representing variable 'vidx'
*/
static void init_var (FuncState *fs, expdesc *e, int vidx) {
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->u.var.vidx = vidx;
  e->u.var.ridx = getlocalvardesc(fs, vidx)->vd.ridx;
}


/*
** Raises an error if variable described by 'e' is read only
*/
static void check_readonly (LexState *ls, expdesc *e) {
  FuncState *fs = ls->fs;
  TString *varname = NULL;  /* to be set if variable is const */
  switch (e->k) {
    case VCONST: {
      varname = ls->dyd->actvar.arr[e->u.info].vd.name;
      break;
    }
    case VLOCAL: {
      Vardesc *vardesc = getlocalvardesc(fs, e->u.var.vidx);
      if (vardesc->vd.kind != VDKREG)  /* not a regular variable? */
        varname = vardesc->vd.name;
      break;
    }
    case VUPVAL: {
      Upvaldesc *up = &fs->f->upvalues[e->u.info];
      if (up->kind != VDKREG)
        varname = up->name;
      break;
    }
    default:
      return;  /* other cases cannot be read-only */
  }
  if (varname) {
    const char *msg = luaO_fmt(ls->L, "attempt to reassign constant '%s'", getstr(varname));
    const char *here = "this variable is constant, and cannot be reassigned.";
    throwerr(ls, luaO_fmt(ls->L, msg, getstr(varname)), here);
  }
}


/*
** Start the scope for the last 'nvars' created variables.
*/
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++) {
    int vidx = fs->nactvar++;
    Vardesc *var = getlocalvardesc(fs, vidx);
    var->vd.ridx = reglevel++;
    var->vd.pidx = registerlocalvar(ls, fs, var->vd.name);
  }
}


/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
static void removevars (FuncState *fs, int tolevel) {
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  while (fs->nactvar > tolevel) {
    LocVar *var = localdebuginfo(fs, --fs->nactvar);
    if (var)  /* does it have debug information? */
      var->endpc = fs->pc;
  }
}


/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}


static Upvaldesc *allocupvalue (FuncState *fs) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  return &f->upvalues[fs->nups++];
}


static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  Upvaldesc *up = allocupvalue(fs);
  FuncState *prev = fs->prev;
  if (v->k == VLOCAL) {
    up->instack = 1;
    up->idx = v->u.var.ridx;
    up->kind = getlocalvardesc(prev, v->u.var.vidx)->vd.kind;
    lua_assert(eqstr(name, getlocalvardesc(prev, v->u.var.vidx)->vd.name));
  }
  else {
    up->instack = 0;
    up->idx = cast_byte(v->u.info);
    up->kind = prev->f->upvalues[v->u.info].kind;
    lua_assert(eqstr(name, prev->f->upvalues[v->u.info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}


/*
** Look for an active local variable with the name 'n' in the
** function 'fs'. If found, initialize 'var' with it and return
** its expression kind; otherwise return -1.
*/
static int searchvar (FuncState *fs, TString *n, expdesc *var) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    Vardesc *vd = getlocalvardesc(fs, i);
    if (eqstr(n, vd->vd.name)) {  /* found? */
      if (vd->vd.kind == RDKCTC)  /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else  /* real variable */
        init_var(fs, var, i);
      return var->k;
    }
  }
  return -1;  /* not found */
}


/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
  fs->needclose = 1;
}


/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}


/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL)  /* no more levels? */
    init_exp(var, VVOID, 0);  /* default is global */
  else {
    int v = searchvar(fs, n, var);  /* look up locals at current level */
    if (v >= 0) {  /* found? */
      if (v == VLOCAL && !base)
        markupval(fs, var->u.var.vidx);  /* local will be used as an upval */
    }
    else {  /* not found as local at current level; try upvalues */
      int idx = searchupvalue(fs, n);  /* try existing upvalues */
      if (idx < 0) {  /* not found? */
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
        if (var->k == VLOCAL || var->k == VUPVAL)  /* local or upvalue? */
          idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
        else  /* it is a global or a constant */
          return;  /* don't need to do anything at this level */
      }
      init_exp(var, VUPVAL, idx);  /* new or old upvalue */
    }
  }
}


/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
static void singlevar (LexState *ls, expdesc *var) {
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  singlevaraux(fs, varname, var, 1);
  if (var->k == VVOID) {  /* global name? */
    expdesc key;
    singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
    lua_assert(var->k != VVOID);  /* this one must exist */
    codestring(&key, varname);  /* key is variable name */
    luaK_indexed(fs, var, &key);  /* env[varname] */
  }
}


/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  int needed = nvars - nexps;  /* extra values needed */
  if (hasmultret(e->k)) {  /* last expression has multiple returns? */
    int extra = needed + 1;  /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
  }
  else {
    if (e->k != VVOID)  /* at least one expression? */
      luaK_exp2nextreg(fs, e);  /* close last expression */
    if (needed > 0)  /* missing values? */
      luaK_nil(fs, fs->freereg, needed);  /* complete with nils */
  }
  if (needed > 0)
    luaK_reserveregs(fs, needed);  /* registers for extra values */
  else  /* adding 'needed' is actually a subtraction */
    fs->freereg += needed;  /* remove extra values */
}


#define enterlevel(ls)	luaE_incCstack(ls->L)


#define leavelevel(ls) ((ls)->L->nCcalls--)


/*
** Generates an error that a goto jumps into the scope of some
** local variable.
*/
[[noreturn]] static void jumpscopeerror (LexState *ls, Labeldesc *gt) {
  const char *varname = getstr(getlocalvardesc(ls->fs, gt->nactvar)->vd.name);
  const char *msg = "<goto %s> at line %d jumps into the scope of local '%s'";
  msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line, varname);
  luaK_semerror(ls, msg);  /* raise the error */
}


/*
** Solves the goto at index 'g' to given 'label' and removes it
** from the list of pending goto's.
** If it jumps into the scope of some variable, raises an error.
*/
static void solvegoto (LexState *ls, int g, Labeldesc *label) {
  int i;
  Labellist *gl = &ls->dyd->gt;  /* list of goto's */
  Labeldesc *gt = &gl->arr[g];  /* goto to be resolved */
  lua_assert(eqstr(gt->name, label->name));
  if (l_unlikely(gt->nactvar < label->nactvar))  /* enter some scope? */
    jumpscopeerror(ls, gt);
  luaK_patchlist(ls->fs, gt->pc, label->pc);
  for (i = g; i < gl->n - 1; i++)  /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** Search for an active label with the given name.
*/
static Labeldesc *findlabel (LexState *ls, TString *name) {
  int i;
  Dyndata *dyd = ls->dyd;
  /* check labels in current function for a match */
  for (i = ls->fs->firstlabel; i < dyd->label.n; i++) {
    Labeldesc *lb = &dyd->label.arr[i];
    if (eqstr(lb->name, name))  /* correct label? */
      return lb;
  }
  return NULL;  /* label not found */
}


/*
** Adds a new label/goto in the corresponding list.
*/
static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


static int newgotoentry (LexState *ls, TString *name, int line, int pc) {
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}


/*
** Solves forward jumps. Check whether new label 'lb' matches any
** pending gotos in current block and solves them. Return true
** if any of the goto's need to close upvalues.
*/
static int solvegotos (LexState *ls, Labeldesc *lb) {
  Labellist *gl = &ls->dyd->gt;
  int i = ls->fs->bl->firstgoto;
  int needsclose = 0;
  while (i < gl->n) {
    if (eqstr(gl->arr[i].name, lb->name)) {
      needsclose |= gl->arr[i].close;
      solvegoto(ls, i, lb);  /* will remove 'i' from the list */
    }
    else
      i++;
  }
  return needsclose;
}


/*
** Create a new label with the given 'name' at the given 'line'.
** 'last' tells whether label is the last non-op statement in its
** block. Solves all pending goto's to this new label and adds
** a close instruction if necessary.
** Returns true iff it added a close instruction.
*/
static int createlabel (LexState *ls, TString *name, int line,
                        int last) {
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last) {  /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
  if (solvegotos(ls, &ll->arr[l])) {  /* need close? */
    luaK_codeABC(fs, OP_CLOSE, luaY_nvarstack(fs), 0, 0);
    return 1;
  }
  return 0;
}


/*
** Adjust pending gotos to outer level of a block.
*/
static void movegotosout (FuncState *fs, BlockCnt *bl) {
  int i;
  Labellist *gl = &fs->ls->dyd->gt;
  /* correct pending gotos to current block */
  for (i = bl->firstgoto; i < gl->n; i++) {  /* for each pending goto */
    Labeldesc *gt = &gl->arr[i];
    /* leaving a variable scope? */
    if (reglevel(fs, gt->nactvar) > reglevel(fs, bl->nactvar))
      gt->close |= bl->upval;  /* jump may need a close */
    gt->nactvar = bl->nactvar;  /* update goto level */
  }
}


static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->scopeend = NO_JUMP;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  bl->insidetbc = static_cast<lu_byte>(fs->bl != NULL && fs->bl->insidetbc);
  bl->previous = fs->bl;
  fs->bl = bl;
  lua_assert(fs->freereg == luaY_nvarstack(fs));
}


/*
** generates an error for an undefined 'goto'.
*/
[[noreturn]] static void undefgoto (LexState *ls, Labeldesc *gt) {
  const char *msg;
  if (eqstr(gt->name, luaS_newliteral(ls->L, "break"))) {
    msg = "break outside loop at line %d";
    msg = luaO_pushfstring(ls->L, msg, gt->line);
  }
  else {
    msg = "no visible label '%s' for <goto> at line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line);
  }
  luaK_semerror(ls, msg);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  LexState *ls = fs->ls;
  int hasclose = 0;
  int stklevel = reglevel(fs, bl->nactvar);  /* level outside the block */
  if (bl->isloop)  /* fix pending breaks? */
    hasclose = createlabel(ls, luaS_newliteral(ls->L, "break"), 0, 0);
  if (!hasclose && bl->previous && bl->upval)
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->bl = bl->previous;
  removevars(fs, bl->nactvar);
  lua_assert(bl->nactvar == fs->nactvar);
  fs->freereg = stklevel;  /* free registers */
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
  if (bl->previous)  /* inner block? */
    movegotosout(fs, bl);  /* update pending gotos to outer block */
  else {
    if (bl->firstgoto < ls->dyd->gt.n)  /* pending gotos in outer block? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
  }
}


/*
** adds a new prototype into list of prototypes
*/
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
static void codeclosure (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs->prev;
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  Proto *f = fs->f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = NULL;
  f->source = ls->source;
  luaC_objbarrier(ls->L, f, f->source);
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  enterblock(fs, bl, 0);
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  luaK_ret(fs, luaY_nvarstack(fs), 0);  /* final return */
  leaveblock(fs);
  lua_assert(fs->bl == NULL);
  luaK_finish(fs);
  luaM_shrinkvector(L, f->code, f->sizecode, fs->pc, Instruction);
  luaM_shrinkvector(L, f->lineinfo, f->sizelineinfo, fs->pc, ls_byte);
  luaM_shrinkvector(L, f->abslineinfo, f->sizeabslineinfo,
                       fs->nabslineinfo, AbsLineInfo);
  luaM_shrinkvector(L, f->k, f->sizek, fs->nk, TValue);
  luaM_shrinkvector(L, f->p, f->sizep, fs->np, Proto *);
  luaM_shrinkvector(L, f->locvars, f->sizelocvars, fs->ndebugvars, LocVar);
  luaM_shrinkvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  ls->fs = fs->prev;
  luaC_checkGC(L);
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
      return 1;
    case TK_PWHEN:
#ifndef PLUTO_COMPATIBLE_WHEN
    case TK_WHEN:
#endif
    case TK_UNTIL: return withuntil;
    default: return 0;
  }
}


static void statlist (LexState *ls, lu_byte *prop = nullptr) {
  /* statlist -> { stat [';'] } */
  while (!block_follow(ls, 1)) {
    if (ls->t.token == TK_RETURN) {
      statement(ls, prop);
      return;  /* 'return' must be last statement */
    }
    lu_byte p = 0xFE;
    statement(ls, &p);
    if (p != 0xFE) prop = nullptr; /* multiple return paths, don't propagate return type */
  }
}


inline int gett(LexState *ls) {
  return ls->t.token;
}


/* Switch logic partially inspired by Paige Marie DePol from the Lua mailing list. */
static void caselist (LexState *ls, bool isdefault) {
  while (gett(ls) != TK_PDEFAULT
      && gett(ls) != TK_PCASE
      && gett(ls) != TK_END
#ifndef PLUTO_COMPATIBLE_DEFAULT
      && gett(ls) != TK_DEFAULT
#endif
#ifndef PLUTO_COMPATIBLE_CASE
      && gett(ls) != TK_CASE
#endif
    ) {
    if (isdefault && gett(ls) == TK_BREAK && luaX_lookahead(ls) == TK_END) {
      luaX_next(ls);
    }
    else {
      if (gett(ls) == TK_PCONTINUE
#ifndef PLUTO_COMPATIBLE_CONTINUE
          || gett(ls) == TK_CONTINUE
#endif
          ) {
        throwerr(ls, "'continue' outside of loop.", "'case' statements are not loops.");
      }
      else {
        statement(ls);
      }
    }
  }
}


static void fieldsel (LexState *ls, expdesc *v) {
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  luaK_exp2anyregup(fs, v);
  luaX_next(ls);  /* skip the dot or colon */
  codename(ls, &key);
  luaK_indexed(fs, v, &key);
}


static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


typedef struct ConsControl {
  expdesc v;  /* last list item read */
  expdesc *t;  /* table descriptor */
  int nh;  /* total number of 'record' elements */
  int na;  /* number of array elements already stored */
  int tostore;  /* number of array elements pending to be stored */
} ConsControl;


static void recfield (LexState *ls, ConsControl *cc) {
  /* recfield -> (NAME | '['exp']') = exp */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME) {
    checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    codename(ls, &key);
  }
  else  /* ls->t.token == '[' */
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  expr(ls, &val);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg;  /* free registers */
}

static void prenamedfield(LexState* ls, ConsControl* cc, const char* name) {
  FuncState* fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc tab, key, val;
  codestring(&key, luaX_newstring(ls, name));
  cc->nh++;
  luaX_next(ls); /* skip name token */
  checknext(ls, '=');
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  expr(ls, &val);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg;  /* free registers */
}

static void closelistfield (FuncState *fs, ConsControl *cc) {
  if (cc->v.k == VVOID) return;  /* there is no list item */
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0;  /* no more items pending */
  }
}


static void lastlistfield (FuncState *fs, ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}


static void listfield (LexState *ls, ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}


static void body (LexState *ls, expdesc *e, int ismethod, int line, lu_byte *prop = nullptr);
static void funcfield (LexState *ls, struct ConsControl *cc) {
  /* funcfield -> function NAME funcargs */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc tab, key, val;
  cc->nh++;
  luaX_next(ls); /* skip TK_FUNCTION */
  codename(ls, &key);
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  body(ls, &val, true, ls->linenumber);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg;  /* free registers */
}


static void field (LexState *ls, ConsControl *cc) {
  /* field -> listfield | recfield | funcfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      if (luaX_lookahead(ls) != '=')  /* expression? */
        listfield(ls, cc);
      else
        recfield(ls, cc);
      break;
    }
    case '[': {
      recfield(ls, cc);
      break;
    }
    case TK_FUNCTION: {
      if (luaX_lookahead(ls) == '(') {
        listfield(ls, cc);
      }
      else {
        funcfield(ls, cc);
      }
      break;
    }
    default: {
      if (ls->t.IsReservedNonValue()) {
        prenamedfield(ls, cc, luaX_reserved2str(ls->t.token));
      } else {
        listfield(ls, cc);
      }
      break;
    }
  }
}


static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  ConsControl cc;
  luaK_code(fs, 0);  /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  checknext(ls, '{');
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    if (ls->t.token == '}') break;
    closelistfield(fs, &cc);
    field(ls, &cc);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  lastlistfield(fs, &cc);
  luaK_settablesize(fs, pc, t->u.info, cc.na, cc.nh);
}

/* }====================================================================== */


static void setvararg (FuncState *fs, int nparams) {
  fs->f->is_vararg = 1;
  luaK_codeABC(fs, OP_VARARGPREP, nparams, 0, 0);
}


static void parlist (LexState *ls) {
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  int isvararg = 0;
  if (ls->t.token != ')' && ls->t.token != '|') {  /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: {
          new_localvar(ls, str_checkname(ls, true));
          nparams++;
          break;
        }
        case TK_DOTS: {
          luaX_next(ls);
          isvararg = 1;
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!isvararg && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  if (isvararg)
    setvararg(fs, f->numparams);  /* declared vararg */
  luaK_reserveregs(fs, fs->nactvar);  /* reserve registers for parameters */
}


static void body (LexState *ls, expdesc *e, int ismethod, int line, lu_byte *prop) {
  /* body ->  '(' parlist ')' block END */
  FuncState new_fs;
  BlockCnt bl;
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  checknext(ls, ')');
  lu_byte rethint = gettypehint(ls);
  lu_byte p = 0xFF;
  statlist(ls, &p);
  if (rethint != 0xFF && /* has type hint for return type? */
      p != 0xFF) { /* return type is known? */
    std::string err = "function was hinted to return ";
    err.append(vk_toTypeString(rethint));
    err.append(" but actually returns ");
    err.append(vk_toTypeString(p));
    throw_warn(ls, err.c_str(), line);
  }
  if (prop) *prop = p; /* propagate return type */
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  codeclosure(ls, e);
  close_func(ls);
}


/*
** Lambda implementation.
** Shorthands lambda expressions into `function (...) return ... end`.
** The '|' token was chosen because it's not commonly used as an unary operator in programming.
** The '->' arrow syntax looked more visually appealing than a colon. It also plays along with common lambda tokens.
*/
static void lambdabody (LexState *ls, expdesc *e, int line) {
  FuncState new_fs;
  BlockCnt bl;
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  checknext(ls, '|');
  parlist(ls);
  checknext(ls, '|');
  checknext(ls, '-');
  checknext(ls, '>');
  expr(ls, e);
  luaK_ret(&new_fs, luaK_exp2anyreg(&new_fs, e), 1);
  new_fs.f->lastlinedefined = ls->linenumber;
  codeclosure(ls, e);
  close_func(ls);
}


static int explist (LexState *ls, expdesc *v, lu_byte *prop = nullptr) {
  /* explist -> expr { ',' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v, prop);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


static void funcargs (LexState *ls, expdesc *f, int line) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist(ls, &args);
        if (hasmultret(args.k))
          luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': {  /* funcargs -> constructor */
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(&args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    nparams = fs->freereg - (base+1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  luaK_fixline(fs, line);
  fs->freereg = base+1;  /* call remove function and arguments and leaves
                            (unless changed) one result */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


/*
** Safe navigation is entirely accredited to SvenOlsen.
** http://lua-users.org/wiki/SvenOlsen
*/
static void safe_navigation(LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  luaX_next(ls);
  luaK_exp2nextreg(fs, v);
  luaK_codeABC(fs, OP_TEST, v->u.info, NO_REG, 0 );
  {
    int old_free = fs->freereg;             
    int vreg = v->u.info;
    int j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
    expdesc key;
    switch(ls->t.token) {
      case '[': {
        luaX_next(ls);  /* skip the '[' */
        if (ls->t.token == '-') {
          expr(ls, &key);
          switch (key.k) {
            case VKINT: {
              key.u.ival *= -1;
              break;
            }
            case VKFLT: {
              key.u.nval *= -1;
              break;
            }
            default: {
              throwerr(ls, "unexpected symbol during navigation.", "unary '-' on non-numeral type.");
            }
          }
        }
        else expr(ls, &key);
        checknext(ls, ']');
        luaK_indexed(fs, v, &key);
        break; 
      }       
      case '.': {
        luaX_next(ls);
        codename(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      default: {
        luaX_syntaxerror(ls, "unexpected symbol");
      }
    }
    luaK_exp2nextreg(fs, v);
    fs->freereg = old_free;
    if (v->u.info != vreg) {
      luaK_codeABC(fs, OP_MOVE, vreg, v->u.info, 0);
      v->u.info = vreg;
    }
    SETARG_sBx(fs->f->code[j], fs->pc-j - 1);
  }
}


static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, v);
      check_match(ls, ')', '(', line);
      luaK_dischargevars(ls->fs, v);
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    case '}':
    case '{': { // Unfinished table constructors.
       if (ls->t.token == '{') {
         throwerr(ls, "unfinished table constructor", "did you mean to close with '}'?");
       }
       else {
         throwerr(ls, "unfinished table constructor", "did you mean to enter with '{'?");
       }
       return;
    }
    case '|': { // Potentially mistyped lambda expression. People may confuse '->' with '=>'.
      while (testnext(ls, '|') || testnext(ls, TK_NAME) || testnext(ls, ','));
      throwerr(ls, "unexpected symbol", "impromper or stranded lambda expression.");
      return;
    }
    default: {
      const char *token = luaX_token2str(ls, ls->t.token);
      throwerr(ls, luaO_fmt(ls->L, "unexpected symbol near %s", token), "unexpected symbol.");
    }
  }
}


static void suffixedexp (LexState *ls, expdesc *v, lu_byte *prop = nullptr) {
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  primaryexp(ls, v);
  for (;;) {
    switch (ls->t.token) {
      case '?': {  /* safe navigation */
        safe_navigation(ls, v);
        break;
      }
      case '.': {  /* fieldsel */
        fieldsel(ls, v);
        break;
      }
      case '[': {  /* '[' exp ']' */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        yindex(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      case ':': {  /* ':' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        codename(ls, &key);
        luaK_self(fs, v, &key);
        funcargs(ls, v, line);
        break;
      }
      case '(': case TK_STRING: case '{': {  /* funcargs */
        if (prop != nullptr && v->k == VLOCAL)
          *prop = getlocalvardesc(ls->fs, v->u.var.vidx)->vd.typeprop;
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v, line);
        break;
      }
      default: return;
    }
  }
}


int cond (LexState *ls);
static void ifexpr (LexState *ls, expdesc *v) {
  /*
  ** Patch published by Ryota Hirose.
  */
  FuncState *fs = ls->fs;
  int condition;
  int escape = NO_JUMP;
  int reg;
  luaX_next(ls);			
  condition = cond(ls);
  checknext(ls, TK_THEN);
  expr(ls, v);					
  reg = luaK_exp2anyreg(fs, v);			
  luaK_concat(fs, &escape, luaK_jump(fs));
  luaK_patchtohere(fs, condition);
  checknext(ls, TK_ELSE);
  expr(ls, v);
  luaK_exp2reg(fs, v, reg);
  luaK_patchtohere(fs, escape);
}


static void simpleexp (LexState *ls, expdesc *v, bool caseexpr, lu_byte *prop = nullptr) {
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  switch (ls->t.token) {
    case TK_FLT: {
      if (prop) *prop = VKINT;
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      if (prop) *prop = VKINT;
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      if (prop) *prop = VKSTR;
      codestring(v, ls->t.seminfo.ts);
      break;
    }
    case TK_NIL: {
      if (prop) *prop = VNIL;
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      if (prop) *prop = VTRUE;
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      if (prop) *prop = VTRUE;
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, fs->f->is_vararg,
                      "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 0, 1));
      break;
    }
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      body(ls, v, 0, ls->linenumber, prop);
      return;
    }
    case '|': {
      lambdabody(ls, v, ls->linenumber);
      return;
    }
    default: {
      suffixedexp(ls, v, prop);
      return;
    }
  }
  luaX_next(ls);
  if (!caseexpr && testnext(ls, ':')) {
    expdesc key;
    codename(ls, &key);
    luaK_self(ls->fs, v, &key);
    funcargs(ls, v, ls->linenumber);
  }
}


static void inexpr (LexState *ls, expdesc *v) {
  expdesc v2;
  checknext(ls, TK_IN);
  expr(ls, &v2);
  luaK_exp2nextreg(ls->fs, v);
  luaK_exp2nextreg(ls->fs, &v2);
  luaK_codeABC(ls->fs, OP_IN, v->u.info, v2.u.info, 0);
  luaK_storevar(ls->fs, v, v);
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '~': return OPR_BNOT;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case '&': return OPR_BAND;
    case '|': return OPR_BOR;
    case '~': return OPR_BXOR;
    case TK_SHL: return OPR_SHL;
    case TK_SHR: return OPR_SHR;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    case TK_COAL: return OPR_COAL;
    case TK_POW: return OPR_POW;  /* '**' operator support */
    default: return OPR_NOBINOPR;
  }
}

/*
** Priority table for binary operators.
*/
static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}, {1, 1}    /* and, or, ?? */
};

#define UNARY_PRIORITY	12  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static BinOpr subexpr (LexState *ls, expdesc *v, int limit, lu_byte *prop = nullptr) {
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR) {  /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls->fs, uop, v, line);
  }
  else if (ls->t.token == TK_IF) ifexpr(ls, v);
  else if (ls->t.token == '+') {
    /* support pseudo-unary '+' by implying '0 + subexpr' */
    init_exp(v, VKINT, 0);
    v->u.ival = 0;
    luaK_infix(ls->fs, OPR_ADD, v);

    expdesc v2;
    int line = ls->linenumber;
    luaX_next(ls); /* skip '+' */
    subexpr(ls, &v2, priority[OPR_ADD].right);
    luaK_posfix(ls->fs, OPR_ADD, v, &v2, line);
  }
  else {
    simpleexp(ls, v, false, prop);
    if (ls->t.token == TK_IN) {
      inexpr(ls, v);
      if (prop) *prop = VTRUE;
    }
  }
  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, priority[op].right);
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}


static void expr (LexState *ls, expdesc *v, lu_byte *prop) {
  subexpr(ls, v, 0, prop);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static void block (LexState *ls) {
  /* block -> statlist */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  statlist(ls);
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev, *next; /* previous & next lhs objects */
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  int extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    if (vkisindexed(lh->v.k)) {  /* assignment to table field? */
      if (lh->v.k == VINDEXUP) {  /* is table an upvalue? */
        if (v->k == VUPVAL && lh->v.u.ind.t == v->u.info) {
          conflict = 1;  /* table is the upvalue being assigned now */
          lh->v.k = VINDEXSTR;
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
      }
      else {  /* table is a register */
        if (v->k == VLOCAL && lh->v.u.ind.t == v->u.var.ridx) {
          conflict = 1;  /* table is the local being assigned now */
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.u.ind.idx == v->u.var.ridx) {
          conflict = 1;
          lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      luaK_codeABC(fs, OP_MOVE, extra, v->u.var.ridx, 0);
    else
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->u.info, 0);
    luaK_reserveregs(fs, 1);
  }
}

/*
  gets the supported binary compound operation (if any)
  gives OPR_NOBINOPR if the operation does not have compound support.
  returns a status (0 false, 1 true) and takes a pointer to set.
  this allows for seamless conditional implementation, avoiding a getcompoundop call for every Lua assignment.
*/
static int getcompoundop (LexState *ls, BinOpr *op) {
  switch (ls->lasttoken) {
    case TK_CCAT: {
      *op = OPR_CONCAT;
      return 1;       /* concatenation */
    }
    case TK_CADD: {
      *op = OPR_ADD;  /* addition */
      return 1;
    }
    case TK_CSUB: {
      *op = OPR_SUB;  /* subtraction */
      return 1;
    }
    case TK_CMUL: {
      *op = OPR_MUL;  /* multiplication */
      return 1;
    }
    case TK_CMOD: {
      *op = OPR_MOD;  /* modulo */
      return 1;
    }
    case TK_CDIV: {
      *op = OPR_DIV;  /* float division */
      return 1;
    }
    case TK_CPOW: {
      *op = OPR_POW;  /* power */
      return 1;
    }
    case TK_CIDIV: {
      *op = OPR_IDIV;  /* integer division */
      return 1;
    }
    case TK_CBOR: {
      *op = OPR_BOR;  /* bitwise OR */
      return 1;
    }
    case TK_CBAND: {
      *op = OPR_BAND;  /* bitwise AND */
      return 1;
    }
    case TK_CBXOR: {
      *op = OPR_BXOR;  /* bitwise XOR */
      return 1;
    }
    case TK_CSHL: {
      *op = OPR_SHL;  /* shift left */
      return 1; 
    }
    case TK_CSHR: {
      *op = OPR_SHR;  /* shift right */
      return 1;
    }
    case TK_COAL: {
      *op = OPR_COAL;
      return 1;
    }
    default: {
      *op = OPR_NOBINOPR;
      return 0;
    }
  }
}

/* 
  compound assignment function
  determines the binary operation to perform depending on lexer state tokens (ls->lasttoken)
  resets the lexer state token
  reserves N registers (where N = local variables on stack)
  preforms binary operation and assignment
*/ 
static void compoundassign(LexState *ls, expdesc* v, BinOpr op) {
  luaX_next(ls);
  int line = ls->linenumber;
  FuncState *fs = ls->fs;
  expdesc e = *v, v2;
  if (v->k != VLOCAL) {  /* complex lvalue, use a temporary register. linear perf incr. with complexity of lvalue */
    luaK_reserveregs(fs, fs->freereg-fs->nactvar);
    enterlevel(ls);
    luaK_infix(fs, op, &e);
    expr(ls, &v2);
    luaK_posfix(fs, op, &e, &v2, line);
    leavelevel(ls);
    luaK_exp2nextreg(fs, &e);
    luaK_setoneret(ls->fs, &e);
    luaK_storevar(ls->fs, v, &e);
  }
  else {  /* simple lvalue; a local. directly change value (~20% speedup vs temporary register) */
    enterlevel(ls);
    luaK_infix(fs, op, &e);
    expr(ls, &v2);
    luaK_posfix(fs, op, &e, &v2, line);
    leavelevel(ls);
    luaK_setoneret(ls->fs, &e);
    luaK_storevar(ls->fs, v, &e);
  }
}

/*
  assignment function
  handles every Lua assignment
  special cases for compound operators via lexer state tokens (ls->lasttoken)
*/
static void restassign (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  check_readonly(ls, &lh->v);
  if (testnext(ls, ',')) {  /* restassign -> ',' suffixedexp restassign */
    struct LHS_assign nv;
    nv.prev = lh;
    nv.next = NULL;
    lh->next = &nv;
    suffixedexp(ls, &nv.v);
    if (!vkisindexed(nv.v.k))
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls);  /* control recursion depth */
    restassign(ls, &nv, nvars+1);
    leavelevel(ls);
  }
  else {  /* restassign -> '=' explist */
    BinOpr op;  /* binary operation from lexer state */
    int token = ls->lasttoken; /* lexer state token */
    if (token != 0 && getcompoundop(ls, &op) != 0) {  /* is there a saved binop? */
      check_condition(ls, nvars == 1, "unsupported tuple assignment");
      compoundassign(ls, &lh->v, op);  /* perform binop & assignment */
      ls->lasttoken = 0;  /* clear last token from lexer state */
      return;  /* avoid default */
    }
    else if (testnext(ls, '=')) { /* no requested binop, continue */
      lu_byte prop;
      int nexps = explist(ls, &e, &prop);
      if (nexps != nvars)
        adjust_assign(ls, nvars, nexps, &e);
      else {
        luaK_setoneret(ls->fs, &e);  /* close last expression */
        if (lh->v.k == VLOCAL) { /* assigning to a local variable? */
          process_assign(ls, getlocalvardesc(ls->fs, lh->v.u.var.vidx), prop);
        }
        luaK_storevar(ls->fs, &lh->v, &e);
        return;  /* avoid default */
      }
    }
  }
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  luaK_storevar(ls->fs, &lh->v, &e);
}

int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


static void gotostat (LexState *ls) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  TString *name = str_checkname(ls);  /* label's name */
  Labeldesc *lb = findlabel(ls, name);
  if (lb == NULL)  /* no label? */
    /* forward jump; will be resolved when the label is declared */
    newgotoentry(ls, name, line, luaK_jump(fs));
  else {  /* found a label */
    /* backward jump; will be resolved here */
    int lblevel = reglevel(fs, lb->nactvar);  /* label level */
    if (luaY_nvarstack(fs) > lblevel)  /* leaving the scope of a variable? */
      luaK_codeABC(fs, OP_CLOSE, lblevel, 0, 0);
    /* create jump and link it to the label */
    luaK_patchlist(fs, luaK_jump(fs), lb->pc);
  }
}


/*
** Break statement. Semantically equivalent to "goto break".
*/
static void breakstat (LexState *ls) {
  int line = ls->linenumber;
  luaX_next(ls);  /* skip break */
  newgotoentry(ls, luaS_newliteral(ls->L, "break"), line, luaK_jump(ls->fs));
}


/*
** Continue statement. Semantically similar to "goto continue".
** Unlike break, this doesn't use labels. It tracks where to jump via BlockCnt.scopeend;
*/
static void continuestat (LexState *ls) {
  FuncState *fs = ls->fs;
  BlockCnt *bl = fs->bl;
  int upval = 0;
  luaX_next(ls); /* skip TK_CONTINUE */
  lua_Integer backwards = 1;
  if (ls->t.token == TK_INT) {
    backwards = ls->t.seminfo.i;
    luaX_next(ls);
  }
  while (bl) {
    if (!bl->isloop) { /* not a loop, continue search */
      upval |= bl->upval; /* amend upvalues for closing. */
      bl = bl->previous; /* jump back current blocks to find the loop */
    }
    else { /* found a loop */
      if (--backwards == 0) { /* this is our loop */
        break;
      }
      else { /* continue search */
        upval |= bl->upval;
        bl = bl->previous;
      }
    };
  }
  if (bl) {
    if (upval) luaK_codeABC(fs, OP_CLOSE, bl->nactvar, 0, 0); /* close upvalues */
    luaK_concat(fs, &bl->scopeend, luaK_jump(fs));
  }
#ifndef PLUTO_COMPATIBLE_CONTINUE
  else error_expected(ls, TK_CONTINUE);
#else
  else error_expected(ls, TK_PCONTINUE);
#endif
}


// Test the next token to see if it's either 'token1' or 'token2'.
inline bool testnext2 (LexState *ls, int token1, int token2) {
  return testnext(ls, token1) || testnext(ls, token2);
}


static const char* expandexpr (LexState *ls) {
  switch (ls->t.token) {
    case '{': {
      return "{}";
    }
    case TK_FUNCTION: {
      return "function (";
    }
    default: {
      return getstr(ls->t.seminfo.ts);
    }
  }
}


static void switchstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  BlockCnt sbl, cbl; // Switch & case blocks.
  expdesc crtl, save, test, lcase;
  int switchToken = gett(ls);
  luaX_next(ls); // Skip switch statement.
  testnext(ls, '(');
  expr(ls, &crtl);
  luaK_exp2nextreg(ls->fs, &crtl);
  init_exp(&save, VLOCAL, crtl.u.info);
  testnext(ls, ')');
  checknext(ls, TK_DO);
  new_localvarliteral(ls, "(switch)"); // Save control value into a local.
  adjustlocalvars(ls, 1);
  enterblock(fs, &sbl, 1);
  do {
    const int caseline = ls->linenumber; // Needed for errors.
#ifdef PLUTO_COMPATIBLE_CASE
    if (!testnext(ls, TK_PCASE)) {
#else
    if (!testnext2(ls, TK_PCASE, TK_CASE)) {
#endif
#ifdef PLUTO_COMPATIBLE_CASE
      error_expected(ls, TK_PCASE);
#else
      error_expected(ls, TK_CASE);
#endif
    }
    if (testnext(ls, '-')) { // Probably a negative constant.
      simpleexp(ls, &lcase, true);
      switch (lcase.k) {
        case VKINT: {
          lcase.u.ival *= -1;
          break;
        }
        case VKFLT: {
          lcase.u.nval *= -1;
          break;
        }
        default: { // Why is there a unary '-' on a non-numeral type?
          throwerr(ls, "unexpected symbol in 'case' expression.", "unary '-' on non-numeral type.");
        }
      }
    }
    else {
      const auto expr = expandexpr(ls); // Raw text of the expression before the lexer skips tokens.
      testnext(ls, '+'); /* support pseudo-unary '+' */
      simpleexp(ls, &lcase, true);
      if (!vkisconst(lcase.k)) {
        ls->linebuff.clear();
        ls->linebuff += "case ";
        ls->linebuff += expr;
        ls->linenumber = caseline;
        throwerr(ls, "malformed 'case' expression.", "expression must be compile-time constant.");
      }
    }
    checknext(ls, ':');
    enterblock(fs, &cbl, 0);
    test = save;
    luaK_infix(fs, OPR_NE, &test);
    luaK_posfix(fs, OPR_NE, &test, &lcase, line);
    caselist(ls, false);
    leaveblock(fs);
    if (gett(ls) == TK_PCASE
#ifndef PLUTO_COMPATIBLE_CASE
        || gett(ls) == TK_CASE
#endif
        ) {
      luaK_code(fs, CREATE_sJ(OP_JMP, (2 + OFFSET_sJ), false)); // Fall-through.
    }
    luaK_patchtohere(fs, test.u.info); // Jump statements if OP_NE, otherwise continue.
  } while (gett(ls) != TK_END && (gett(ls) != TK_PDEFAULT
#ifndef PLUTO_COMPATIBLE_DEFAULT
      && gett(ls) != TK_DEFAULT
#endif
      ));
#ifdef PLUTO_COMPATIBLE_DEFAULT
  if (testnext(ls, TK_PDEFAULT)) { // Default case.
#else
  if (testnext2(ls, TK_PDEFAULT, TK_DEFAULT)) { // Default case.
#endif
    checknext(ls, ':');
    enterblock(fs, &cbl, 0);
    caselist(ls, true);
    leaveblock(fs);
  }
  check_match(ls, TK_END, switchToken, line);
  leaveblock(fs);
}


/*
** Check whether there is already a label with the given 'name'.
*/
static void checkrepeated (LexState *ls, TString *name) {
  Labeldesc *lb = findlabel(ls, name);
  if (l_unlikely(lb != NULL)) {  /* already defined? */
    const char *msg = "label '%s' already defined on line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(name), lb->line);
    luaK_semerror(ls, msg);  /* error */
  }
}


static void labelstat (LexState *ls, TString *name, int line) {
  /* label -> '::' NAME '::' */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    statement(ls);  /* skip other no-op statements */
  checkrepeated(ls, name);  /* check for repeated labels */
  createlabel(ls, name, line, block_follow(ls, 0));
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  whileinit = luaK_getlabel(fs);
  condexit = cond(ls);
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);
  luaK_jumpto(fs, whileinit);
  luaK_patchlist(fs, bl.scopeend, whileinit);
  check_match(ls, TK_END, TK_WHILE, line);
  leaveblock(fs);
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat (LexState *ls) {
  /* repeatstat -> REPEAT block ( UNTIL | WHEN ) cond */
  int condexit;
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  enterblock(fs, &bl2, 0);  /* scope block */
  luaX_next(ls);  /* skip REPEAT */
  statlist(ls);
  luaK_patchtohere(fs, bl1.scopeend);
  if (testnext(ls, TK_UNTIL)) {
    condexit = cond(ls);  /* read condition (inside scope block) */
#ifdef PLUTO_COMPATIBLE_WHEN
  } else if (testnext(ls, TK_PWHEN)) {
#else
  } else if (testnext2(ls, TK_PWHEN, TK_WHEN)) {
#endif
    expdesc v;
    expr(ls, &v);  /* read condition */
    if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
    luaK_goiffalse(ls->fs, &v);
    condexit = v.t;
  }
  else {
    error_expected(ls, TK_UNTIL);
  }
  leaveblock(fs);  /* finish scope */
  if (bl2.upval) {  /* upvalues? */
    int exit = luaK_jump(fs);  /* normal exit must jump over fix */
    luaK_patchtohere(fs, condexit);  /* repetition must close upvalues */
    luaK_codeABC(fs, OP_CLOSE, reglevel(fs, bl2.nactvar), 0, 0);
    condexit = luaK_jump(fs);  /* repeat after closing upvalues */
    luaK_patchtohere(fs, exit);  /* normal exit comes to here */
  }
  luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
  leaveblock(fs);  /* finish loop */
}


/*
** Read an expression and generate code to put its results in next
** stack slot.
**
*/
static void exp1 (LexState *ls) {
  expdesc e;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  lua_assert(e.k == VNONRELOC);
}


/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump (FuncState *fs, int pc, int dest, int back) {
  Instruction *jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_Bx(*jmp, offset);
}


/*
** Generate code for a 'for' loop.
*/
static void forbody (LexState *ls, int base, int line, int nvars, int isgen) {
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  checknext(ls, TK_DO);
  prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs);  /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  luaK_patchtohere(fs, bl.previous->scopeend);
  if (isgen) {  /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
}


static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp,exp[,exp] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvar(ls, varname);
  checknext(ls, '=');
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  exp1(ls);  /* limit */
  if (testnext(ls, ','))
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
    luaK_int(fs, fs->freereg, 1);
    luaK_reserveregs(fs, 1);
  }
  adjustlocalvars(ls, 3);  /* control variables */
  forbody(ls, base, line, 1, 0);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 5;  /* gen, state, control, toclose, 'indexname' */
  int line;
  int base = fs->freereg;
  /* create control variables */
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  /* create declared variables */
  new_localvar(ls, indexname);
  while (testnext(ls, ',')) {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 4, explist(ls, &e), &e);
  adjustlocalvars(ls, 4);  /* control variables */
  marktobeclosed(fs);  /* last control var. must be closed */
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  forbody(ls, base, line, nvars - 4, 1);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  luaX_next(ls);  /* skip 'for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case '=': {
      fornum(ls, varname, line);
      break;
    }
    case ',': case TK_IN: {
      forlist(ls, varname);
      break;
    }
    default: {
      luaX_syntaxerror(ls, "'=' or 'in' expected");
    }
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}


static void test_then_block (LexState *ls, int *escapelist, lu_byte *prop) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  expdesc v;
  int jf;  /* instruction to skip 'then' code (if condition is false) */
  luaX_next(ls);  /* skip IF or ELSEIF */
  expr(ls, &v);  /* read condition */
  checknext(ls, TK_THEN);
  if (ls->t.token == TK_BREAK) {  /* 'if x then break' ? */
    int line = ls->linenumber;
    luaK_goiffalse(ls->fs, &v);  /* will jump if condition is true */
    luaX_next(ls);  /* skip 'break' */
    enterblock(fs, &bl, 0);  /* must enter block before 'goto' */
    newgotoentry(ls, luaS_newliteral(ls->L, "break"), line, v.t);
    while (testnext(ls, ';')) {}  /* skip semicolons */
    if (block_follow(ls, 0)) {  /* jump is the entire block? */
      leaveblock(fs);
      return;  /* and that is it */
    }
    else  /* must skip over 'then' part if condition is false */
      jf = luaK_jump(fs);
  }
  else {  /* regular case (not a break) */
    luaK_goiftrue(ls->fs, &v);  /* skip over block if condition is false */
    enterblock(fs, &bl, 0);
    jf = v.f;
  }
  statlist(ls, prop);  /* 'then' part */
  leaveblock(fs);
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
    luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
  luaK_patchtohere(fs, jf);
}


static void ifstat (LexState *ls, int line, lu_byte *prop = nullptr) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  int escapelist = NO_JUMP;  /* exit list for finished parts */
  test_then_block(ls, &escapelist, prop);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist, prop);  /* ELSEIF cond THEN block */
  if (testnext(ls, TK_ELSE))
    block(ls);  /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* function's variable index */
  new_localvar(ls, str_checkname(ls, true));  /* new local variable */
  adjustlocalvars(ls, 1);  /* enter its scope */
  body(ls, &b, 0, ls->linenumber, &getlocalvardesc(fs, fvar)->vd.typeprop);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}


static int getlocalattribute (LexState *ls) {
  /* ATTRIB -> ['<' Name '>'] */
  if (testnext(ls, '<')) {
    const char *attr = getstr(str_checkname(ls));
    checknext(ls, '>');
    if (strcmp(attr, "const") == 0)
      return RDKCONST;  /* read-only variable */
    else if (strcmp(attr, "close") == 0)
      return RDKTOCLOSE;  /* to-be-closed variable */
    else
      luaK_semerror(ls,
        luaO_pushfstring(ls->L, "unknown attribute '%s'", attr));
  }
  return VDKREG;  /* regular variable */
}


static void checktoclose (FuncState *fs, int level) {
  if (level != -1) {  /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}


static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME ATTRIB { ',' NAME ATTRIB } ['=' explist] */
  FuncState *fs = ls->fs;
  int toclose = -1;  /* index of to-be-closed variable (if any) */
  Vardesc *var;  /* last variable */
  int vidx, kind;  /* index and kind of last variable */
  lu_byte typehint;
  int nvars = 0;
  int nexps;
  expdesc e;
  do {
    vidx = new_localvar(ls, str_checkname(ls, true));
    typehint = gettypehint(ls);
    kind = getlocalattribute(ls);
    var = getlocalvardesc(fs, vidx);
    var->vd.kind = kind;
    var->vd.typehint = typehint;
    if (kind == RDKTOCLOSE) {  /* to-be-closed? */
      if (toclose != -1)  /* one already present? */
        luaK_semerror(ls, "multiple to-be-closed variables in local list");
      toclose = fs->nactvar + nvars;
    }
    nvars++;
  } while (testnext(ls, ','));
  lu_byte prop = 0xFF;
  if (testnext(ls, '='))
    nexps = explist(ls, &e, &prop);
  else {
    e.k = VVOID;
    nexps = 0;
  }
  if (nvars == nexps &&  /* no adjustments? */
      var->vd.kind == RDKCONST &&  /* last variable is const? */
      luaK_exp2const(fs, &e, &var->k)) {  /* compile-time constant? */
    var->vd.kind = RDKCTC;  /* variable is a compile-time constant */
    adjustlocalvars(ls, nvars - 1);  /* exclude last variable */
    fs->nactvar++;  /* but count it */
  }
  else {
    if (nexps == 1) {
      process_assign(ls, var, prop);
    }
    adjust_assign(ls, nvars, nexps, &e);
    adjustlocalvars(ls, nvars);
  }
  checktoclose(fs, toclose);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  if (ls->t.token == ':') {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  ismethod = funcname(ls, &v);
  body(ls, &b, ismethod, line);
  check_readonly(ls, &v);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}


static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    v.prev = NULL;
    restassign(ls, &v, 1);
  }
  else {  /* stat -> func */
    Instruction *inst;
    check_condition(ls, v.v.k == VCALL, "syntax error");
    inst = &getinstruction(fs, &v.v);
    SETARG_C(*inst, 1);  /* call statement uses no results */
  }
}


static void retstat (LexState *ls, lu_byte *prop) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  expdesc e;
  int nret;  /* number of values being returned */
  int first = luaY_nvarstack(fs);  /* first slot to be returned */
  if (block_follow(ls, 1) || ls->t.token == ';')
    nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e, prop);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == luaY_nvarstack(fs));
      }
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);  /* can use original slot */
      else {  /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &e);
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';');  /* skip optional semicolon */
}


static void statement (LexState *ls, lu_byte *prop) {
  int line = ls->linenumber;  /* may be needed for error messages */
  enterlevel(ls);
  switch (ls->t.token) {
    case ';': {  /* stat -> ';' (empty statement) */
      luaX_next(ls);  /* skip ';' */
      break;
    }
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line, prop);
      break;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      break;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      break;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      break;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls);
      break;
    }
    case TK_FUNCTION: {  /* stat -> funcstat */
      funcstat(ls, line);
      break;
    }
    case TK_LOCAL: {  /* stat -> localstat */
      luaX_next(ls);  /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls);
      else
        localstat(ls);
      break;
    }
    case TK_DBCOLON: {  /* stat -> label */
      luaX_next(ls);  /* skip double colon */
      labelstat(ls, str_checkname(ls), line);
      break;
    }
    case TK_RETURN: {  /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      retstat(ls, prop);
      break;
    }
    case TK_BREAK: {  /* stat -> breakstat */
      breakstat(ls);
      break;
    }
#ifndef PLUTO_COMPATIBLE_CONTINUE
    case TK_CONTINUE:
#endif
    case TK_PCONTINUE: {
      continuestat(ls);
      break;
    }
    case TK_GOTO: {  /* stat -> 'goto' NAME */
      luaX_next(ls);  /* skip 'goto' */
      gotostat(ls);
      break;
    }
#ifndef PLUTO_COMPATIBLE_CASE
    case TK_CASE:
#endif
    case TK_PCASE: {
      throwerr(ls, "inappropriate 'case' statement.", "outside of 'switch' block.");
    }
#ifndef PLUTO_COMPATIBLE_DEFAULT
    case TK_DEFAULT:
#endif
    case TK_PDEFAULT: {
      throwerr(ls, "inappropriate 'default' statement.", "outside of 'switch' block.");
    }
#ifndef PLUTO_COMPATIBLE_SWITCH
    case TK_SWITCH:
#endif
    case TK_PSWITCH: {
      switchstat(ls, line);
      break;
    }
    default: {  /* stat -> func | assignment */
      exprstat(ls);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  ls->fs->freereg = luaY_nvarstack(ls->fs);  /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc (LexState *ls, FuncState *fs) {
  BlockCnt bl;
  Upvaldesc *env;
  open_func(ls, fs, &bl);
  setvararg(fs, 0);  /* main function is always declared vararg */
  env = allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  luaX_next(ls);  /* read first token */
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}


LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  LClosure *cl = luaF_newLclosure(L, 1);  /* create main closure */
  setclLvalue2s(L, L->top, cl);  /* anchor it (to avoid being collected) */
  luaD_inctop(L);
  lexstate.h = luaH_new(L);  /* create table for scanner */
  sethvalue2s(L, L->top, lexstate.h);  /* anchor it */
  luaD_inctop(L);
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  L->top--;  /* remove scanner's table */
  return cl;  /* closure is on the stack, too */
}

