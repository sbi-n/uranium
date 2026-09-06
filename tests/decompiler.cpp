#include "decompiler.h"
#include "ast.h"
#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)
LUAU_FASTFLAG(DebugLuauUserDefinedClassesRuntime)
LUAU_FASTFLAG(LuauIntegerType2)

namespace
{
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }

std::string evaluate(const std::string& source)
{
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    auto L = state.get();
    luaL_openlibs(L);
    unsigned interrupts = 0;
    lua_callbacks(L)->userdata = &interrupts;
    lua_callbacks(L)->interrupt = [](lua_State* L, int gc) {
        if (gc < 0 && ++*static_cast<unsigned*>(lua_callbacks(L)->userdata) > 10000) luaL_error(L, "test execution budget exceeded");
    };
    auto bytecode = Luau::compile(source);
    if (luau_load(L, "=test", bytecode.data(), bytecode.size(), 0) || lua_pcall(L, 0, LUA_MULTRET, 0))
        throw std::runtime_error(lua_tostring(L, -1));
    std::ostringstream out;
    out << std::setprecision(17);
    for (int i = 1; i <= lua_gettop(L); ++i)
    {
        out << lua_type(L, i) << ':';
        if (lua_type(L, i) == LUA_TSTRING)
        {
            size_t length; const char* text = lua_tolstring(L, i, &length);
            out << length << ':' << std::string(text, length);
        }
        else if (lua_type(L, i) == LUA_TNUMBER) out << lua_tonumber(L, i);
        else if (lua_type(L, i) == LUA_TINTEGER) out << lua_tointeger64(L, i, nullptr);
        else if (lua_type(L, i) == LUA_TBOOLEAN) out << lua_toboolean(L, i);
        else check(lua_isnil(L, i), "test should return primitive results");
        out << '|';
    }
    return out.str();
}

struct Case { const char* name; const char* source; };
const Case cases[] = {
    {"empty", "return"},
    {"arithmetic", R"(local function test(a,b)
        return a+b,a-b,a*b,a/b,a//b,a%b,a^b,2-a,2/a,a and b,a or b,not a,-a,#'abc',a..b..'z'
        end return test(7,3))"},
    {"branches", R"(local function test(a,b)
        local x
        if a == b then x=11 elseif a < b then x=22 else x=33 end
        if x == 22 then x += 3 end
        return x
        end return test(1,1),test(1,2),test(2,1))"},
    {"early returns", R"(local function test(a,b)
        if a then if b then return 1 else return 2 end end
        if b then return 3 end return 4
        end return test(true,true),test(true,false),test(false,true),test(false,false))"},
    {"boolean joins", R"(local function test(a,b,c)
        local x = a and b or c
        local y = a == b
        return x,y,a < b,a <= b,a ~= c
        end return test(3,4,5))"},
    {"while", R"(local n=0 local sum=0
        while n<8 do n+=1 sum+=n end return n,sum)"},
    {"while break continue", R"(local n=0 local sum=0
        while n<20 do n+=1 if n%2==0 then continue end if n>9 then break end sum+=n end return n,sum)"},
    {"repeat", R"(local n=9 repeat n-=1 until n<=3 return n)"},
    {"repeat local", R"(local n=0 repeat n+=1 local stop=n>4 until stop return n)"},
    {"infinite loop break", R"(local n=0 while true do n+=1 if n==5 then break end end return n)"},
    {"numeric for", R"(local function test(n,step) local sum=0 for i=1,n,step do sum+=i end return sum end
        return test(8,1),test(8,2),test(-3,-1),test(0,1))"},
    {"numeric continue", R"(local sum=0 for i=1,20 do if i%2==0 then continue end if i>9 then break end sum+=i end return sum)"},
    {"nested loops", R"(local sum=0 for i=1,5 do for j=1,3 do if i==j then continue end sum+=i*j end end return sum)"},
    {"generic loops", R"(local t={2,4,6} local sum=0
        for k,v in ipairs(t) do sum+=k*v end
        for k,v in pairs(t) do sum+=v end
        for k,v in t do sum+=v end return sum)"},
    {"generic continue", R"(local sum=0 for k,v in ipairs({1,2,3,4,5,6}) do if k==2 then continue end if k==5 then break end sum+=v end return sum)"},
    {"tables", R"(local t={a=3,['end']=4,9,8} t[2]=7 t.a+=1 return t.a,t['end'],t[1],t[2])"},
    {"nested tables", R"(local t={a={1,2},b={c='yes'},3,4} return t.a[2],t.b.c,t[1],t[2])"},
    {"varargs", R"(local function test(x,...)
        local a,b,c=... local t={1,2,...} return x,a,b,c,t[1],t[3],t[4],t[5]
        end return test(9,4,nil,6))"},
    {"open calls", R"(local function values(...) return ... end
        local function test(...) return values(7,values(...)) end
        return test(1,nil,3))"},
    {"result adjustment", R"(local function values() return 1,2,3 end
        local a=values() local b,c=values() return a,b,c,(values()))"},
    {"method call", R"(local t={x=10} function t:add(a,...) local b=... return self.x+a+b end return t:add(3,4))"},
    {"mutable capture", R"(local n=1
        local function add(x) n+=x return n end
        local a=add(2) n+=5 local b=add(3) return a,b,n)"},
    {"inherited capture", R"(local n=1
        local function outer(x) return function(y) n+=x+y return n end end
        local f=outer(2) local a=f(3) return a,f(4),n)"},
    {"recursive closure", R"(local function factorial(n) if n<=1 then return 1 end return n*factorial(n-1) end return factorial(6))"},
    {"closed captures", R"(local fs={} for i=1,4 do local x=i fs[i]=function() x+=1 return x end end
        return fs[1](),fs[2](),fs[1](),fs[4]())"},
    {"register reuse", R"(local f do local a=8 f=function() a+=1 return a end end
        local a=100 return f(),a,f())"},
    {"side effect order", R"(local log='' local x=1
        local function a() log..='a' x=9 return 2 end
        local function b() log..='b' return 3 end
        local p=a() local q=b() local r=x return q,p,r,log)"},
    {"mutable read order", R"(local x=1 local function mutate() x=2 return 3 end
        local a=x local b=mutate() return b,a,x)"},
    {"short circuit effects", R"(local n=0 local function hit() n+=1 return n end
        local a=false and hit() local b=true or hit() local c=hit() and hit() return a,b,c,n)"},
    {"binary strings", "return 'a\\000b\\n\\\"\\\\', -32768, 1.25, true, nil"},
    {"repeat continue", R"(local n=0 local sum=0 repeat n+=1 if n%2==0 then continue end sum+=n until n>=7 return n,sum)"},
    {"repeat break", R"(local n=0 repeat n+=1 if n==4 then break end until n==9 return n)"},
    {"repeat compound condition", R"(local n=0 repeat n+=1 until n>3 and n%2==0 return n)"},
    {"while compound condition", R"(local a=0 local b=9 while a<10 and b>0 do a+=1 b-=2 end return a,b)"},
    {"while effects in condition", R"(local n=0 local function hit() n+=1 return n<5 end while hit() do end return n)"},
    {"nested while", R"(local n=0 local sum=0 while n<4 do n+=1 local j=0 while j<4 do j+=1 if j==n then break end sum+=j end end return n,sum)"},
    {"loop early return", R"(local function test(n) for i=1,n do if i==4 then return i end end return -1 end return test(3),test(9))"},
    {"mutated loop index", R"(local sum=0 for i=1,5 do sum+=i i=99 sum+=i end return sum)"},
    {"descending loop", R"(local sum=0 for i=5,1,-2 do sum+=i end return sum)"},
    {"nil loop capture", R"(local fs={} for i=1,3 do local x local function f() return x end x=i fs[i]=f end return fs[1](),fs[2](),fs[3]())"},
    {"mutable phi capture", R"(local x=1 local function f() return x end if unknown then x=9 else x=7 end return f(),x)"},
    {"multiple closures", R"(local x=0 local function inc() x+=1 end local function get() return x end inc() inc() return get())"},
    {"table open call", R"(local function f() return 3,nil,5 end local t={1,2,f()} return t[1],t[2],t[3],t[4],t[5])"},
    {"table value timing", R"(local x=1 local function change() x=9 return 2 end local t={a=x,b=change()} return t.a,t.b,x)"},
    {"metamethod timing", R"(local log='' local mt={__index=function(t,k) log..=k return 3 end}
        local t=setmetatable({},mt) local a=t.a local b=t.b return b,a,log)"},
    {"comparison NaN", R"(local function f(a,b) return not(a<=b),not(a<b),a==b end return f(0/0,3))"},
    {"false if expression", R"(local function f(c) local x=if c then false else 4 return x end return f(true),f(false))"},
    {"global generated names", R"(v_0=7 uv_0=8 f_0=9 local x=v_0 local y=uv_0 local function f() return f_0 end return x,y,f())"},
    {"multiple assignment swap", R"(local a=3 local b=7 local n=0 while n<5 do a,b=b,a n+=1 end return a,b)"},
    {"table capture and open tail", R"(local x=1 local function change() x=9 return 2,nil,4 end local t={a=x,change()} return t.a,t[1],t[2],t[3],x)"},
    {"table evaluation order", R"(local log='' local function f(x) log..=x return x end
        local t={a=f('a')} t.b=f('b') return t.a,t.b,log)"},
    {"zero result call", R"(local x=1 local function f() x+=1 return 4,5 end f() return x)"},
    {"open varargs trailing nil", R"(local function f(...) return ... end return f(1,nil,nil))"},
    {"branch closure", R"(local function make(c) local f if c then local x=3 f=function() return x end else local x=7 f=function() return x end end return f() end return make(true),make(false))"},
    {"branch open calls", R"(local function a() return 1,nil,3 end local function b() return 4,5 end
        local function f(c) if c then return a() else return b() end end return f(true))"},
    {"class", R"(class Point public x public y function magnitude(self) return self.x*self.x+self.y*self.y end end
        local p=Point.new({x=3,y=4}) return p:magnitude(),p.x)"},
    {"class capture", R"(local x=3 class Point public x
        function make() return Point.new({x=x}) end end
        x=7 local p=Point.make() return p.x)"},
    {"class inheritance", R"(open class Animal public age function ageNext(self) return self.age+1 end end
        class Cat extends Animal public breed end
        local c=Cat.new({age=3,breed='cat'}) return c:ageNext(),c.breed)"},
    {"parenthesized statement", R"(local n=0 local function f() n+=1 end local function g() n+=4 end
        f(); (if n==1 then f else g)(); return n)"},
    {"for evaluation order", R"(local log='' local function f(x) log..=x return tonumber(x) end
        local n=0 for i=f('1'),f('3'),f('2') do n+=i end return n,log)"},
    {"condition side effects", R"(local n=0 local function f() n+=1 return n end
        local function g(a) return a and f() or f() end return g(false),g(true),n)"},
    {"custom iterator", R"(local t=setmetatable({2,4,6},{__iter=function(t) return next,t,nil end})
        local sum=0 for k,v in t do sum+=v end return sum)"},
    {"vararg empty table", R"(local function f(...) local t={...} return #t,t[1] end return f())"},
    {"empty comparison", R"(local n=0 local mt={__lt=function(a,b) n+=1 return true end}
        local a=setmetatable({},mt) local b=setmetatable({},mt) if a<b then end return n)"},
    {"long list with open tail", R"(local function f() return 91,nil,93 end
        local t={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
        21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,f()}
        return t[1],t[16],t[17],t[32],t[33],t[50],t[60],t[61],t[62],t[63])"},
    {"integer boundaries", "return 0x8000000000000000i, 9223372036854775807i, -27i, 0i"},
    {"precedence", R"(local function f(a,b,c)
        return a^b^c,(a^b)^c,a-(b-c),a/(b/c),-(a^b),(-a)^b,a..(b..c),not(a==b)
        end return f(8,2,3))"},
};

void roundTrips()
{
    unsigned passed = 0, failed = 0;
    for (const auto& test : cases)
    {
        auto expected = evaluate(test.source);
        for (int optimization = 0; optimization <= 2; ++optimization)
            for (int debug : {0, 2})
            {
                Luau::CompileOptions options; options.optimizationLevel = optimization; options.debugLevel = debug;
                std::string output;
                try
                {
                    auto bytecode = Luau::compile(test.source, options);
                    output = decompile(bytecode);
                    check(decompile(bytecode) == output, "non-deterministic output");
                    auto actual = evaluate(output);
                    check(actual == expected, "results differ: expected " + expected + " actual " + actual);
                    ++passed;
                }
                catch (const std::exception& error)
                {
                    ++failed;
                    std::cerr << test.name << " (opt=" << optimization << ", debug=" << debug << "): " << error.what() << '\n' << output << '\n';
                }
            }
    }
    std::cout << passed << " decompiler round trips passed, " << failed << " failed\n";
    check(!failed, "decompiler round-trip failures");
}

void stagesAndNames()
{
    Luau::CompileOptions options; options.optimizationLevel = 0; options.debugLevel = 0;
    auto bytecode = Luau::compile(R"(local x=1 local function first(a)
        local function second(b) x+=b return a+x end return second end
        local function third() return x end return first,third)", options);
    auto cfg = buildCFG(lift(bytecode));
    auto ssa = buildSSA(std::move(cfg));
    auto ast = buildAST(ssa);
    auto text = printAST(ast);
    check(text.find("local uv_0 = 1") != std::string::npos, "captured binding should use uv_0");
    check(text.find("local function f_0(") < text.find("local function f_1("), "nested function declaration order");
    check(text.find("local function f_1(") < text.find("local function f_2("), "function numbering must follow printed order");
    check(text.find("local function f_0(uv_1)") != std::string::npos, "captured parameter should share its upvalue name");

    auto diamond = buildSSA(buildCFG(lift(Luau::compile("local x if input then x=1 else x=2 end return x", options))));
    bool phi = false;
    for (const auto& block : diamond.functions[0].blocks)
        for (const auto& p : block.phis) { phi = true; check(p.inputs.size() == 2, "phi must have both incoming values"); }
    check(phi, "branch merge must produce SSA phi");

    auto ordered = decompile(Luau::compile(R"(local a=input print(a,a)
        local function outer(p) local b=input print(b,b)
            local function inner(q) return q end return p,inner end
        local c=input print(c,c) return outer)", options));
    size_t previous = 0;
    for (const char* declaration : {"local v_0 = input", "local function f_0(v_1)", "local v_2 = input", "local function f_1(v_3)", "local v_4 = input"})
    {
        auto position = ordered.find(declaration);
        check(position != std::string::npos && position >= previous, "variables must be numbered by printed declaration order");
        previous = position;
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        FFlag::DebugLuauUserDefinedClasses.value = true;
        FFlag::DebugLuauUserDefinedClassesRuntime.value = true;
        FFlag::LuauIntegerType2.value = true;
        if (argc > 1)
        {
            for (const auto& test : cases) if (test.name == std::string_view(argv[1]))
            {
                auto bytecode = Luau::compile(test.source);
                auto cfg = buildCFG(lift(bytecode));
                std::cout << dump(cfg.ir);
                for (size_t f = 0; f < cfg.functions.size(); ++f)
                    for (size_t b = 0; b < cfg.functions[f].blocks.size(); ++b)
                    {
                        auto& block = cfg.functions[f].blocks[b];
                        std::cout << "f" << f << " b" << b << " idom=" << block.immediateDominator << " ipdom=" << block.immediatePostDominator << '\n';
                    }
                std::cout << decompile(bytecode);
            }
            return 0;
        }
        roundTrips(); stagesAndNames();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
