/* Read-only tables for Lua */

#include <string.h>
#include "lrotable.h"
#include "lua.h"
#include "lauxlib.h"
#include "lstring.h"
#include "lobject.h"
#include "lapi.h"
#include "udl.h"
#include "platform_conf.h"

/* Local defines */
#define LUAR_FINDFUNCTION     0
#define LUAR_FINDVALUE        1

/* Externally defined read-only table array */
extern const luaR_table lua_rotables;

extern char stext[];
extern char etext[];

/* Is the rotable really in ROM? */
static int luaR_is_in_rom( void *p )
{
  return ( stext <= ( char* )p && ( char* )p <= etext );
}

/* Find a global "read only table" in the constant lua_rotable array */
void* luaR_findglobal(const char *name, unsigned len) {
  const luaR_entry *pentry;
  const char *keyname;
  int i = -1;

  if (strlen(name) > LUA_MAX_ROTABLE_NAME)
    return NULL;
  // Look at the dynamically loaded modules first
   while ((i = udl_ltr_find_next_module(i)) != -1 )
    if (!strncmp(udl_get_module_name(i), name, len))
      return udl_ltr_get_rotable(i);
  // Then look at the static list of modules
  for (pentry=lua_rotables.entries; pentry->key.type != LUA_TNIL; pentry ++) {
    if (pentry->key.type == LUA_TSTRING) {
      keyname = pentry->key.id.strkey;
      if (*keyname != '\0' && strlen(keyname) == len && !strncmp(keyname, name, len)) {
        return (void*)(rvalue(&pentry->value));
      }
    }
  }
  return NULL;
}

// Helper: offset a value and return the new value
static TValue* luaR_offsetValue(const TValue *r, unsigned offset) {
  static TValue v;

  if( r == NULL )
    return NULL;
  v = *r;
  if (offset && !ttisnil(r) && !ttisnumber(r) && !ttisboolean(r))
    v.value.p = ( char* )v.value.p + offset;
  return &v;
}

/* Find an entry in a rotable and return it */
static const TValue* luaR_auxfind(const luaR_entry *pentry, const char *strkey, luaR_numkey numkey, unsigned *ppos) {
  const TValue *res = NULL;
  unsigned i = 0;
  u32 offset;
  
  if (pentry == NULL)
    return NULL; 
  offset = luaR_is_in_rom((void*)pentry) ? 0 : udl_get_offset(udl_get_id((u32)pentry));
  while(pentry->key.type != LUA_TNIL) {
    if ((strkey && (pentry->key.type == LUA_TSTRING) && (!strcmp(pentry->key.id.strkey+offset, strkey))) || 
        (!strkey && (pentry->key.type == LUA_TNUMBER) && ((luaR_numkey)pentry->key.id.numkey == numkey))) {
      res = &pentry->value;
      break;
    }
    i ++; pentry ++;
  }
  if (res && ppos)
    *ppos = i;  
  return luaR_offsetValue(res, offset);
}

int luaR_findfunction(lua_State *L, const luaR_table *ptable) {
  const TValue *res = NULL;
  const char *key = luaL_checkstring(L, 2);
    
  res = luaR_auxfind(ptable->entries, key, 0, NULL);  
  if (res && ttislightfunction(res)) {
    luaA_pushobject(L, res);
    return 1;
  }
  else
    return 0;
}

/* Find an entry in a rotable and return its type 
   If "strkey" is not NULL, the function will look for a string key,
   otherwise it will look for a number key */
const TValue* luaR_findentry(void *data, const char *strkey, luaR_numkey numkey, unsigned *ppos) {
  return luaR_auxfind(((const luaR_table*)data)->entries, strkey, numkey, ppos);
}

/* Find the metatable of a given table */
void* luaR_getmeta(void *data) {
#ifdef LUA_META_ROTABLES
  const TValue *res = luaR_auxfind(((const luaR_table*)data)->entries, "__metatable", 0, NULL);
  return res && ttisrotable(res) ? rvalue(res) : NULL;
#else
  return NULL;
#endif
}

static void luaR_next_helper(lua_State *L, const luaR_entry *pentries, int pos, TValue *key, TValue *val) {
  u32 offset = luaR_is_in_rom((void*)pentries) ? 0 : udl_get_offset(udl_get_id((u32)pentries));
  setnilvalue(key);
  setnilvalue(val);
  if (pentries[pos].key.type != LUA_TNIL) {
    /* Found an entry */
    if (pentries[pos].key.type == LUA_TSTRING)
      setsvalue(L, key, luaS_new(L, pentries[pos].key.id.strkey+offset))
    else
      setnvalue(key, (lua_Number)pentries[pos].key.id.numkey)
   setobj2s(L, val, luaR_offsetValue(&pentries[pos].value, offset));
  }
}

/* next (used for iteration) */
void luaR_next(lua_State *L, void *data, TValue *key, TValue *val) {
  const luaR_entry* pentries = ((const luaR_table*)data)->entries;
  char strkey[LUA_MAX_ROTABLE_NAME + 1], *pstrkey = NULL;
  luaR_numkey numkey = 0;
  unsigned keypos;
  
  /* Special case: if key is nil, return the first element of the rotable */
  if (ttisnil(key)) 
    luaR_next_helper(L, pentries, 0, key, val);
  else if (ttisstring(key) || ttisnumber(key)) {
    /* Find the previous key again */  
    if (ttisstring(key)) {
      luaR_getcstr(strkey, rawtsvalue(key), LUA_MAX_ROTABLE_NAME);          
      pstrkey = strkey;
    } else   
      numkey = (luaR_numkey)nvalue(key);
    luaR_findentry(data, pstrkey, numkey, &keypos);
    /* Advance to next key */
    keypos ++;    
    luaR_next_helper(L, pentries, keypos, key, val);
  }
}

/* Convert a Lua string to a C string */
void luaR_getcstr(char *dest, const TString *src, size_t maxsize) {
  if (src->tsv.len+1 > maxsize)
    dest[0] = '\0';
  else {
    memcpy(dest, getstr(src), src->tsv.len);
    dest[src->tsv.len] = '\0';
  } 
}

/* Return 1 if the given pointer is a rotable */
#ifdef LUA_META_ROTABLES
int luaR_isrotable(void *p) {
  Table *ptable = (Table*)p;
  return ( ptable->tt == LUA_TROTABLE ) && ( ptable->marked & ( 1 << LROTABLEBIT ) );
}
#endif

