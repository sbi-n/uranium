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

std::string evaluate(const std::string& source, const Luau::CompileOptions& options = {})
{
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    auto L = state.get();
    luaL_openlibs(L);
    unsigned interrupts = 0;
    lua_callbacks(L)->userdata = &interrupts;
    lua_callbacks(L)->interrupt = [](lua_State* L, int gc) {
        if (gc < 0 && ++*static_cast<unsigned*>(lua_callbacks(L)->userdata) > 10000) luaL_error(L, "test execution budget exceeded");
    };
    auto bytecode = Luau::compile(source, options);
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
    {"table callback open tail", R"(local log=''
        local function connect(callback) local value=callback() log..=value return value,nil,5 end
        local t={connect(function() return 'a' end),connect(function() return 'b' end)}
        return t[1],t[2],t[3],t[4],t[5],log)"},
    {"table callback capture timing", R"(local x=1
        local function connect(callback) local value=callback() x+=1 return value,nil,x end
        local t={connect(function() return x end),connect(function() return x end)}
        return t[1],t[2],t[3],t[4],x)"},
    {"table value timing", R"(local x=1 local function change() x=9 return 2 end local t={a=x,b=change()} return t.a,t.b,x)"},
    {"table store operands", R"(local log=''
        local input=setmetatable({}, {__index=function(t,k) log..=k return k=='key' and 'answer' or 7 end})
        local target=setmetatable({}, {__newindex=function(t,k,v) log..='store' rawset(t,k,v) end})
        local key=input.key local value=input.value target[key]=value
        local amount=math.abs(-2) target.amount=amount return target.answer,target.amount,log)"},
    {"table store snapshots", R"(local t={} local original=t local key=1
        local function change() t={} key=2 return 9 end
        t[key]=change() return original[1],original[2],t[1],t[2],key)"},
    {"parameter snapshot before reassignment", R"(local function test(n,c)
        local old=n if c then n=7 end return old,n end
        return test(3,true),test(4,false))"},
    {"call arguments before parameter reassignment", R"(local function test(a,c)
        local b=a.aass local result=select(2,a,a.aass)
        if c then a={} end return result,a.aass end
        return test({aass=7},false),test({aass=9},true))"},
    {"loop argument aliases", R"(local result=0
        for i=1,3 do local copy=i math.abs(i) result+=select(1,copy) end return result)"},
    {"loop alias snapshot before write", R"(local result=0
        for i=1,3 do local copy=i i=99 result+=select(1,copy) end return result)"},
    {"immutable captured argument aliases", R"(local a={aass=7}
        local function reader() return a end
        local copy=a math.abs(-2) return select(2,reader(),copy.aass))"},
    {"argument snapshot across mutation", R"(local a={aass=5}
        local function mutate() a={aass=9} return 2 end
        local function consume(first,second,third) return first.aass,second,third end
        return consume(a,mutate(),a.aass))"},
    {"property before callee lookup", R"(consume=function(a,b) return 'old',b end
        local a=setmetatable({}, {__index=function()
            consume=function(a,b) return 'new',b end return 7 end})
        local b=a.aass return consume(a,b))"},
    {"property after callee lookup", R"(consume=function(a,b) return 'old',b end
        local a=setmetatable({}, {__index=function()
            consume=function(a,b) return 'new',b end return 7 end})
        return consume(a,a.aass))"},
    {"single use in branch and loop", R"(local function test(n,c)
        local old=n local suffix='x' local result=''
        for i=1,3 do result..=suffix end
        if c then return old,result end return 0,result end
        return test(3,true),test(4,false))"},
    {"callee across closure declaration", R"(local call=pcall
        local function callback() return 17 end
        return call(callback))"},
    {"pcall callback arguments and results", R"(local n=1
        local function callback(a,...) n+=a return n,... end
        local ok,a,b,c=pcall(callback,2,'tail',nil) return ok,a,b,c,n)"},
    {"pcall callback error", R"(local ok,message=pcall(function() error('callback failed',0) end)
        return ok,message)"},
    {"spawn nested callbacks", R"(task={spawn=function(callback,...) return callback(...) end}
        local n=3 local function callback(a)
            local ok,value=pcall(function() n+=a return n end) return ok,value
        end return task.spawn(callback,4))"},
    {"callback capture timing", R"(local n=1
        local function callback() return n end
        local function change() n=9 end change() return pcall(callback))"},
    {"callback value snapshot", R"(local function test(n,c)
        local snapshot=n local function callback() return snapshot end
        if c then n=9 end return pcall(callback),n end return test(3,true),test(4,false))"},
    {"shared callback", R"(local n=0 local function callback() n+=1 return n end
        local ok,a=pcall(callback) local ok2,b=pcall(callback) return ok,a,ok2,b,n)"},
    {"recursive callback", R"(local function callback(n)
        if n<=1 then return 1 end return n*callback(n-1) end return pcall(callback,5))"},
    {"callback identity across loop", R"(task={spawn=function(callback)
        if previous then return previous==callback end previous=callback return true end}
        local n=0 local function callback() return n end local same=true
        for i=1,3 do n=i same=task.spawn(callback) and same end return same)"},
    {"nested builtin table store", R"(local log='' local data={}
        local function read(label,value) log..=label return value end
        local t=setmetatable({}, {__newindex=function(_,key,value) log..='store' data[key]=value end})
        t[read('key','value')]=math.clamp(math.round(read('value',12.6)),0,255)
        t.text=tostring(math.round(read('text',0.42)*100))..'%'
        return data.value,data.text,log)"},
    {"builtin argument packs", R"(local function values() return -3,8,2 end
        local function selectTail(...) return select(2,...) end
        return math.max(values()),selectTail(1,nil,3))"},
    {"stored function captures", R"(local value=1 local t={}
        local function callback(n) value+=n return value end
        t.callback=callback value=5 return t.callback(2),t.callback(3),value)"},
    {"stored function snapshot", R"(local function test(value)
        local snapshot=value local t={callback=function() return snapshot end}
        value=9 return t.callback(),value end return test(3))"},
    {"stored shared and recursive functions", R"(local t={}
        local function callback(n) if n<=1 then return 1 end return n*callback(n-1) end
        t.first=callback t.second=callback return t.first==t.second,t.first(5))"},
    {"stored function loop identity", R"(local value=1 local function callback() return value end
        local t={} for i=1,3 do t[i]=callback value=i end
        return t[1]==t[2],t[2]==t[3],t[1]())"},
    {"metamethod timing", R"(local log='' local mt={__index=function(t,k) log..=k return 3 end}
        local t=setmetatable({},mt) local a=t.a local b=t.b return b,a,log)"},
    {"comparison NaN", R"(local function f(a,b) return not(a<=b),not(a<b),a==b end return f(0/0,3))"},
    {"false if expression", R"(local function f(c) local x=if c then false else 4 return x end return f(true),f(false))"},
    {"global generated names", R"(v_0=7 uv_0=8 f_0=9 local x=v_0 local y=uv_0 local function f() return f_0 end return x,y,f())"},
    {"duplicate function debugnames", R"(local function same() return 1 end local first=same
        local function same() return first()+2 end return first(),same())"},
    {"nested function debugnames", R"(local function same(n)
        local function same(x) return n+x end return same(3) end return same(4))"},
    {"generated function debugnames", R"(local function v_0(value) return value+1 end
        local function uv_0() return v_0(3) end local function f_0() return uv_0() end return f_0())"},
    {"environment function debugname", R"(local function getfenv() return 4 end v_0=7 return getfenv(),v_0)"},
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

void branchRecovery()
{
    const Case branches[] = {
        {"short circuit shared arms", R"(
            if a and b and not c then mark('branch_body_marker')
            else mark('alternative_marker') end
            mark('shared_tail_marker'))"},
        {"nested early return continuation", R"(
            if a then
                if b then return end
                mark('branch_body_marker')
            end
            mark('shared_tail_marker'))"},
        {"returning elseif arms", R"(
            if a then
                if b then return end
                mark('branch_body_marker')
            elseif b then
                if c then return end
                mark('alternative_marker')
            else return end
            mark('shared_tail_marker'))"},
        {"mixed short circuit", R"(
            if (a or b) and c then mark('branch_body_marker')
            else mark('alternative_marker') end
            mark('shared_tail_marker'))"},
        {"nested opposite conditions", R"(
            local t = setmetatable({}, {__index=function(_, key)
                mark(key)
                if key == 'left' then return b else return c end
            end})
            if not a or not (t.left or t.right) then mark('branch_body_marker') end
            mark('shared_tail_marker'))"},
        {"shared loop continuation", R"(
            for i=1,4 do
                if a then
                    if b then continue end
                    mark('branch_body_marker')
                end
                if c and i==2 then break end
                mark('shared_tail_marker')
            end)"},
        {"guarded shared closure", R"(
            if a then
                if b then return end
                mark('branch_body_marker')
            end
            local value=c
            local function callback() mark('shared_tail_marker') return value end
            value=b
            mark(tostring(callback())))"},
        {"short circuit side effects", R"(
            local function hit(key, value) mark(key) return value end
            if (hit('a', a) or hit('b', b)) and hit('c', c) then
                mark('branch_body_marker')
            else mark('alternative_marker') end
            mark('shared_tail_marker'))"},
        {"short circuit with inlined branches", R"(
            local function read(value)
                if b then mark('check') end
                if c then mark('second_check') end
                mark('read')
                return value
            end
            if read(a) ~= true or read(b) ~= true then
                mark('branch_body_marker')
            end
            if read(a) and read(c) then
                mark('alternative_marker')
            end
            mark('shared_tail_marker'))"},
        {"condition mutation timing", R"(
            local function hit() a=not a mark('hit') return b end
            if a and hit() then mark('branch_body_marker') end
            mark(tostring(a))
            mark('shared_tail_marker'))"},
        {"shared nil materialization", R"(
            local value = a or (not b and tostring(c or 'default')) or nil
            mark(tostring(value))
            mark('shared_tail_marker'))"},
    };
    for (const auto& test : branches)
    {
        std::string source = "local log='' local function mark(s) log..=s..':' end local function test(a,b,c) ";
        source += test.source;
        std::string definition = source + " end return test";
        source += R"( end
            local values={false,true,0,''}
            for i=1,5 do for j=1,5 do for k=1,5 do
                test(values[i],values[j],values[k]) mark('|')
            end end end return log)";
        auto expected = evaluate(source);
        for (int optimization : {0, 1, 2})
            for (int debug : {0, 2})
            {
                Luau::CompileOptions options; options.optimizationLevel = optimization; options.debugLevel = debug;
                auto output = decompile(Luau::compile(source, options));
                std::string context = std::string(test.name) + " (opt=" + std::to_string(optimization) +
                    ", debug=" + std::to_string(debug) + ")";
                check(evaluate(output) == expected, context + ": branch effects/results changed\n" + output);
                // Returning the function prevents optimization level 2 from
                // also inlining a legitimate second copy into the test driver.
                auto bytecode = Luau::compile(definition, options);
                auto structure = decompile(bytecode);
                auto cfg = buildCFG(lift(bytecode));
                for (const char* marker : {"branch_body_marker", "alternative_marker", "shared_tail_marker"})
                {
                    if (source.find(marker) == std::string::npos) continue;
                    // Nested callbacks can still be inlined by the compiler.
                    // Count its reachable copies, so only decompiler duplication
                    // (or loss) fails this assertion.
                    size_t expectedCopies = 0, actualCopies = 0;
                    for (size_t f = 0; f < cfg.ir.functions.size(); ++f)
                        for (const auto& block : cfg.ir.functions[f].blocks)
                            if (cfg.functions[f].blocks[block.id].reachable)
                                for (const auto& instruction : block.instructions)
                                    for (const auto& operand : instruction.operands)
                                        if (auto text = std::get_if<IRConstantString>(&operand); text && text->value == marker)
                                            ++expectedCopies;
                    for (size_t pos = structure.find(marker); pos != std::string::npos; pos = structure.find(marker, pos + 1))
                        ++actualCopies;
                    check(expectedCopies > 0 && actualCopies == expectedCopies,
                        context + ": missing or duplicated branch body " + marker + "\n" + structure);
                }
            }
    }
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

void argumentAliases()
{
    for (int optimization : {0, 1, 2})
        for (int debug : {0, 2})
        {
            Luau::CompileOptions options; options.optimizationLevel = optimization; options.debugLevel = debug;
            auto simple = decompile(Luau::compile("local a=input local b=a.aass print(a)", options));
            check(simple.find("print(v_0)") != std::string::npos, "plain arguments should reuse the original binding");

            auto parameter = decompile(Luau::compile(R"(local function test(a)
                local b=a.aass print(a) print(a,a.aass)
                if input then a={} end return a end return test)", options));
            check(parameter.find("print(v_0)") != std::string::npos, "later parameter assignments must not force an argument copy:\n" + parameter);
            check(parameter.find("print(v_0, v_0.aass)") != std::string::npos, "property arguments should inline beside their original receiver:\n" + parameter);

            auto loop = decompile(Luau::compile("for a in values do local b=a.aass print(a,a.aass) end", options));
            check(loop.find("print(v_0, v_0.aass)") != std::string::npos, "loop argument copies should disappear:\n" + loop);

            auto captured = decompile(Luau::compile(R"(local a=input local function reader() return a end
                local b=a.aass local copy=a print(copy) return reader)", options));
            check(captured.find("print(uv_0)") != std::string::npos, "read-only captures should not need argument snapshots:\n" + captured);
        }
}

void singleUseCallAndStoreValues()
{
    for (int optimization : {0, 1, 2})
        for (int debug : {0, 2})
        {
            Luau::CompileOptions options; options.optimizationLevel = optimization; options.debugLevel = debug;
            for (const char* source : {
                "print(math.abs(input))",
                "return math.clamp(math.round(input), 0, 255)",
                "target.child.value = math.abs(input)",
                "target[readKey()] = math.clamp(math.round(input), 0, 255)",
                "target:consume(math.abs(input), tonumber(text))",
                "target.Text = tostring(math.round(input * 100)) .. '%'",
                "target.value = function() return 1 end",
                "local function callback() return 1 end target.value = callback",
                "return {callback = function() return 1 end}",
            })
            {
                auto output = decompile(Luau::compile(source, options));
                check(output.find("local ") == std::string::npos,
                    "single-use call and table operands should inline (opt=" + std::to_string(optimization) + "):\n" + output);
            }

            // Unsafe environments exercise the fallback lookup order, including
            // open arguments that are represented as nested calls in the AST.
            for (const char* source : {
                R"(local env=getfenv() local log=''
                    env.math={abs=function(n) log..='old' return n+1 end}
                    local value=setmetatable({}, {__index=function()
                        log..='arg' env.math.abs=function(n) log..='new' return n+10 end return 3 end})
                    local result=math.abs(value.input) return result,log)",
                R"(local env=getfenv() local log=''
                    local function argument()
                        log..='arg' env.tonumber=function(n) log..='new' return n+10 end return 3,nil end
                    local result=tonumber(argument()) return result,log)",
                R"(local env=getfenv() local convert=tonumber
                    local function argument() env.tonumber=function() return 99 end return '7' end
                    return convert(argument()))",
                R"(local env=getfenv()
                    local function argument() env.math={max=function(...) return 99 end} return 3,4 end
                    return math.max(argument()))",
            })
            {
                auto output = decompile(Luau::compile(source, options));
                check(evaluate(output, options) == evaluate(source, options),
                    "inlined calls must preserve fallback lookups and saved callees (opt=" + std::to_string(optimization) + "):\n" + output);
            }
        }
}

void callbackInlining()
{
    const char* source = R"(local value=1
        local function protected(a,...) value+=a return value,... end
        local ok,result=pcall(protected,2,'tail')
        local function scheduled() value+=result end task.spawn(scheduled)
        task.spawn(function() pcall(function() value+=1 end) end)
        return ok,value)";
    for (int optimization : {0, 1, 2})
        for (int debug : {0, 2})
        {
            Luau::CompileOptions options; options.optimizationLevel = optimization; options.debugLevel = debug;
            auto output = decompile(Luau::compile(source, options));
            check(output.find("local function ") == std::string::npos, "single-use callback declarations should disappear:\n" + output);
            check(output.find("pcall(function(") != std::string::npos, "pcall callback should be embedded");
            check(output.find("task.spawn(function()") != std::string::npos, "spawn callback should be embedded");
            const char* task = "task={spawn=function(callback,...) return callback(...) end} ";
            check(evaluate(task + output) == evaluate(std::string(task) + source), "inline callbacks must preserve captures and results");
        }

    Luau::CompileOptions options; options.optimizationLevel = 0; options.debugLevel = 2;
    auto shared = decompile(Luau::compile(R"(local function callback() return 1 end
        pcall(callback) task.spawn(callback))", options));
    check(shared.find("local function callback(") != std::string::npos && shared.find("pcall(callback)") != std::string::npos &&
        shared.find("task.spawn(callback)") != std::string::npos, "shared callbacks must retain one binding");

    auto recursive = decompile(Luau::compile(R"(local function callback(n)
        if n>0 then return callback(n-1) end return 0 end pcall(callback,3))", options));
    check(recursive.find("local function callback(") != std::string::npos && recursive.find("pcall(callback, 3)") != std::string::npos,
        "recursive callbacks must retain their self binding");

    auto loop = decompile(Luau::compile(R"(local n=0 local function callback() return n end
        while pcall(callback) do n+=1 if n>2 then break end end)", options));
    check(loop.find("local function callback(") != std::string::npos, "loop conditions must reuse the original callback");
}

void inliningAndDebugNames()
{
    Luau::CompileOptions options; options.optimizationLevel = 0; options.debugLevel = 2;
    auto output = decompile(Luau::compile(R"(local function fill(t,input)
        local key=input.key local value=input.value t[key]=value
        local amount=math.abs(-2) t.amount=amount
        local call=print local function callback() return 1 end call(callback)
        return t end return fill)", options));
    check(output.find("local v_") == std::string::npos, "single-use operands should not leave temporary declarations:\n" + output);
    check(output.find("v_0[v_1.key] = v_1.value") != std::string::npos, "table keys and values should be substituted");
    check(output.find("v_0.amount = math.abs(-2)") != std::string::npos, "table stores should accept inline calls");
    check(output.find("print(function()") != std::string::npos, "callee and its single-use callback should inline");
    check(output.find("local function fill(") != std::string::npos, "function should retain its debugname");

    auto branch = decompile(Luau::compile(R"(local function choose(value,condition)
        local alias=value if condition then return alias end return 0 end return choose)", options));
    check(branch.find("local v_") == std::string::npos, "immutable single-use aliases should inline into branches");

    auto shared = decompile(Luau::compile(R"(local value=input return value,value)", options));
    check(shared.find("local v_0 = input") != std::string::npos, "multiple reads must retain a shared binding");

    auto recursive = decompile(Luau::compile(R"(local function factorial(n)
        if n<=1 then return 1 end return n*factorial(n-1) end return factorial(6))", options));
    check(recursive.find("local function factorial(") != std::string::npos && recursive.find("factorial(v_0 - 1)") != std::string::npos,
        "recursive calls must use the debugname");

    const char* collisionSource = R"(local function first(n) return math.abs(n) end
        local function second(n) return first(n)+1 end return second(-3))";
    auto ir = lift(Luau::compile(collisionSource, options));
    for (auto& fn : ir.functions) if (fn.id) fn.debugname = "math";
    auto collision = printAST(buildAST(buildSSA(buildCFG(std::move(ir)))));
    check(collision.find("local function math_1(") != std::string::npos && collision.find("local function math_2(") != std::string::npos,
        "debugnames must avoid global and local name collisions");
    check(evaluate(collision) == evaluate(collisionSource), "name collisions must not change global lookup or captured calls");

    for (const char* invalid : {"end", "a.b", "two words", ""})
    {
        auto invalidIR = lift(Luau::compile("local function named(n) return n+1 end return named(2)", options));
        invalidIR.functions.at(1).debugname = invalid;
        auto fallback = printAST(buildAST(buildSSA(buildCFG(std::move(invalidIR)))));
        check(fallback.find("local function f_0(") != std::string::npos, "invalid debugnames need a valid generated name");
        check(evaluate(fallback) == evaluate("return 3"), "invalid debugname fallback must compile and execute");
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
        roundTrips(); branchRecovery(); stagesAndNames(); inliningAndDebugNames(); callbackInlining(); argumentAliases(); singleUseCallAndStoreValues();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
