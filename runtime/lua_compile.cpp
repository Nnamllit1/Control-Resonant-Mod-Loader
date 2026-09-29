#include "lua_compile.h"
#include "lua_controller.h"
#include <Luau/Compiler.h>
#include <Windows.h>

namespace crml::engine::lua {
Compilation compile_source(const std::string& source) {
    if(source.size()>max_source_bytes) return {{},"source_too_large"};
    if(!source.empty() && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,source.data(),static_cast<int>(source.size()),nullptr,0))
        return {{},"invalid_utf8"};
    Luau::CompileOptions options;options.optimizationLevel=0;options.debugLevel=2;
    try {
        const bool bom=source.size()>=3 && source.compare(0,3,"\xef\xbb\xbf")==0;
        const auto bytes=Luau::compile(bom?source.substr(3):source,options);
        if(!bytes.empty() && !bytes[0]) {
            Compilation result{{},"compile_error"};
            if(bytes.size()>2 && bytes[1]==':') {
                size_t at=2;unsigned line=0;
                while(at<bytes.size() && at<10 && bytes[at]>='0' && bytes[at]<='9') line=line*10+unsigned(bytes[at++]-'0');
                if(at<bytes.size() && bytes[at]==':') result.line=line;
            }
            return result;
        }
        if(bytes.size()<3 || bytes.size()>ControllerHost::max_program_bytes || bytes[0]!=6 || bytes[1]!=3)
            return {{},"unsupported_bytecode"};
        return {{bytes.begin(),bytes.end()}};
    } catch(const std::exception&) {return {{},"compiler_failure"};}
}
}
