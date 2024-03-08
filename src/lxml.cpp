#include <cstring> // strcmp

#define LUA_LIB
#include "lualib.h"

#include "vendor/Soup/soup/xml.hpp"

static void pushxmltag (lua_State *L, const soup::XmlTag& tag) {
  lua_newtable(L);
  lua_pushliteral(L, "tag");
  pluto_pushstring(L, tag.name);
  lua_settable(L, -3);
  if (!tag.attributes.empty()) {
    lua_pushliteral(L, "attributes");
    lua_newtable(L);
    for (const auto& attr : tag.attributes) {
      pluto_pushstring(L, attr.first);
      pluto_pushstring(L, attr.second);
      lua_settable(L, -3);
    }
    lua_settable(L, -3);
  }
  if (!tag.children.empty()) {
    lua_pushliteral(L, "children");
    lua_newtable(L);
    lua_Integer i = 1;
    for (const auto& child : tag.children) {
      lua_pushinteger(L, i++);
      if (child->isTag()) {
        pushxmltag(L, child->asTag());
      }
      else /*if (child->isText())*/ {
        pluto_pushstring(L, child->asText().contents);
      }
      lua_settable(L, -3);
    }
    lua_settable(L, -3);
  }

  if (luaL_newmetatable(L, "pluto:xml_full_node")) {
    lua_pushliteral(L, "__index");
    lua_pushcfunction(L, [](lua_State *L) -> int {
      lua_pushliteral(L, "children");
      if (lua_rawget(L, 1) > LUA_TNIL) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
          if (lua_type(L, -1) == LUA_TTABLE) {
            lua_pushliteral(L, "tag");
            lua_rawget(L, -2);
            if (lua_compare(L, 2, -1, LUA_OPEQ)) {
              lua_pop(L, 1);
              return 1;
            }
            lua_pop(L, 1);
          }
          lua_pop(L, 1);
        }
      }
      return 0;
    });
    lua_settable(L, -3);
  }
  lua_setmetatable(L, -2);
}

static int xml_decode (lua_State *L) {
  const soup::XmlMode *mode = &soup::xml::MODE_XML;
  if (lua_gettop(L) >= 2) {
    const char *modename = luaL_checkstring(L, 2);
    if (strcmp(modename, "html") == 0)
      mode = &soup::xml::MODE_HTML;
    else if (strcmp(modename, "lax") == 0)
      mode = &soup::xml::MODE_LAX_XML;
    else if (strcmp(modename, "xml") != 0)
      luaL_error(L, "unknown parser mode '%s'", modename);
  }
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  auto root = soup::xml::parseAndDiscardMetadata(data, data + len, *mode);
  pushxmltag(L, *root);
  return 1;
}

static const luaL_Reg funcs[] = {
  {"decode", xml_decode},
  {nullptr, nullptr}
};

PLUTO_NEWLIB(xml)
